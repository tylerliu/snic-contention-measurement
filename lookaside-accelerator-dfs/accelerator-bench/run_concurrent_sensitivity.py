#!/usr/bin/env python3
import argparse
import csv
import itertools
import json
import os
import subprocess
import sys
import time
import shlex
from datetime import datetime
from typing import List, Dict, Any

from bench_utils import (
    build_ssh_command, 
    parse_network_stats, 
    parse_crypto_stats,
    parse_command_stats,
    build_executable_path,
    verify_arp_resolution,
    is_generator_started
)

def expand_tilde(cmd_str):
    if "SUDO_USER" in os.environ and cmd_str.startswith("~/"):
        return f"/home/{os.environ['SUDO_USER']}/" + cmd_str[2:]
    return os.path.expanduser(cmd_str)

def split_command(cmd_spec):
    if isinstance(cmd_spec, list):
        return [expand_tilde(str(arg)) for arg in cmd_spec]
    return [expand_tilde(arg) for arg in shlex.split(str(cmd_spec))]

def add_line_buffering(cmd):
    cmd = list(cmd)
    if cmd[0] == 'sudo':
        cmd.insert(1, 'stdbuf')
        cmd.insert(2, '-oL')
    else:
        cmd.insert(0, 'stdbuf')
        cmd.insert(1, '-oL')
    return cmd

def main():
    parser = argparse.ArgumentParser(description="Run concurrent sensitivity benchmarks across multiple machines via SSH")
    parser.add_argument('--config', type=str, default="concurrent_sensitivity_cases.json", help="Concurrent config JSON")
    parser.add_argument('--test', type=str, default="XTS-Decrypt-Network-Load", help="Test to run from config")
    parser.add_argument('--crypto-cases', type=str, default="loaded_cases.json", help="Crypto cases JSON")
    parser.add_argument('--network-cases', type=str, default="network_throughput_cases.json", help="Network cases JSON")
    parser.add_argument('--csv', type=str, default="", help="Output CSV file path")
    parser.add_argument('--dry-run', action='store_true', help="Print commands without executing")
    parser.add_argument('--verbose', action='store_true', help="Verbose output")

    args = parser.parse_args()

    # Load configurations
    if not os.path.exists(args.config):
        print(f"Error: Config {args.config} not found.")
        sys.exit(1)
        
    with open(args.config, 'r') as f:
        full_config = json.load(f)
        if args.test not in full_config:
            print(f"Error: Test '{args.test}' not found in {args.config}")
            sys.exit(1)
        config = full_config[args.test]

    # Load standalone cases for base commands/params
    crypto_cases = {}
    if os.path.exists(args.crypto_cases):
        with open(args.crypto_cases, 'r') as f:
            crypto_cases = json.load(f)

    network_cases = {}
    if os.path.exists(args.network_cases):
        with open(args.network_cases, 'r') as f:
            network_cases = json.load(f)

    duration = config.get('duration', 10)
    jobs_config = config['jobs']

    # 1. Identify all parameters that will be varied
    param_names = []
    param_values_list = []
    
    for job in jobs_config:
        overrides = job.get('params_override', {})
        for param_name, values in overrides.items():
            param_names.append(f"{job['name']}_{param_name}")
            param_values_list.append(values)

    # Generate Cartesian product of all parameters
    all_combinations = list(itertools.product(*param_values_list))

    # 2. Setup CSV
    if not args.csv:
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        args.csv = f"concurrent_results_{args.test}_{timestamp}.csv"

    csv_headers = param_names[:]
    for job in jobs_config:
        if job['type'] == 'crypto':
            csv_headers.extend([f"{job['name']}_ops", f"{job['name']}_cycles", f"{job['name']}_wait_cycles", f"{job['name']}_ops_sec"])
        elif job['type'] == 'network':
            csv_headers.extend([f"{job['name']}_tx_pps", f"{job['name']}_tx_mbps", f"{job['name']}_rx_pps", f"{job['name']}_rx_mbps", f"{job['name']}_paused_cycles"])
        elif job['type'] == 'command':
            csv_headers.extend([f"{job['name']}_device_mbps", f"{job['name']}_avg_time_s", f"{job['name']}_samples"])

    if not args.dry_run:
        csv_file = open(args.csv, 'w', newline='')
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow(csv_headers)
    else:
        print(f"Would write CSV with headers: {csv_headers}")

    # 3. Execution Loop
    for combo in all_combinations:
        print(f"\n>>> Running combination: {dict(zip(param_names, combo))}")
        
        # Map values back to jobs
        job_params = []
        val_idx = 0
        for job in jobs_config:
            overrides = job.get('params_override', {})
            current_job_params = {}
            for param_name in overrides:
                current_job_params[param_name] = combo[val_idx]
                val_idx += 1
            job_params.append(current_job_params)

        processes = []
        job_outputs = {job['name']: "" for job in jobs_config}
        network_receiver_outputs = {}
        
        # 3a. Launch and Verify Network Jobs First
        for i, job in enumerate(jobs_config):
            if job['type'] != 'network':
                continue
                
            host = job.get('host', 'localhost')
            params = job_params[i]
            case_id = job['case']
            
            if case_id not in network_cases:
                print(f"Error: Network case {case_id} not found.")
                sys.exit(1)
            
            case_cfg = network_cases[case_id]
            receiver_names = []
            for receiver_idx, receiver_cfg in enumerate(case_cfg.get('receiver_cmds', [])):
                if isinstance(receiver_cfg, dict):
                    receiver_cmd_spec = receiver_cfg['cmd']
                    receiver_host = receiver_cfg.get('host', host)
                    receiver_name = receiver_cfg.get('name', f"{job['name']}_receiver_{receiver_idx}")
                else:
                    receiver_cmd_spec = receiver_cfg
                    receiver_host = host
                    receiver_name = f"{job['name']}_receiver_{receiver_idx}"

                receiver_cmd = add_line_buffering(split_command(receiver_cmd_spec))
                receiver_final_cmd = build_ssh_command(receiver_cmd, host=receiver_host, timeout_secs=duration + 30)
                receiver_names.append(receiver_name)
                job_outputs[receiver_name] = ""

                print(f"  Starting network receiver '{receiver_name}' on {receiver_host}: {' '.join(receiver_final_cmd)}")
                if not args.dry_run:
                    p = subprocess.Popen(receiver_final_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                    processes.append({
                        'name': receiver_name,
                        'type': 'network_receiver',
                        'proc': p,
                        'host': receiver_host
                    })

            network_receiver_outputs[job['name']] = receiver_names
            if receiver_names and not args.dry_run:
                time.sleep(1)

            cmd = add_line_buffering(split_command(case_cfg['cmd']))
                
            throttle = params.get('throttles', 0)
            cmd.extend(["-t", str(throttle)])
            
            # Final command with timeout
            # Increase timeout significantly to account for ARP verification and crypto SSH startup delays
            final_cmd = build_ssh_command(cmd, host=host, timeout_secs=duration + 30)
            
            # Retry loop for ARP resolution
            success = False
            for attempt in range(1, 4):
                if attempt > 1:
                    print(f"  Attempt {attempt} for network job '{job['name']}'...")
                
                print(f"  Starting network job '{job['name']}' on {host}: {' '.join(final_cmd)}")
                if args.dry_run:
                    success = True
                    break
                    
                p = subprocess.Popen(final_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                
                # Monitor for ARP resolution / startup
                # We'll read lines until we see 'TOTAL TX:' or an ARP failure
                print(f"    Verifying ARP resolution...")
                start_verify = time.time()
                arp_resolved = False
                while time.time() - start_verify < 20: # 20s timeout for ARP
                    line = p.stdout.readline()
                    if not line:
                        break
                    job_outputs[job['name']] += line
                    if is_generator_started(line):
                        arp_resolved = True
                        break
                    if verify_arp_resolution(line):
                        print(f"    [!] ARP resolution failed on attempt {attempt}")
                        break
                
                if arp_resolved:
                    print(f"    [+] ARP resolution successful.")
                    processes.append({
                        'name': job['name'],
                        'type': 'network',
                        'proc': p,
                        'host': host
                    })
                    success = True
                    break
                else:
                    # Kill and retry
                    p.terminate()
                    p.wait()
                    time.sleep(1)
            
            if not success and not args.dry_run:
                print(f"Error: Network job '{job['name']}' failed to resolve ARP after 3 attempts. Aborting.")
                # Kill any other processes already started
                for p_dict in processes:
                    p_dict['proc'].terminate()
                sys.exit(1)

        # 3b. Launch Crypto and Generic Command Jobs
        for i, job in enumerate(jobs_config):
            if job['type'] not in ('crypto', 'command'):
                continue
                
            host = job.get('host', 'localhost')
            params = job_params[i]

            if job['type'] == 'command':
                raw_cmd = job.get('cmd')
                if not raw_cmd:
                    print(f"Error: Command job '{job['name']}' is missing 'cmd'.")
                    sys.exit(1)

                cmd = split_command(raw_cmd)

                for k, v in params.items():
                    val = v[0] if isinstance(v, list) else v
                    cmd.extend([f"--{k}", str(val)])

                timeout_secs = job.get('timeout_secs', duration + 30)
                stop_signal = job.get('stop_signal', 'SIGINT')
                final_cmd = build_ssh_command(cmd, host=host, timeout_secs=timeout_secs, signal=stop_signal)

                print(f"  Starting command job '{job['name']}' on {host}: {' '.join(final_cmd)}")
                if not args.dry_run:
                    p = subprocess.Popen(final_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                    processes.append({
                        'name': job['name'],
                        'type': 'command',
                        'proc': p,
                        'host': host
                    })
                continue

            case_id = job['case']
            
            if case_id not in crypto_cases:
                print(f"Error: Crypto case {case_id} not found.")
                continue
            
            case_cfg = crypto_cases[case_id]
            
            # Path resolution
            home = os.path.expanduser("~")
            if "SUDO_USER" in os.environ:
                home = f"/home/{os.environ['SUDO_USER']}"
            rel_path = os.path.relpath(os.getcwd(), home)
            
            if host != 'localhost':
                exe = os.path.join(".", rel_path, "build", case_id)
            else:
                exe = os.path.join("build", case_id)
            
            cmd = ["taskset", "-c", str(job.get('core', 3)), exe]
            eal_args = case_cfg.get('eal_args', [])
            cmd.extend(eal_args)
            cmd.append("--")
            
            all_params = {**case_cfg.get('params', {}), **params}
            for k, v in all_params.items():
                val = v[0] if isinstance(v, list) else v
                cmd.extend([f"--{k}", str(val)])
            
            cmd.extend(["--duration", str(duration)])
            final_cmd = build_ssh_command(cmd, host=host)
            
            print(f"  Starting crypto job '{job['name']}' on {host}: {' '.join(final_cmd)}")
            if not args.dry_run:
                p = subprocess.Popen(final_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                processes.append({
                    'name': job['name'],
                    'type': 'crypto',
                    'proc': p,
                    'host': host
                })

        if args.dry_run:
            continue

        # 3c. Monitor and Wait
        # Use background threads to read output and avoid pipe blocking
        import threading
        
        # We'll store a marker for where the 'post-verification' output begins
        verification_output_lens = {name: len(job_outputs[name]) for name in job_outputs}
        
        def pipe_reader(p_dict):
            name = p_dict['name']
            proc = p_dict['proc']
            # Read until EOF
            for line in iter(proc.stdout.readline, ''):
                job_outputs[name] += line

        reader_threads = []
        for p_dict in processes:
            t = threading.Thread(target=pipe_reader, args=(p_dict,), daemon=True)
            t.start()
            reader_threads.append(t)

        print(f"  Waiting {duration}s for jobs to complete...")
        time.sleep(duration + 2)

        import signal
        # Ensure all processes are finished or timeout
        for p_dict in processes:
            name = p_dict['name']
            
            if p_dict['type'] in ('network', 'network_receiver', 'command'):
                print(f"  Sending SIGINT to {p_dict['type']} job '{name}' to stop it...")
                p_dict['proc'].send_signal(signal.SIGINT)
                
            try:
                # wait() since the reader threads are already consuming output
                p_dict['proc'].wait(timeout=5)
            except subprocess.TimeoutExpired:
                print(f"  Warning: Job {name} timed out, killing...")
                p_dict['proc'].kill()
                p_dict['proc'].wait()
            
            if args.verbose:
                print(f"--- Raw output for {name} ---")
                print(job_outputs[name])
                print("-----------------------------")

        # 4. Parse Results
        results_row = list(combo)
        for job in jobs_config:
            name = job['name']
            full_output = job_outputs[name]
            # Output from after verification/crypto start
            post_verify_output = full_output[verification_output_lens[name]:]
            
            if job['type'] == 'crypto':
                stats = parse_crypto_stats(full_output)
                ops = stats.get('total_ops', 0)
                cycles = stats.get('total_cycles', 0)
                wait = stats.get('wait_cycles', 0)
                
                if ops == 0:
                    print(f"  Warning: No crypto operations parsed for job '{name}'")
                    if not args.verbose:
                        print("  Raw output:")
                        print(full_output)
                
                ops_sec = ops / duration if duration > 0 else 0
                results_row.extend([ops, cycles, wait, ops_sec])
                
                if args.verbose:
                    print(f"  [{name}] Ops: {ops}, Cycles: {cycles}, Ops/Sec: {ops_sec:,.0f}")
                
            elif job['type'] == 'network':
                # Parse specifically from the post-verify output to get samples after crypto start
                # We want the 5th sample after crypto start
                stats = parse_network_stats(post_verify_output, sample_index=5)
                for receiver_name in network_receiver_outputs.get(name, []):
                    receiver_output = job_outputs[receiver_name][verification_output_lens[receiver_name]:]
                    receiver_stats = parse_network_stats(receiver_output, sample_index=5)
                    if 'rx_pps' in receiver_stats:
                        stats['rx_pps'] = receiver_stats['rx_pps']
                    if 'rx_mbps' in receiver_stats:
                        stats['rx_mbps'] = receiver_stats['rx_mbps']

                tx_pps = stats.get('tx_pps', 0)
                tx_mbps = stats.get('tx_mbps', 0)
                rx_pps = stats.get('rx_pps', 0)
                rx_mbps = stats.get('rx_mbps', 0)
                paused = stats.get('cycles_paused_tx', 0)
                results_row.extend([tx_pps, tx_mbps, rx_pps, rx_mbps, paused])
                
                if args.verbose:
                    print(f"  [{name}] TX: {tx_mbps} MB/s, RX: {rx_mbps} MB/s, Paused: {paused}")

            elif job['type'] == 'command':
                stats = parse_command_stats(full_output)
                device_mbps = stats.get('device_mbps', 0)
                avg_time_s = stats.get('avg_time_s', 0)
                samples = stats.get('samples', 0)

                if samples == 0:
                    print(f"  Warning: No command throughput parsed for job '{name}'")
                    if not args.verbose:
                        print("  Raw output:")
                        print(full_output)

                results_row.extend([device_mbps, avg_time_s, samples])

                if args.verbose:
                    print(f"  [{name}] Device throughput: {device_mbps} MB/s, Avg Time: {avg_time_s} s, Samples: {samples}")

        if not args.dry_run:
            csv_writer.writerow(results_row)
            csv_file.flush()

    if not args.dry_run:
        csv_file.close()
        print(f"\nDone! Results saved to {args.csv}")

if __name__ == "__main__":
    main()

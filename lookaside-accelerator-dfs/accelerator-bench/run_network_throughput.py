#!/usr/bin/env python3
import argparse
import csv
import json
import os
import re
import shlex
import signal
import subprocess
import sys
import time
from datetime import datetime

def main():
    parser = argparse.ArgumentParser(description="Run network throughput sensitivity experiment")
    parser.add_argument('--cases-file', type=str, default="network_throughput_cases.json", help="JSON file with test cases")
    parser.add_argument('--test', type=str, default="Host-to-Net-1472B", help="Name of the test to run from cases file")
    parser.add_argument('--csv', type=str, default="", help="Output CSV file path")
    parser.add_argument('--dry-run', action='store_true', help="Print commands without executing")

    args = parser.parse_args()

    if not os.path.exists(args.cases_file):
        print(f"Error: Cases file {args.cases_file} not found.")
        sys.exit(1)

    with open(args.cases_file, 'r') as f:
        cases = json.load(f)

    if args.test not in cases:
        print(f"Error: Test {args.test} not found in {args.cases_file}")
        sys.exit(1)

    base_cmd_str = cases[args.test].get("cmd", "")
    if not base_cmd_str:
        print(f"Error: No 'cmd' specified for test {args.test}")
        sys.exit(1)

    throttle_list = cases[args.test].get("throttles", [0, 10, 50, 100, 500, 1000, 5000, 10000])

    # Expand tilde in the command
    if "SUDO_USER" in os.environ and base_cmd_str.startswith("~/"):
        base_cmd_str = f"/home/{os.environ['SUDO_USER']}/" + base_cmd_str[2:]
    else:
        base_cmd_str = os.path.expanduser(base_cmd_str)
        
    base_cmd = shlex.split(base_cmd_str)

    # Make sure we use sudo
    if base_cmd[0] != "sudo":
        base_cmd = ["sudo", "stdbuf", "-oL"] + base_cmd
    else:
        base_cmd.insert(1, "stdbuf")
        base_cmd.insert(2, "-oL")

    csv_path = args.csv
    if not csv_path:
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        csv_path = f"network_throughput_{args.test}_{timestamp}.csv"

    csv_file = None
    csv_writer = None
    if not args.dry_run:
        csv_file = open(csv_path, mode='w', newline='')
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow(['test', 'throttle', 'tx_pps', 'tx_mbps', 'rx_pps', 'rx_mbps', 'cycles_paused_tx'])

    tx_regex = re.compile(r"TOTAL TX:\s*([0-9.]+)\s*pps,\s*([0-9.]+)\s*MB/s")
    rx_regex = re.compile(r"TOTAL RX:\s*([0-9.]+)\s*pps,\s*([0-9.]+)\s*MB/s")
    pause_regex = re.compile(r"Cycles paused \(TX\):\s*([0-9]+)")

    from bench_utils import verify_arp_resolution, is_generator_started

    for throttle in throttle_list:
        print(f"\n--- Running test '{args.test}' with throttle={throttle} ---")
        
        cmd = base_cmd + ["-t", str(throttle)]
        
        if args.dry_run:
            print(f"Would execute: {' '.join(cmd)}")
            continue

        # Retry loop for ARP resolution
        success = False
        for attempt in range(1, 4):
            if attempt > 1:
                print(f"  Attempt {attempt}...")
            
            print(f"Executing: {' '.join(cmd)}")
            process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            
            tx_samples = []
            rx_samples = []
            pause_samples = []
            arp_failed = False
            generator_started = False
            
            try:
                for line in iter(process.stdout.readline, ''):
                    sys.stdout.write(line)
                    sys.stdout.flush()
                    
                    if not generator_started:
                        if verify_arp_resolution(line):
                            print(f"\n[!] ARP resolution failed on attempt {attempt}")
                            arp_failed = True
                            break
                        if is_generator_started(line):
                            generator_started = True

                    tx_match = tx_regex.search(line)
                    if tx_match:
                        tx_samples.append((float(tx_match.group(1)), float(tx_match.group(2))))
                        
                    rx_match = rx_regex.search(line)
                    if rx_match:
                        rx_samples.append((float(rx_match.group(1)), float(rx_match.group(2))))
                    
                    pause_match = pause_regex.search(line)
                    if pause_match:
                        pause_samples.append(int(pause_match.group(1)))
                        
                        if len(pause_samples) >= 5:
                            process.send_signal(signal.SIGINT)
                            success = True
                            break
                
                # Wait for the process to terminate
                process.communicate(timeout=5)
                
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
            except KeyboardInterrupt:
                process.send_signal(signal.SIGINT)
                process.communicate()
                print("Interrupted by user. Exiting.")
                sys.exit(0)
            
            if success:
                break
            elif arp_failed:
                print(f"  Restarting process due to ARP failure...")
                time.sleep(1)
            else:
                print(f"  Process exited unexpectedly or timed out.")
                break
                
        if not success:
            print(f"Error: Failed to collect samples for throttle {throttle} after retries. Aborting.")
            sys.exit(1)
            
        if len(tx_samples) >= 5 and len(rx_samples) >= 5 and len(pause_samples) >= 5:
            # Take the 5th sample (index 4)
            tx_pps, tx_mbps = tx_samples[4]
            rx_pps, rx_mbps = rx_samples[4]
            cycles_paused = pause_samples[4]
            
            print(f"Captured 5th Sample -> TX: {tx_pps} pps, {tx_mbps} MB/s | RX: {rx_pps} pps, {rx_mbps} MB/s | Paused: {cycles_paused}")
            
            if csv_writer:
                csv_writer.writerow([args.test, throttle, tx_pps, tx_mbps, rx_pps, rx_mbps, cycles_paused])
                csv_file.flush()
        else:
            print("Error: Did not collect enough samples before process exited.")

    if csv_file:
        csv_file.close()
        print(f"\nResults saved to {csv_path}")

if __name__ == "__main__":
    main()

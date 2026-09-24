#!/usr/bin/env python3
import argparse
import csv
import json
import os
import sys
from datetime import datetime
from bench_utils import BenchmarkRunner, build_executable_path, run_setup_command, detect_tsc_hz, _parse_cycles, _parse_metadata, _parse_wait_cycles, _parse_operations
import subprocess
import itertools

def main():
    parser = argparse.ArgumentParser(description="Run loaded throughput benchmark for specific functions")
    parser.add_argument('--functions', nargs='*', default=[], help="Benchmark functions to run. Runs all in JSON if empty.")
    parser.add_argument('--cases-file', type=str, default="loaded_cases.json", help="JSON file with test cases")
    parser.add_argument('--duration', '-d', type=int, default=5, help="Duration in seconds to run each benchmark")
    parser.add_argument('--cpu-core', type=int, default=3, help="CPU core to pin to")
    parser.add_argument('--build-dir', type=str, default='build', help="Meson build directory")
    parser.add_argument('--csv', type=str, default='', help="Output CSV file path")
    parser.add_argument('--dry-run', action='store_true', help="Print commands instead of executing")
    parser.add_argument('--verbose', action='store_true', help="Verbose output")

    args = parser.parse_args()

    if not os.path.exists(args.cases_file):
        print(f"Error: Cases file {args.cases_file} not found.", file=sys.stderr)
        sys.exit(1)

    with open(args.cases_file, 'r') as f:
        benchmark_cases = json.load(f)

    function_names = list(benchmark_cases.keys()) if len(args.functions) == 0 else args.functions

    runner = BenchmarkRunner(cpu_core=args.cpu_core, verbose=args.verbose)
    if not args.dry_run:
        runner.setup_cpu()

    csv_path = args.csv
    if not csv_path:
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        csv_path = f"loaded_throughput_{timestamp}.csv"

    csv_file = None
    csv_writer = None
    if not args.dry_run:
        csv_file = open(csv_path, mode='w', newline='')
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow(['function', 'pause_loops', 'duration', 'total_cycles', 'wait_cycles', 'total_operations', 'cycles_per_op', 'wait_cycles_per_op', 'ops_per_sec', 'metadata'])

    setup_ran_for = set()

    for func in function_names:
        if func not in benchmark_cases:
            print(f"Error: Function {func} not found in {args.cases_file}", file=sys.stderr)
            continue

        bench_cfg = benchmark_cases[func]
        eal_args = bench_cfg.get('eal_args', [])
        setup_command = bench_cfg.get('setup_command', None)
        params_dict = bench_cfg.get('params', {})
        grouped_params = bench_cfg.get('grouped_params', {})

        exe_path = build_executable_path(args.build_dir, func)
        if not os.path.exists(exe_path) and not args.dry_run:
            print(f"Executable {exe_path} not found. Skipping.", file=sys.stderr)
            continue

        if setup_command and not args.dry_run and func not in setup_ran_for:
            if not run_setup_command(setup_command):
                print(f"Setup command failed for {func}", file=sys.stderr)
                continue
            setup_ran_for.add(func)

        tsc_hz = detect_tsc_hz(args.build_dir, eal_args, runner.cpu_core) if not args.dry_run else None

        param_keys = list(params_dict.keys())
        param_values = [params_dict[k] for k in param_keys]

        combos_to_run = []
        if grouped_params:
            for group_name, group_list in grouped_params.items():
                for group_item in group_list:
                    for combo in itertools.product(*param_values):
                        combos_to_run.append((combo, group_item))
        else:
            for combo in itertools.product(*param_values):
                combos_to_run.append((combo, None))

        # Warm-up using the first combination with pause_loops set to 0 if present
        if not args.dry_run and combos_to_run:
            first_combo, first_group = combos_to_run[0]
            warmup_args = eal_args + ['--']
            for i, key in enumerate(param_keys):
                val = "0" if key == "pause_loops" else str(first_combo[i])
                warmup_args.extend([f"--{key}", val])
            if first_group:
                for k, v in first_group.items():
                    if v is not None:
                        val = "0" if k == "pause_loops" else str(v)
                        warmup_args.extend([f"--{k}", val])
            runner.warm_up(exe_path, warmup_args)

        for combo, group_item in combos_to_run:
            benchmark_args = []
            metadata_params = {}
            for i, key in enumerate(param_keys):
                benchmark_args.extend([f"--{key}", str(combo[i])])
                metadata_params[key] = combo[i]
            
            if group_item:
                for key, value in group_item.items():
                    if value is not None:
                        benchmark_args.extend([f"--{key}", str(value)])
                        metadata_params[key] = value

            benchmark_args.extend(['-d', str(args.duration)])
            
            pause_loops_val = metadata_params.get("pause_loops", 0)

            print(f"\n--- Running {func} (pause_loops={pause_loops_val}) ---")
            
            cmd_args = eal_args + ['--'] + benchmark_args
            full_cmd = ["taskset", "-c", str(runner.cpu_core), exe_path] + cmd_args

            if args.dry_run:
                print(f"Would execute: {' '.join(full_cmd)}")
                continue

            try:
                res = subprocess.run(full_cmd, capture_output=True, text=True, check=True)
                stdout = res.stdout
                
                total_cycles = _parse_cycles(stdout)
                wait_cycles = _parse_wait_cycles(stdout)
                metadata = _parse_metadata(stdout)
                
                total_ops = _parse_operations(stdout)
                
                if total_cycles is not None and total_ops is not None:
                    cycles_per_op = total_cycles / total_ops if total_ops > 0 else 0
                    wait_cycles_per_op = (wait_cycles or 0) / total_ops if total_ops > 0 else 0
                    ops_per_sec = 0
                    if tsc_hz and cycles_per_op > 0:
                        ops_per_sec = (1.0 / cycles_per_op) * tsc_hz

                    print(f"  Total Cycles: {total_cycles:.0f}")
                    print(f"  Wait Cycles:  {wait_cycles:.0f}")
                    print(f"  Total Ops:    {total_ops}")
                    print(f"  Cycles/Op:    {cycles_per_op:.2f}")
                    print(f"  Wait/Op:      {wait_cycles_per_op:.2f}")
                    print(f"  Ops/Sec:      {ops_per_sec:,.0f}")
                    
                    if csv_writer:
                        csv_writer.writerow([
                            func, pause_loops_val, args.duration, total_cycles, wait_cycles, total_ops,
                            cycles_per_op, wait_cycles_per_op, ops_per_sec, 
                            json.dumps(metadata)
                        ])
                        csv_file.flush()
                else:
                    print("Error: Could not parse cycles from output.")
                    print("Stdout:")
                    print(stdout)
                    
            except subprocess.CalledProcessError as e:
                print(f"Error running benchmark: {e}")
                print(e.stderr)

    if csv_file:
        csv_file.close()
        print(f"\nResults saved to {csv_path}")

if __name__ == '__main__':
    main()

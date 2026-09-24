import argparse
import os
import shlex
import subprocess
import sys
import csv
import re
import json
import itertools
import signal
import atexit
from datetime import datetime

# Import shared utilities
from bench_utils import (
    BenchmarkRunner,
    detect_and_explain_common_errors,
    build_executable_path,
    _parse_cycles,
    _parse_metadata,
    run_setup_command,
    check_permissions,
    detect_tsc_hz,
)


## Removed local BenchmarkRunner and helpers in favor of bench_utils


## Helpers imported from bench_utils

## build_executable_path imported from bench_utils


def discover_functions(build_dir: str) -> list[str]:
    if not os.path.isdir(build_dir):
        return []
    discovered: list[str] = []
    try:
        for entry in os.listdir(build_dir):
            if entry.startswith('.'):
                continue
            # In simplified mode we consider all executables present
            full = os.path.join(build_dir, entry)
            if not os.path.isfile(full):
                continue
            if not os.access(full, os.X_OK):
                continue
            discovered.append(entry)
    except FileNotFoundError:
        return []

    # Run 'empty' first if present
    discovered_sorted = sorted([f for f in discovered if f != 'empty'])
    if 'empty' in discovered:
        return ['empty'] + discovered_sorted
    return discovered_sorted


def get_benchmark_config(full_config, func):
    # Deprecated in simplified mode; directly use provided config
    return full_config.get(func, {})

def run_benchmark(function_name: str, build_dir: str, runner: BenchmarkRunner, cmd_args: list[str], env: dict[str, str] | None = None, case_info: str | None = None, dry_run: bool = False, setup_command: str | None = None) -> tuple[int, float | None, dict, str, str]:
    exe_path = build_executable_path(build_dir, function_name)

    if not os.path.exists(exe_path):
        print(f"Executable not found: {exe_path}", file=sys.stderr)
        print("Make sure you have built the project with Meson before running this script.", file=sys.stderr)
        return 1, None, {}, "", ""

    # Run setup command first (if provided)
    if setup_command and not run_setup_command(setup_command, dry_run):
        print(f"Setup command failed for {function_name}", file=sys.stderr)
        return 1, None, {}, "", ""

    # Warm up the executable first (skip in dry-run mode)
    if not dry_run and not runner.warm_up(exe_path, cmd_args):
        print(f"Warning: Warm-up failed for {function_name}", file=sys.stderr)

    # Use taskset to pin to specific CPU core for consistent measurements
    cmd = ["taskset", "-c", str(runner.cpu_core), exe_path] + cmd_args

    case_suffix = f" ({case_info})" if case_info else ""
    print(f"\n--- Running benchmark for {function_name}{case_suffix} ---")
    print("Command:", " ".join(shlex.quote(part) for part in cmd))

    if dry_run:
        print("DRY RUN: Would execute the above command")
        return 0, None, {}, "", ""

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    except FileNotFoundError:
        print(f"Failed to execute: {exe_path} (file not found)", file=sys.stderr)
        return 1, None, {}, "", ""

    if result.returncode != 0:
        print("Error running benchmark:", file=sys.stderr)
        if result.stdout:
            print(result.stdout, file=sys.stderr)
        if result.stderr:
            print(result.stderr, file=sys.stderr)
        detect_and_explain_common_errors(result.stdout or "", result.stderr or "")
        return result.returncode, None, {}, result.stdout, result.stderr
    else:
        print(result.stdout.strip())
        cycles = _parse_cycles(result.stdout or "")
        metadata = _parse_metadata(result.stdout or "")
        return 0, cycles, metadata, result.stdout, result.stderr


## check_permissions imported from bench_utils


if __name__ == '__main__':
    # Manually split sys.argv on '--' so that everything after it is passed to the executable
    parser = argparse.ArgumentParser(
        description='Run pre-built API benchmarks with CPU optimizations for accurate measurements.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                                    # Run all benchmarks with optimizations
  %(prog)s --verbose                          # Run with detailed output
  %(prog)s --cpu-core 2 --iterations 5000000 # Use specific CPU core and iterations
  %(prog)s rte_eth_rx_burst                   # Run specific function
  %(prog)s --dry-run                          # Show commands without executing them

Optimizations applied:
  - CPU governor set to performance mode
  - CPU idle states disabled during benchmarking
  - Process pinned to specific CPU core using taskset
  - Warm-up runs to ensure consistent cache state
  - Proper cleanup on interruption (Ctrl+C)
  - Permission checks for DPDK requirements
        """
    )
    parser.add_argument('functions', nargs='*', help='Functions to benchmark (defaults to all functions in benchmark_cases.json).')
    parser.add_argument('--build-dir', default='build', help='Meson build directory containing benchmark executables (function names as executables).')
    parser.add_argument('-i', '--iterations', type=int, default=1000000, help='Number of iterations for benchmarks (default: 1000000)')
    parser.add_argument('--csv', default=None, help='Path to CSV file for results. If omitted, a timestamped file is created in the current directory.')
    parser.add_argument('--cpu-core', type=int, default=3, help='CPU core to pin benchmarks to (default: 3)')
    parser.add_argument('--verbose', action='store_true', help='Enable verbose output showing detailed setup and warm-up information')
    parser.add_argument('--dry-run', action='store_true', help='Show commands that would be executed without actually running them')

    args = parser.parse_args()

    # Check permissions first
    check_permissions()

    # Create benchmark runner
    runner = BenchmarkRunner(cpu_core=args.cpu_core, verbose=args.verbose)
    
    # Set up CPU for optimal benchmarking
    # Set up CPU for benchmarking (skip in dry-run mode)
    if not args.dry_run:
        runner.setup_cpu()

    # Sudo warning (no elevation)
    if hasattr(os, 'geteuid') and os.geteuid() != 0:
        print("Warning: Not running as root. Many DPDK/cryptodev setups require root/sudo. This script will not elevate.", file=sys.stderr)

    exit_code = 0

    # CSV setup (skip in dry-run mode)
    csv_file = None
    csv_writer = None
    if not args.dry_run:
        csv_path = args.csv
        if not csv_path:
            timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
            csv_path = f"api_perf_results_{timestamp}.csv"
        csv_file = open(csv_path, mode='w', newline='')
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow(['function', 'iterations', 'total_cycles', 'metadata'])

    # Load benchmark cases from JSON
    with open('benchmark_cases.json', 'r') as f:
        benchmark_cases = json.load(f)

    function_names = list(benchmark_cases.keys()) if len(args.functions) == 0 else args.functions

    # Optional: detect TSC using get_tsc_hz if present
    def detect_tsc_hz_local(eal_args):
        return detect_tsc_hz(args.build_dir, eal_args, runner.cpu_core)

    # Now run all other benchmarks
    setup_ran_for: set[str] = set()

    def ensure_setup_once(ran_set, key, command) -> bool:
        """Run setup command once per key. Returns True if ok or nothing to do."""
        if not command:
            return True
        if key in ran_set:
            return True
        ok_local = run_setup_command(command, args.dry_run)
        if ok_local:
            ran_set.add(key)
        return ok_local
    for func in function_names:
        bench_cfg = benchmark_cases.get(func)
        if bench_cfg is None:
            print(f"Unknown function '{func}' (not in benchmark_cases.json)", file=sys.stderr)
            continue
        params_dict = bench_cfg.get("params", {})
        grouped_params = bench_cfg.get("grouped_params", {})
        eal_args = bench_cfg.get("eal_args", [])
        setup_command = bench_cfg.get("setup_command", None)
        
        # Run setup command once per function before running all cases
        if setup_command:
            if not ensure_setup_once(setup_ran_for, func, setup_command):
                print(f"Setup command failed for {func}", file=sys.stderr)
                exit_code = 1
                # Skip running cases for this benchmark on failure
                continue
        
        # Generate parameter combinations
        def run_benchmark_case(regular_params: dict, grouped_params: dict = None):
            """Run a benchmark case with the given parameter combinations."""
            param_keys = list(regular_params.keys())
            param_values = [regular_params[k] for k in param_keys]
            
            if grouped_params:
                # Handle grouped parameters
                for group_name, group_list in grouped_params.items():
                    for group_item in group_list:
                        # Combine regular parameters with grouped parameters
                        for combo in itertools.product(*param_values):
                            yield _build_and_run_benchmark_case(
                                param_keys, combo, group_item, eal_args, args.iterations,
                                func, runner, csv_writer, args.dry_run, tsc_hz
                            )
            else:
                # Handle regular parameters only
                for combo in itertools.product(*param_values):
                    yield _build_and_run_benchmark_case(
                        param_keys, combo, None, eal_args, args.iterations,
                        func, runner, csv_writer, args.dry_run, tsc_hz
                    )
        
        def _build_and_run_benchmark_case(param_keys, combo, group_item, eal_args, iterations, func, runner, csv_writer, dry_run, tsc_hz):
            """Build and run a single benchmark case."""
            # Build benchmark (post --) args: params then iterations
            benchmark_args: list[str] = []
            case_info_parts = []
            metadata_params = {}
            
            # Add regular parameters
            for i, key in enumerate(param_keys):
                benchmark_args.extend([f"--{key}", str(combo[i])])
                case_info_parts.append(f"{key}={combo[i]}")
                metadata_params[key] = combo[i]
            
            # Add grouped parameters if provided
            if group_item:
                for key, value in group_item.items():
                    if value is not None:  # Skip null values
                        benchmark_args.extend([f"--{key}", str(value)])
                        case_info_parts.append(f"{key}={value}")
                        metadata_params[key] = value
            
            benchmark_args.extend(['-i', str(iterations)])

            # Full command: EAL args first, then '--', then benchmark args
            cmd_args = eal_args + ['--'] + benchmark_args
            case_info = ", ".join(case_info_parts) if case_info_parts else "Default"
            
            # Set up environment
            env = os.environ.copy()
            
            # Executable naming: function name equals executable name in simplified mode
            rc, cycles, metadata, stdout, stderr = run_benchmark(
                func, build_dir=args.build_dir, runner=runner, 
                cmd_args=cmd_args, env=env, case_info=case_info, dry_run=dry_run, 
                setup_command=None
            )
            
            if cycles is not None:
                total_cycles = cycles
                # Try to infer burst size from params
                burst_size_value = None
                if 'burst_size' in metadata_params:
                    burst_size_value = metadata_params['burst_size']
                elif 'burst_size' in metadata:
                    burst_size_value = metadata['burst_size']
                try:
                    bsz = float(burst_size_value) if burst_size_value is not None else None
                except (TypeError, ValueError):
                    bsz = None
                # Interpret iterations as total ops now
                total_ops = float(iterations)
                cycles_per_op = (total_cycles / total_ops) if total_ops > 0 else None
                print(f"cycles_total: {total_cycles:.0f}")
                if cycles_per_op is not None:
                    print(f"cycles_per_op: {cycles_per_op:.6f}")
                if tsc_hz and cycles_per_op and cycles_per_op > 0:
                    ops_per_cycle = 1.0 / cycles_per_op
                    ops_per_sec = ops_per_cycle * tsc_hz
                    print(f"ops_per_cycle: {ops_per_cycle:.6f}")
                    print(f"ops_per_sec: {ops_per_sec:.2f}")
                
                # Merge metadata from benchmark with parameters
                metadata.update(metadata_params)
                if csv_writer:
                    metadata_json = json.dumps(metadata).replace('"', "'")
                    csv_writer.writerow([func, iterations, total_cycles, metadata_json])
            
            return rc
        
        # Run all benchmark cases
        # Detect TSC per-function with its eal_args
        tsc_hz = detect_tsc_hz_local(eal_args)
        if tsc_hz:
            print(f"detected_tsc_hz: {tsc_hz:.0f}")

        for rc in run_benchmark_case(params_dict, grouped_params):
            if rc != 0:
                exit_code = rc

    if csv_file:
        csv_file.flush()
        csv_file.close()
        print(f"\n--- All benchmarks complete ---\nResults written to {csv_path}")
    else:
        print(f"\n--- Dry run complete ---\nNo results file created (dry-run mode)")
    sys.exit(exit_code)

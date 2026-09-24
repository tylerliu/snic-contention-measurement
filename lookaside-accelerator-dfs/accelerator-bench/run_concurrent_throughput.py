import argparse
import csv
import itertools
import json
import os
import re
import shlex
import subprocess
import sys
from datetime import datetime
from typing import Any, Dict, List, Optional, Tuple

# Reuse helpers from shared module
from bench_utils import (
    BenchmarkRunner,
    _parse_cycles,
    _parse_metadata,
    run_setup_command,
)


def _ensure_dir(path: str) -> None:
    if not os.path.isdir(path):
        os.makedirs(path, exist_ok=True)


def _read_json(path: str) -> Any:
    with open(path, 'r') as f:
        return json.load(f)


def _write_csv_row(writer: csv.writer, row: List[Any]) -> None:
    writer.writerow(row)


def _detect_tsc_hz(build_dir: str, eal_args: List[str], cpu_core: int) -> Optional[float]:
    exe = os.path.join(build_dir, 'get_tsc_hz')
    if not os.path.exists(exe):
        return None
    cmd = ["taskset", "-c", str(cpu_core), exe] + eal_args
    try:
        res = subprocess.run(cmd, capture_output=True, text=True)
    except FileNotFoundError:
        return None
    if res.returncode != 0:
        return None
    m = re.search(r"metadata:\s*\{[^}]*'tsc_hz':\s*([0-9]+)\s*\}", res.stdout or "")
    if m:
        try:
            return float(m.group(1))
        except ValueError:
            return None
    return None


def _find_file_prefix(eal_args: List[str]) -> Optional[str]:
    for i, arg in enumerate(eal_args):
        if arg == '--file-prefix' and i + 1 < len(eal_args):
            return eal_args[i + 1]
        if arg.startswith('--file-prefix='):
            return arg.split('=', 1)[1]
    return None


def _set_or_add_file_prefix(eal_args: List[str], prefix: str) -> List[str]:
    # Normalize to separate args form: ['--file-prefix', prefix]
    # Replace if exists, else append
    updated: List[str] = []
    i = 0
    replaced = False
    while i < len(eal_args):
        arg = eal_args[i]
        if arg == '--file-prefix':
            # Skip current and next, replace with our pair
            i += 2
            updated.extend(['--file-prefix', prefix])
            replaced = True
            continue
        if arg.startswith('--file-prefix='):
            i += 1
            updated.extend(['--file-prefix', prefix])
            replaced = True
            continue
        updated.append(arg)
        i += 1
    if not replaced:
        updated.extend(['--file-prefix', prefix])
    return updated


def _merge_eal_args(base: List[str], extra: List[str]) -> List[str]:
    # Simple append; caller ensures no conflicting flags except file-prefix handled separately
    return list(base or []) + list(extra or [])


def _merge_params(base: Dict[str, Any], extra: Dict[str, Any]) -> Dict[str, Any]:
    merged: Dict[str, Any] = dict(base or {})
    for k, v in (extra or {}).items():
        merged[k] = v
    return merged


def _cartesian_param_sets(params: Dict[str, Any]) -> List[Dict[str, Any]]:
    if not params:
        return [{}]
    keys = list(params.keys())
    values = [v if isinstance(v, list) else [v] for v in (params[k] for k in keys)]
    combos = []
    for combo in itertools.product(*values):
        combos.append({keys[i]: combo[i] for i in range(len(keys))})
    return combos


def _build_benchmark_args(param_set: Dict[str, Any], iterations: int) -> List[str]:
    args: List[str] = []
    for key, value in param_set.items():
        # Defensive check: ensure no unexpanded arrays leak through
        if isinstance(value, list):
            if len(value) == 1:
                value = value[0]
            else:
                raise ValueError(f"Parameter '{key}' has unexpanded list value: {value}")
        args.extend([f"--{key}", str(value)])
    args.extend(['-i', str(iterations)])
    return args


def _extract_devices_from_eal_args(eal_args: List[str]) -> List[str]:
    # Very light heuristic: collect BDFs following '-a' or strings containing ':' typical of BDF
    devices: List[str] = []
    i = 0
    while i < len(eal_args):
        arg = eal_args[i]
        if arg == '-a' and i + 1 < len(eal_args):
            devices.append(eal_args[i + 1])
            i += 2
            continue
        if ':' in arg and '=' not in arg and arg.count(':') >= 2:
            devices.append(arg)
        i += 1
    return devices


def main() -> int:
    parser = argparse.ArgumentParser(
        description='Run multiple throughput benchmarks concurrently with config-driven orchestration.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='Example: sudo python3 run_concurrent_throughput.py --scenario concurrent_scenarios.json --build-dir build --verbose'
    )
    parser.add_argument('--scenario', required=True, help='Path to scenario JSON file defining groups and concurrent jobs.')
    parser.add_argument('--build-dir', default='build', help='Meson build directory containing benchmark executables.')
    parser.add_argument('--csv', default=None, help='Path to CSV file for results. If omitted, a timestamped file is created.')
    parser.add_argument('--iterations', type=int, default=1000000, help='Base iterations; actual iterations = base * multiplier for each job.')
    parser.add_argument('--verbose', action='store_true', help='Enable verbose output.')
    parser.add_argument('--dry-run', action='store_true', help='Print commands without executing.')
    parser.add_argument('--tune-cpu', action='store_true', help='Attempt CPU tuning (governor/idle) on the first job core.')
    parser.add_argument('--run-dir', default=None, help='Directory to store per-job logs. Default: runs/<timestamp>')
    args = parser.parse_args()

    # Load scenario
    scenario = _read_json(args.scenario)

    # Load defaults from benchmark_cases.json or override via scenario
    import_cases_path = scenario.get('import_cases') or 'benchmark_cases.json'
    benchmark_cases = _read_json(import_cases_path) if os.path.exists(import_cases_path) else {}

    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    run_root = args.run_dir or os.path.join('runs', f'concurrent_{timestamp}')
    _ensure_dir(run_root)

    # CSV setup
    csv_path = args.csv or os.path.join(run_root, f'concurrent_results_{timestamp}.csv')
    csv_file = open(csv_path, mode='w', newline='') if not args.dry_run else None
    csv_writer = csv.writer(csv_file) if csv_file else None
    if csv_writer:
        # Match run_benchmarks.py essentials and add batch identifier
        csv_writer.writerow(['batch', 'function', 'iterations', 'total_cycles', 'metadata'])

    # Prepare a single BenchmarkRunner for optional CPU tuning and warm-ups
    runner = BenchmarkRunner(cpu_core=0, verbose=args.verbose)

    # Permissions warning (do not elevate)
    if hasattr(os, 'geteuid') and os.geteuid() != 0:
        print('Warning: Not running as root. DPDK setups often require sudo/root.', file=sys.stderr)

    all_groups: List[Dict[str, Any]] = scenario.get('groups', [])
    exit_code_overall = 0
    all_variants: List[Dict[str, Any]] = []  # Collect variants from all groups
    global_batch_idx = 0  # Global batch index across all groups

    for group_idx, group in enumerate(all_groups):
        group_name = group.get('name') or f'group_{group_idx}'
        group_dir = os.path.join(run_root, group_name)
        _ensure_dir(group_dir)

        # Group shared defaults
        # Shared params create the same test cases across all jobs in the group
        # This allows comparing different algorithms on identical test cases
        shared_params: Dict[str, Any] = group.get('shared_params', {}) or {}
        shared_eal_args: List[str] = group.get('shared_eal_args', []) or []
        shared_multiplier: Optional[float] = group.get('shared_iteration_multiplier', 1.0)
        expand_mode: str = 'cartesian'  # Default to cartesian expansion

        # Build job variants (each is one process to launch)
        jobs = group.get('jobs', [])
        variants: List[Dict[str, Any]] = []

        # Create synchronized batches based on shared_params
        # Each batch runs all jobs with the same parameter values concurrently
        shared_param_combinations = _cartesian_param_sets(shared_params) if shared_params else [{}]
        
        # Check if any job has array parameters that need expansion
        job_has_arrays = False
        for job in jobs:
            job_params = job.get('params', {}) or {}
            for key, value in job_params.items():
                if isinstance(value, list) and len(value) > 1:
                    job_has_arrays = True
                    break
            if job_has_arrays:
                break
        
        # Create variants for each shared parameter combination
        for shared_param_set in shared_param_combinations:
            # For each job in this group, create a variant for this shared parameter set
            for job in jobs:
                job_id = job.get('id')
                function = job.get('function')
                if not function:
                    print(f"Skipping job without 'function' in group {group_name}", file=sys.stderr)
                    continue

                # Inherit defaults from benchmark_cases.json
                inherit = bool(job.get('inherit'))
                inherited_case = benchmark_cases.get(function, {}) if inherit else {}
                inherited_params = dict(inherited_case.get('params', {})) if inherited_case else {}
                inherited_eal_args = list(inherited_case.get('eal_args', [])) if inherited_case else []

                # Compose EAL args (inherit -> group shared -> eal_args_merge -> job eal_args)
                job_eal_args_merge: List[str] = job.get('eal_args_merge', []) or []
                job_eal_args: List[str] = job.get('eal_args', []) or []
                eal_args = _merge_eal_args(inherited_eal_args, shared_eal_args)
                eal_args = _merge_eal_args(eal_args, job_eal_args_merge)
                eal_args = _merge_eal_args(eal_args, job_eal_args)

                # Compose params: inherit -> job params
                job_params = job.get('params', {}) or {}
                base_params = _merge_params(inherited_params, job_params)

                # Iteration multiplier precedence: case > job > shared > 1.0
                job_multiplier = job.get('iteration_multiplier', shared_multiplier)

                # If job has array params, create variants for each job param combination
                if job_has_arrays:
                    job_param_combinations = _cartesian_param_sets(job_params) if job_params else [{}]
                    for job_param_set in job_param_combinations:
                        # Merge shared params with job-specific params
                        final_params = _merge_params(base_params, shared_param_set)
                        final_params = _merge_params(final_params, job_param_set)
                        # Flatten any singleton lists to scalars at creation time
                        for pk, pv in list(final_params.items()):
                            if isinstance(pv, list) and len(pv) == 1:
                                final_params[pk] = pv[0]
                        
                        # Create variant for this combination
                        cpu_core = int(job.get('cpu_core', 0))
                        iterations = int(args.iterations * job_multiplier)
                        variant = {
                            'group': group_name,
                            'job_id': job_id or function,
                            'function': function,
                            'eal_args': list(eal_args),
                            'params': final_params,
                            'iterations': iterations,
                            'cpu_core': cpu_core,
                            'batch_idx': global_batch_idx,
                        }
                        variants.append(variant)
                        all_variants.append(variant)
                else:
                    # No job arrays, use original logic
                    # Merge shared params with job-specific params
                    final_params = _merge_params(base_params, shared_param_set)
                    # Flatten any singleton lists to scalars at creation time
                    for pk, pv in list(final_params.items()):
                        if isinstance(pv, list) and len(pv) == 1:
                            final_params[pk] = pv[0]
                    
                    # Create variant for this batch
                    cpu_core = int(job.get('cpu_core', 0))
                    iterations = int(args.iterations * job_multiplier)
                    variant = {
                        'group': group_name,
                        'job_id': job_id or function,
                        'function': function,
                        'eal_args': list(eal_args),
                        'params': final_params,
                        'iterations': iterations,
                        'cpu_core': cpu_core,
                        'batch_idx': global_batch_idx,
                    }
                    variants.append(variant)
                    all_variants.append(variant)
            
            # Increment batch index after processing all jobs for this shared parameter set
            global_batch_idx += 1

        # Validation: enforce unique file-prefix within batches only
        # Group variants by batch first to ensure prefixes are unique within each batch
        batches_for_prefix: Dict[int, List[Dict[str, Any]]] = {}
        for v in variants:
            batch_idx = v.get('batch_idx', 0)
            if batch_idx not in batches_for_prefix:
                batches_for_prefix[batch_idx] = []
            batches_for_prefix[batch_idx].append(v)
        
        # Generate unique prefixes within each batch
        for batch_idx, batch_variants in batches_for_prefix.items():
            used_prefixes_in_batch: set = set()
            for i, v in enumerate(batch_variants):
                prefix_existing = _find_file_prefix(v['eal_args'])
                if not prefix_existing:
                    # Generate a prefix based on job_id only (consistent across batches)
                    auto_prefix = f"{v['job_id']}"
                    v['eal_args'] = _set_or_add_file_prefix(v['eal_args'], auto_prefix)
                else:
                    # Use existing prefix
                    v['eal_args'] = _set_or_add_file_prefix(v['eal_args'], prefix_existing)
                
                prefix = _find_file_prefix(v['eal_args'])
                if prefix in used_prefixes_in_batch:
                    # Make unique within this batch only
                    new_prefix = f"{prefix}-{i}"
                    v['eal_args'] = _set_or_add_file_prefix(v['eal_args'], new_prefix)
                    prefix = new_prefix
                used_prefixes_in_batch.add(prefix)

        # Light device contention warning - only check within batches
        # Group variants by batch first
        batches_for_warning: Dict[int, List[Dict[str, Any]]] = {}
        for v in variants:
            batch_idx = v.get('batch_idx', 0)
            if batch_idx not in batches_for_warning:
                batches_for_warning[batch_idx] = []
            batches_for_warning[batch_idx].append(v)
        
        # Check device contention within each batch
        for batch_idx, batch_variants in batches_for_warning.items():
            seen_devices: Dict[str, int] = {}
            for v in batch_variants:
                for dev in _extract_devices_from_eal_args(v['eal_args']):
                    seen_devices[dev] = seen_devices.get(dev, 0) + 1
            for dev, count in seen_devices.items():
                if count > 1:
                    print(f"Warning: device '{dev}' is referenced by {count} concurrent jobs in batch {batch_idx + 1}. Multiple primary processes may fail.", file=sys.stderr)

    # Now execute all variants from all groups
    # Optional CPU tuning on the first variant core
    if getattr(args, 'tune_cpu', False) and all_variants:
        runner.cpu_core = int(all_variants[0]['cpu_core'])
        if not args.dry_run:
            runner.setup_cpu()

    # Detect TSC per variant (could be expensive; keep simple)
    for v in all_variants:
        v['tsc_hz'] = _detect_tsc_hz(args.build_dir, v['eal_args'], v['cpu_core'])

    # Group variants by batch for synchronized execution
    batches: Dict[int, List[Dict[str, Any]]] = {}
    for v in all_variants:
        batch_idx = v.get('batch_idx', 0)
        if batch_idx not in batches:
            batches[batch_idx] = []
        batches[batch_idx].append(v)

    # Execute batches sequentially, but jobs within each batch concurrently
    for batch_idx in sorted(batches.keys()):
        batch_variants = batches[batch_idx]
        print(f"\n=== Executing Batch {batch_idx + 1} with {len(batch_variants)} jobs ===")
        
        # Launch all variants in this batch concurrently
        procs: List[Tuple[Dict[str, Any], subprocess.Popen, str, str]] = []
        for i, v in enumerate(batch_variants):
            group_dir = os.path.join(run_root, v['group'])
            _ensure_dir(group_dir)
            job_dir = os.path.join(group_dir, f"{v['job_id']}-{i}")
            _ensure_dir(job_dir)

            exe_path = os.path.join(args.build_dir, v['function'])
            if not os.path.exists(exe_path):
                print(f"Executable not found: {exe_path}", file=sys.stderr)
                exit_code_overall = 1
                continue

            benchmark_args = _build_benchmark_args(v['params'], v['iterations'])
            cmd = ["taskset", "-c", str(v['cpu_core']), exe_path] + v['eal_args'] + ["--"] + benchmark_args

            print(f"\n=== Launching {v['function']} ({v['job_id']}) on core {v['cpu_core']} ===")
            print("Command:", " ".join(shlex.quote(part) for part in cmd))
            if args.dry_run:
                # Skip starting process, but still write a placeholder log
                stdout_path = os.path.join(job_dir, 'stdout.log')
                stderr_path = os.path.join(job_dir, 'stderr.log')
                with open(stdout_path, 'w') as so:
                    so.write('DRY RUN: command not executed\n')
                with open(stderr_path, 'w') as se:
                    se.write('')
                continue

            stdout_f = open(os.path.join(job_dir, 'stdout.log'), 'w')
            stderr_f = open(os.path.join(job_dir, 'stderr.log'), 'w')
            proc = subprocess.Popen(cmd, stdout=stdout_f, stderr=stderr_f, text=True)
            procs.append((v, proc, stdout_f.name, stderr_f.name))

        # Wait for completion and parse outputs for this batch
        for v, proc, stdout_path, stderr_path in procs:
            rc = proc.wait()
            with open(stdout_path, 'r') as so:
                out_text = so.read()
            with open(stderr_path, 'r') as se:
                err_text = se.read()

            if rc != 0:
                print(f"Error: job {v['job_id']} ({v['function']}) exited with {rc}", file=sys.stderr)
                if err_text:
                    print(err_text, file=sys.stderr)
                exit_code_overall = rc

            cycles = _parse_cycles(out_text or '')
            metadata = _parse_metadata(out_text or '')
            # Merge input params into metadata for traceability
            metadata.update(v['params'])
            metadata['cpu_core'] = v['cpu_core']
            metadata_json = json.dumps(metadata).replace('"', "'")

            iterations = v['iterations']
            cycles_per_op: Optional[float] = (float(cycles) / float(iterations)) if (cycles is not None and iterations > 0) else None
            tsc_hz = v.get('tsc_hz')
            ops_per_sec: Optional[float] = (1.0 / cycles_per_op * tsc_hz) if (tsc_hz and cycles_per_op and cycles_per_op > 0) else None
            file_prefix = _find_file_prefix(v['eal_args']) or ''

            # Print ops/sec to screen like run_benchmarks.py, but do not store in CSV
            if cycles is not None:
                # Print program id before metrics
                if v.get('job_id'):
                    print(f"program_id: {v['job_id']}")
                print(f"cycles_total: {int(cycles)}")
                if cycles_per_op is not None:
                    print(f"cycles_per_op: {cycles_per_op:.6f}")
                if ops_per_sec is not None:
                    print(f"ops_per_sec: {ops_per_sec:.2f}")

            if csv_writer:
                _write_csv_row(csv_writer, [
                    (batch_idx + 1), v['function'], iterations,
                    int(cycles) if cycles is not None else '',
                    metadata_json
                ])

        # Flush per batch
        if csv_file:
            csv_file.flush()

    if csv_file:
        csv_file.close()
        print(f"\n--- Concurrent run complete ---\nResults written to {csv_path}")

    return exit_code_overall


if __name__ == '__main__':
    sys.exit(main())



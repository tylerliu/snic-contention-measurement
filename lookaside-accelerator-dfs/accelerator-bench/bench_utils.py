import atexit
import json
import os
import re
import shlex
import signal
import subprocess
import sys
from typing import Any, Dict, List, Optional, Tuple


class BenchmarkRunner:
    def __init__(self, cpu_core: int = 3, verbose: bool = False) -> None:
        self.cpu_core = cpu_core
        self.verbose = verbose
        self.original_settings: Dict[str, Any] = {}
        self.setup_completed = False
        self.teardown_completed = False

        atexit.register(self.teardown)
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)

    def signal_handler(self, signum, frame):
        print(f"\nReceived signal {signum}, cleaning up...")
        self.teardown()
        sys.exit(1)

    def get_current_settings(self) -> None:
        try:
            result = subprocess.run([
                "cpupower", "-c", str(self.cpu_core), "frequency-info"
            ], capture_output=True, text=True, check=True)

            for line in result.stdout.split('\n'):
                if 'current policy:' in line and 'governor "' in line:
                    governor_start = line.find('governor "') + 9
                    governor_end = line.find('"', governor_start)
                    if governor_end > governor_start:
                        governor = line[governor_start:governor_end]
                        self.original_settings['governor'] = governor
                        break

            print("✓ Captured current CPU settings for restoration")

        except subprocess.CalledProcessError as e:
            print(f"⚠ Warning: Could not capture current settings: {e}")
            self.original_settings['governor'] = 'ondemand'

    def check_cpupower_support(self) -> Dict[str, bool]:
        supported_commands: Dict[str, bool] = {}
        try:
            subprocess.run(["cpupower", "frequency-set", "--help"], capture_output=True, text=True, check=True)
            supported_commands['frequency-set'] = True
        except (subprocess.CalledProcessError, FileNotFoundError):
            supported_commands['frequency-set'] = False

        try:
            subprocess.run(["cpupower", "idle-set", "--help"], capture_output=True, text=True, check=True)
            supported_commands['idle-set'] = True
        except (subprocess.CalledProcessError, FileNotFoundError):
            supported_commands['idle-set'] = False

        return supported_commands

    def setup_cpu(self) -> None:
        print("Setting up CPU for benchmarking...")
        supported_commands = self.check_cpupower_support()
        self.get_current_settings()

        print(f"Current CPU {self.cpu_core} settings:")
        if 'governor' in self.original_settings:
            print(f"  Governor: {self.original_settings['governor']}")
        print()

        if supported_commands.get('frequency-set', False):
            try:
                subprocess.run([
                    "sudo", "cpupower", "-c", str(self.cpu_core), "frequency-set",
                    "-g", "performance"
                ], check=True, capture_output=True)
                print(f"✓ Set CPU {self.cpu_core} to performance mode")
            except subprocess.CalledProcessError as e:
                print(f"⚠ Warning: Could not set CPU governor: {e}")
        else:
            print("⚠ Note: CPU frequency control not available, skipping governor setting")

        if supported_commands.get('idle-set', False):
            try:
                subprocess.run([
                    "sudo", "cpupower", "-c", str(self.cpu_core), "idle-set", "-d", "0"
                ], check=True, capture_output=True)
                print("✓ Disabled CPU idle states")
            except subprocess.CalledProcessError as e:
                print(f"⚠ Warning: Could not disable CPU idle states: {e}")
        else:
            print("⚠ Note: CPU idle state control not available, skipping idle state setting")

        self.setup_completed = True

    def teardown(self) -> None:
        if not self.setup_completed or self.teardown_completed:
            return
        self.teardown_completed = True
        print("\nRestoring CPU settings...")

        if 'governor' in self.original_settings:
            try:
                subprocess.run([
                    "sudo", "cpupower", "-c", str(self.cpu_core), "frequency-set",
                    "-g", self.original_settings['governor']
                ], check=True, capture_output=True)
                print(f"✓ Restored governor to {self.original_settings['governor']}")
            except subprocess.CalledProcessError as e:
                print(f"⚠ Warning: Could not restore governor: {e}")

        try:
            subprocess.run([
                "sudo", "cpupower", "-c", str(self.cpu_core), "idle-set", "-e", "0"
            ], check=True, capture_output=True)
            print("✓ Re-enabled CPU idle states")
        except subprocess.CalledProcessError:
            pass

        print("✓ CPU settings restored")

    def warm_up(self, exe_path: str, cmd_args: List[str]) -> bool:
        if self.verbose:
            print(f"Warming up {exe_path}...")
        try:
            warm_up_args: List[str] = []
            i = 0
            while i < len(cmd_args):
                if cmd_args[i] == "-i" and i + 1 < len(cmd_args):
                    warm_up_args.extend(["-i", "10000"])
                    i += 2
                else:
                    warm_up_args.append(cmd_args[i])
                    i += 1

            subprocess.run([
                "taskset", "-c", str(self.cpu_core), exe_path
            ] + warm_up_args, check=True, capture_output=True)
            if self.verbose:
                print("✓ Warm-up completed")
        except subprocess.CalledProcessError as e:
            if self.verbose:
                print(f"✗ Warm-up failed: {e}")
            return False
        return True


def detect_and_explain_common_errors(stdout_text: str, stderr_text: str) -> None:
    combined = f"{stdout_text}\n{stderr_text}"
    lower = combined.lower()

    if ("operation not permitted" in lower or "permission denied" in lower) and hasattr(os, 'geteuid') and os.geteuid() != 0:
        print("Hint: Permission issue detected. Many DPDK/DOCA setups require root. Re-run with sudo (warning only; this script does not elevate).", file=sys.stderr)

    if "huge" in lower and "page" in lower:
        print("Hint: Hugepage-related error. Ensure hugepages are configured and available for your platform.", file=sys.stderr)

    if "eal:" in lower and ("cannot allocate memory" in lower or "could not reserve memory" in lower or "no available hugepages" in lower):
        print("Hint: EAL memory reservation failed, commonly due to missing/insufficient hugepages.", file=sys.stderr)

    if "telemetry: no legacy callbacks, legacy socket not created" in lower:
        print("Note: Telemetry legacy socket is disabled. This message is informational and usually harmless.", file=sys.stderr)

    if "cannot configure ethdev port" in lower and "null config" in lower:
        print("Hint: Port configuration called with NULL config. Update the benchmark driver to provide a valid rte_eth_conf.", file=sys.stderr)

    if ("cannot configure device:" in lower and "err=-22" in lower) or "cause: cannot configure device" in lower:
        print("Hint: Device configuration returned -EINVAL. Ensure the device is present/bound and queue settings are valid; verify EAL device args (e.g., -a, --vdev).", file=sys.stderr)


def build_executable_path(build_dir: str, function_name: str) -> str:
    return os.path.join(build_dir, function_name)


def _parse_cycles(stdout_text: str) -> Optional[float]:
    match = re.search(r"(?:Total cycles|Cycles for \w+ empty):\s*([0-9]+(?:\.[0-9]+)?)", stdout_text)
    if match:
        try:
            return float(match.group(1))
        except ValueError:
            return None

def build_ssh_command(cmd_list: List[str], host: Optional[str] = None, timeout_secs: Optional[int] = None, signal: str = "SIGINT") -> List[str]:
    """
    Wraps a command list with sudo, timeout, and ssh as needed.
    """
    final_cmd = list(cmd_list)
    
    # Prefix with timeout if duration is specified
    if timeout_secs:
        # We use --preserve-status so we can detect clean exits
        final_cmd = ["sudo", "timeout", "--preserve-status", f"--signal={signal}", str(timeout_secs)] + final_cmd
    elif final_cmd[0] != "sudo":
        # Ensure sudo is present if not already there (benchmarks usually need it)
        final_cmd = ["sudo"] + final_cmd

    if host and host != "localhost":
        # Quote the command for SSH execution
        cmd_str = " ".join(shlex.quote(arg) for arg in final_cmd)
        
        # If running as root via sudo, run ssh as the original user
        # to pick up their ~/.ssh/config and keys.
        ssh_base = ["ssh", host]
        if os.geteuid() == 0 and "SUDO_USER" in os.environ:
            ssh_base = ["sudo", "-u", os.environ["SUDO_USER"], "ssh", host]
            
        return ssh_base + [cmd_str]
    
    return final_cmd

def parse_network_stats(stdout_text: str, sample_index: int = -1) -> Dict[str, Any]:
    """
    Parses TX/RX PPS, MB/s and paused cycles from dpdk_simple_gen output.
    If sample_index is positive, returns the Nth sample (starting from 1).
    Otherwise returns the last sample.
    """
    stats = {}
    
    tx_matches = re.findall(r"TOTAL TX:\s*([0-9.]+)\s*pps,\s*([0-9.]+)\s*MB/s", stdout_text)
    rx_matches = re.findall(r"TOTAL RX:\s*([0-9.]+)\s*pps,\s*([0-9.]+)\s*MB/s", stdout_text)
    pause_matches = re.findall(r"Cycles paused \(TX\):\s*([0-9]+(?:\.[0-9]+)?)", stdout_text)

    tx_idx = -1
    rx_idx = -1
    if tx_matches:
        # If we want the 5th sample, but only have 4, we take the last one available
        if sample_index > 0:
            tx_idx = min(sample_index - 1, len(tx_matches) - 1)

        stats['tx_pps'] = float(tx_matches[tx_idx][0])
        stats['tx_mbps'] = float(tx_matches[tx_idx][1])

    if rx_matches:
        if sample_index > 0:
            rx_idx = min(sample_index - 1, len(rx_matches) - 1)

        stats['rx_pps'] = float(rx_matches[rx_idx][0])
        stats['rx_mbps'] = float(rx_matches[rx_idx][1])

    if pause_matches:
        # Pause matches might be fewer or differently timed, try to match index
        idx = tx_idx if tx_idx >= 0 else rx_idx
        p_idx = min(idx if idx >= 0 else len(pause_matches)-1, len(pause_matches)-1)
        stats['cycles_paused_tx'] = int(float(pause_matches[p_idx]))

    return stats


def parse_command_stats(stdout_text: str) -> Dict[str, Any]:
    """
    Parses generic command workload output. Currently recognizes DPA memop
    throughput lines like:
      Throughput (Device Reported): 12117.62 MB/s (Avg Time: 1.35 s)
    """
    stats = {}

    throughput_matches = re.findall(
        r"Throughput \(Device Reported\):\s*([0-9]+(?:\.[0-9]+)?)\s*MB/s"
        r"(?:\s*\(Avg Time:\s*([0-9]+(?:\.[0-9]+)?)\s*s\))?",
        stdout_text
    )

    if throughput_matches:
        throughputs = [float(match[0]) for match in throughput_matches]
        stats['device_mbps'] = sum(throughputs) / len(throughputs)
        stats['samples'] = len(throughputs)

        avg_times = [float(match[1]) for match in throughput_matches if match[1]]
        if avg_times:
            stats['avg_time_s'] = sum(avg_times) / len(avg_times)

    return stats

def _parse_operations(stdout_text: str) -> Optional[int]:
    match = re.search(r"Total operations:\s*([0-9]+)", stdout_text)
    if match:
        try:
            return int(match.group(1))
        except ValueError:
            return None
    return None

def _parse_wait_cycles(stdout_text: str) -> Optional[float]:
    match = re.search(r"Wait cycles:\s*([0-9]+(?:\.[0-9]+)?)", stdout_text)
    if match:
        try:
            return float(match.group(1))
        except ValueError:
            return None
    return 0.0

def verify_arp_resolution(line: str) -> bool:
    """
    Returns True if an ARP resolution failure is detected in the output line.
    Specific to dpdk_simple_gen output format.
    """
    return "ff:ff:ff:ff:ff:ff" in line or "ARP resolution timed out" in line

def is_generator_started(line: str) -> bool:
    """
    Returns True if the generator has successfully started producing samples.
    """
    return "TOTAL TX:" in line or "TOTAL RX:" in line


def _parse_metadata(stdout_text: str) -> Dict[str, Any]:
    match = re.search(r"metadata:\s*(\{.*\})", stdout_text)
    if match:
        try:
            json_str = match.group(1).replace("'", '"')
            return json.loads(json_str)
        except (ValueError, json.JSONDecodeError):
            return {}
    return {}


def parse_crypto_stats(stdout_text: str) -> Dict[str, Any]:
    """
    Parses Total cycles, Total operations, and Wait cycles from crypto benchmark output.
    """
    stats = {}
    
    cycles = _parse_cycles(stdout_text)
    if cycles is not None:
        stats['total_cycles'] = int(cycles)
        
    ops = _parse_operations(stdout_text)
    if ops is not None:
        stats['total_ops'] = int(ops)
        
    wait = _parse_wait_cycles(stdout_text)
    if wait is not None:
        stats['wait_cycles'] = int(wait)
        
    return stats

def run_setup_command(setup_command: str, dry_run: bool = False) -> bool:
    if not setup_command:
        return True
    print(f"\n--- Running setup command ---")
    print(f"Command: {setup_command}")
    if dry_run:
        print("DRY RUN: Would execute the above setup command")
        return True
    try:
        result = subprocess.run(setup_command, shell=True, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"Setup command failed with return code {result.returncode}:", file=sys.stderr)
            if result.stdout:
                print(result.stdout, file=sys.stderr)
            if result.stderr:
                print(result.stderr, file=sys.stderr)
            return False
        else:
            if result.stdout:
                print(result.stdout.strip())
            print("✓ Setup command completed successfully")
            return True
    except Exception as e:
        print(f"Error running setup command: {e}", file=sys.stderr)
        return False


def check_permissions() -> bool:
    if hasattr(os, 'geteuid') and os.geteuid() == 0:
        print("✓ Script is running with sudo privileges")
        print("   This is recommended for accurate DPDK measurements")
        print("   as it allows access to hardware resources and hugepages")
        print()
        return True
    else:
        print("⚠ Warning: Script is not running with sudo privileges")
        print("   Many DPDK setups require root/sudo for:")
        print("   - Hugepage allocation")
        print("   - Device binding")
        print("   - CPU frequency control")
        print("   Solutions:")
        print("   1. Run with sudo: sudo python3 ... (recommended)")
        print("   2. Ensure hugepages are pre-allocated")
        print("   3. Pre-bind devices to DPDK drivers")
        print("   Note: Some measurements may fail without proper permissions")
        print()
        return False


def detect_tsc_hz(build_dir: str, eal_args: List[str], cpu_core: int) -> Optional[float]:
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



"""
remote.py - Local/SSH command helpers for cluster-mode orchestration.
"""

from __future__ import annotations

import socket
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Any, Callable, Dict, List, Tuple


LOCAL_HOSTNAME = socket.gethostname()


def is_local_host(host: str) -> bool:
    return host == LOCAL_HOSTNAME or host in ('localhost', '127.0.0.1')


def ssh_argv(host: str, connect_timeout: int = 10) -> List[str]:
    """Standard non-interactive ssh prefix used by every remote command."""
    return ['ssh',
            '-o', 'BatchMode=yes',
            '-o', f'ConnectTimeout={connect_timeout}',
            '-o', 'StrictHostKeyChecking=no',
            host]


def run_remote(host: str, *, remote_cmd: List[str], stdin: str = None,
               ssh_timeout: int = 30, connect_timeout: int = 10
               ) -> subprocess.CompletedProcess:
    """Run a command on `host`, or locally if `host` resolves to this machine."""
    argv = remote_cmd if is_local_host(host) else (
        ssh_argv(host, connect_timeout) + remote_cmd)
    return subprocess.run(
        argv, input=stdin,
        capture_output=True, text=True, timeout=ssh_timeout,
    )


def parallel_per_host(hosts: List[str], fn: Callable[[str], Any],
                      max_workers: int = 32) -> Tuple[Dict[str, Any], Dict[str, str]]:
    """Run fn(host) in parallel across hosts."""
    if not hosts:
        return {}, {}

    results: Dict[str, Any] = {}
    errors: Dict[str, str] = {}
    with ThreadPoolExecutor(max_workers=min(max_workers, len(hosts))) as executor:
        futures = {executor.submit(fn, host): host for host in hosts}
        for future in as_completed(futures):
            host = futures[future]
            try:
                results[host] = future.result()
            except Exception as exc:
                errors[host] = str(exc)
    return results, errors


def check_versions_on_hosts(hosts: List[str], binary: str,
                            ssh_timeout: int = 30,
                            connect_timeout: int = 10
                            ) -> Tuple[Dict[str, str], Dict[str, str]]:
    """Run '<binary> -V' on each host.

    Returns (versions, errors): `versions` maps host -> the parsed version
    string (only for hosts the probe succeeded on); `errors` maps host -> an
    error message for hosts that could not be probed (SSH/connect failure,
    timeout, etc). Keeping these separate prevents an unreachable host from
    being compared as if it reported a real (mismatched) version string.
    """
    def get_version(host: str) -> str:
        # Let SSH/subprocess failures propagate; parallel_per_host routes
        # them into the returned `errors` dict instead of a fake "version".
        result = run_remote(
            host, remote_cmd=[binary, '-V'],
            ssh_timeout=ssh_timeout, connect_timeout=connect_timeout)

        output = (result.stdout + result.stderr).strip()
        for line in output.splitlines():
            line = line.strip()
            if not line:
                continue
            if any(line.startswith(prefix)
                   for prefix in ('[', 'UCX ', 'WARNING', 'ERROR')):
                continue
            return line
        return 'unknown'

    return parallel_per_host(hosts, get_version)

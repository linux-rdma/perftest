"""
discovery.py - GPU affinity discovery orchestration for cluster mode.
"""

from __future__ import annotations

import json
import subprocess
import sys
from typing import Dict, List

from .remote import parallel_per_host, run_remote
from .traffic_patterns import (
    RankAssignment, build_occurrence_map, resolve_occurrence_aware,
)


def lookup_gpu(discovery_results, rdma_device):
    """Find the GPU BDF for a given RDMA device in discovery results."""
    for entry in discovery_results:
        if rdma_device in entry.get('rdma_devices', []):
            return entry.get('gpu_bdf')
    return None


def host_needs_probe(gpu_map: dict, host: str) -> bool:
    """Return True if host has no concrete device identification yet.

    `gpu_map[host]` may be a single dict or a list of dicts (one per local
    RDMA NIC/occurrence, for a host repeated in testNodes[] with per-NIC GPU
    overrides); the host needs probing if *any* occurrence still lacks a
    concrete assignment.
    """
    configured = gpu_map.get(host)
    if configured is None:
        return True
    entries = configured if isinstance(configured, list) else [configured]
    return any(e.get('gpuDeviceId', -1) < 0 and not e.get('gpuDeviceBusId', '')
              for e in entries)


DISCOVERY_COMMAND = 'perftest_nic_gpu_discover'


def run_discovery_on_hosts(hosts: List[str], ssh_timeout: int = 30,
                           connect_timeout: int = 10) -> Dict[str, list]:
    """Run perftest_nic_gpu_discover on hosts in parallel via SSH."""
    def probe(host: str):
        try:
            result = run_remote(
                host, remote_cmd=[DISCOVERY_COMMAND],
                ssh_timeout=ssh_timeout, connect_timeout=connect_timeout)
        except subprocess.TimeoutExpired:
            raise RuntimeError(
                f"{DISCOVERY_COMMAND} timed out after {ssh_timeout}s")

        stderr = result.stderr or ''
        if result.returncode == 127 or 'command not found' in stderr:
            raise RuntimeError(
                f"{DISCOVERY_COMMAND} not found on remote host. Install "
                "perftest cluster mode (`sudo make install` from the "
                "perftest source tree on every worker host).")
        if result.returncode == 126:
            raise RuntimeError(
                f"{DISCOVERY_COMMAND} found but not executable on remote "
                "host (permission denied). Check the execute bit and PATH "
                f"resolution for {DISCOVERY_COMMAND}.")
        if result.returncode != 0:
            raise RuntimeError(
                f"{DISCOVERY_COMMAND} exited {result.returncode}: "
                f"{stderr.strip() or result.stdout.strip()}")

        output = result.stdout.strip()
        if not output:
            raise RuntimeError(f"{DISCOVERY_COMMAND} returned no output")

        try:
            data = json.loads(output)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"Invalid JSON from {DISCOVERY_COMMAND}: {exc}")

        if isinstance(data, dict) and 'error' in data:
            raise RuntimeError(f"Discovery error: {data['error']}")
        return data

    results, errors = parallel_per_host(hosts, probe)
    if errors:
        lines = '\n'.join(f'  {h}: {e}' for h, e in sorted(errors.items()))
        raise RuntimeError(
            f"[Cluster] GPU discovery failed on {len(errors)} host(s):\n{lines}")
    return results


def run_auto_detect(assignments: List[RankAssignment],
                    gpu_map: Dict[str, dict],
                    out=None) -> Dict[str, dict]:
    """Probe hosts without concrete GPU assignments and return updated gpu_map.

    A host driving more than one local RDMA NIC gets one resolved GPU entry
    per distinct device, in declaration order (perftest_nic_gpu_discover
    returns the full per-NIC list for a host in a single probe).
    """
    if out is None:
        out = sys.stdout

    all_hosts = list(dict.fromkeys(a.hostname for a in assignments))
    hosts_to_probe = [h for h in all_hosts if host_needs_probe(gpu_map, h)]
    if not hosts_to_probe:
        return gpu_map

    print(f"[Cluster] Auto-detecting GPU affinity on "
          f"{len(hosts_to_probe)} host(s)...", file=out, flush=True)

    discovery_results = run_discovery_on_hosts(hosts_to_probe)
    updated = dict(gpu_map)

    for host, results in discovery_results.items():
        existing = updated.get(host)
        existing_entries = existing if isinstance(existing, list) else [existing or {}]
        existing_type = next(
            (e.get('gpuType', '') for e in existing_entries if e), '')

        host_assignments = sorted(
            (a for a in assignments if a.hostname == host),
            key=lambda a: a.node_index)
        # Distinct local devices this host uses, in occurrence order (there
        # may be more than one RankAssignment per device - e.g. the same NIC
        # used as both server and client across different connections).
        seen_devices: List[str] = []
        for a in host_assignments:
            if a.device not in seen_devices:
                seen_devices.append(a.device)

        by_device: Dict[str, dict] = {}
        for device in seen_devices:
            gpu_bdf = lookup_gpu(results, device)
            if gpu_bdf is None:
                print(f"[Cluster]   {host}: no GPU found for RDMA device "
                      f"'{device}'", file=out)
                continue
            by_device[device] = {
                'gpuType': existing_type or 'cuda',
                'gpuDeviceId': -1,
                'gpuDeviceBusId': gpu_bdf,
            }
            print(f"[Cluster]   {host}: {device} -> gpu={gpu_bdf}", file=out)

        if not by_device:
            continue
        if len(seen_devices) == 1:
            updated[host] = by_device[seen_devices[0]]
        else:
            # One entry per occurrence, in seen_devices order, so
            # apply_gpu_map's occurrence-index lookup lines up. An
            # unresolved device gets an empty placeholder rather than
            # silently inheriting a different NIC's GPU.
            updated[host] = [by_device.get(d, {}) for d in seen_devices]

    apply_gpu_map(assignments, updated)
    return updated


def apply_gpu_map(assignments: List[RankAssignment],
                  gpu_map: Dict[str, dict]) -> None:
    """Apply gpu_map values back to RankAssignment objects in place.

    `gpu_map[hostname]` may be a single dict (applies uniformly) or a list
    of dicts, one per occurrence in testNodes[] declaration order (see
    resolve_occurrence_aware()).
    """
    occ_map = build_occurrence_map(
        [(a.hostname, a.node_index) for a in assignments])
    for assignment in assignments:
        configured = resolve_occurrence_aware(
            gpu_map, assignment.hostname, assignment.node_index, occ_map, None)
        if configured and (configured.get('gpuDeviceId', -1) >= 0
                           or configured.get('gpuDeviceBusId', '')):
            assignment.gpu_type = configured.get('gpuType', '')
            assignment.gpu_device_id = configured.get('gpuDeviceId', -1)
            assignment.gpu_device_bus_id = configured.get('gpuDeviceBusId', '')

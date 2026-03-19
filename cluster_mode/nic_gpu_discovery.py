#!/usr/bin/env python3
"""
nic_gpu_discovery.py - Discover NIC-to-GPU physical relationships.

Installed by `make install` as $(bindir)/perftest_nic_gpu_discover (see Makefile.am).
The orchestrator invokes it on each remote host via SSH:
    ssh host perftest_nic_gpu_discover

Two matching methods (tried in order for each RDMA NIC):
  1. DMA-based:  Match NIC to a paired DMA function via VPD serial numbers,
                 then resolve the GPU in the same PCI domain. Requires root
                 for lspci -vvv VPD access.
  2. Topology:   Find the closest GPU by PCIe topology distance (sysfs path
                 walk to nearest common PCI bridge ancestor). Used as
                 fallback when no DMA match exists or VPD is unreadable.

Output: JSON array. Each entry has a 'method' field ('dma' or 'topology')
and method-specific data in a 'details' nested object.

When run as a script (piped via SSH by the orchestrator), prints JSON to
stdout and exits.
"""

import json
import os
import subprocess
import sys
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple


@dataclass(frozen=True)
class DiscoveryEntry:
    nic_bdf: str
    gpu_bdf: str
    method: str
    nic_ports: List[str]
    rdma_devices: List[str]
    numa_node: Optional[int] = None
    details: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return {
            'nic_bdf': self.nic_bdf,
            'gpu_bdf': self.gpu_bdf,
            'method': self.method,
            'nic_ports': self.nic_ports,
            'rdma_devices': self.rdma_devices,
            'numa_node': self.numa_node,
            'details': self.details,
        }


def _run(cmd: List[str]) -> str:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
    except FileNotFoundError:
        raise RuntimeError(f"Command not found: {cmd[0]}")
    if r.returncode != 0:
        raise RuntimeError(
            f"Command failed ({' '.join(cmd)}): {r.stderr.strip()}"
        )
    return r.stdout


def _bdf_domain(bdf: str) -> str:
    """'0009:03:00.0' -> '0009'"""
    return bdf.split(':')[0]


def _bdf_slot(bdf: str) -> str:
    """'0000:03:00.1' -> '0000:03:00' (strip function)"""
    return bdf.rsplit('.', 1)[0]


def _read_sysfs_attr(bdf: str, attr: str) -> Optional[str]:
    try:
        with open(f'/sys/bus/pci/devices/{bdf}/{attr}', encoding='utf-8') as f:
            return f.read().strip()
    except OSError:
        return None


def _find_rdma_nics() -> Tuple[List[str], Dict[str, List[str]]]:
    """Discover RDMA-capable NICs via /sys/class/infiniband/.

    Vendor-agnostic: any NIC that registers an RDMA device will be found,
    whether it's Ethernet or InfiniBand mode.

    Returns (nic_bdfs, rdma_map) where:
        nic_bdfs: list of unique PCI BDFs for all RDMA NIC ports
        rdma_map: {pci_bdf: [rdma_dev_name, ...]}
    """
    ib_path = '/sys/class/infiniband'
    rdma_map = {}
    if not os.path.isdir(ib_path):
        return [], {}
    for dev_name in sorted(os.listdir(ib_path)):
        try:
            real = os.path.realpath(os.path.join(ib_path, dev_name, 'device'))
            pci_bdf = os.path.basename(real)
            rdma_map.setdefault(pci_bdf, []).append(dev_name)
        except OSError:
            continue
    return sorted(rdma_map.keys()), rdma_map


def _find_gpus() -> List[str]:
    """Find NVIDIA GPU display/3D controllers via lspci."""
    return [line.split()[0] for line in _run(['lspci', '-D']).splitlines()
            if ('NVIDIA' in line and
                ('3D controller' in line or
                 'VGA compatible controller' in line))]


def _find_dma_controllers() -> List[str]:
    """Find DMA controllers via sysfs PCI class 0801 (vendor-agnostic)."""
    pci_path = '/sys/bus/pci/devices'
    if not os.path.isdir(pci_path):
        return []
    return [entry for entry in sorted(os.listdir(pci_path))
            if (_read_sysfs_attr(entry, 'class') or '').startswith('0x0801')]


def _read_vpd(bdf: str) -> Tuple[Optional[str], Optional[str]]:
    """Read VPD [SN] and [V3] fields via `lspci -s <bdf> -vvv`.

    Returns (serial_number, v3) or (None, None) if VPD is unreadable.
    """
    try:
        output = _run(['lspci', '-s', bdf, '-vvv'])
    except RuntimeError:
        return None, None
    sn, v3 = None, None
    in_vpd = False
    for line in output.splitlines():
        stripped = line.strip()
        if 'Vital Product Data' in stripped and 'Capabilities' in stripped:
            in_vpd = True
            continue
        if not in_vpd:
            continue
        if stripped.startswith('Capabilities:') or (stripped and not line[0].isspace()):
            break
        if '[SN]' in stripped and ':' in stripped:
            sn = stripped.split(':', 1)[1].strip()
        elif '[V3]' in stripped and ':' in stripped:
            v3 = stripped.split(':', 1)[1].strip()
    return sn, v3


def _group_nic_ports(nic_bdfs: List[str]) -> Dict[str, List[str]]:
    """Group NIC BDFs by slot (domain:bus:device).

    Returns {primary_bdf: [all_port_bdfs]} where primary is function 0.
    """
    slots = {}
    for bdf in sorted(nic_bdfs):
        slot = _bdf_slot(bdf)
        slots.setdefault(slot, []).append(bdf)

    grouped = {}
    for slot, bdfs in slots.items():
        primary = f"{slot}.0"
        if primary not in bdfs:
            primary = bdfs[0]
        grouped[primary] = sorted(bdfs)
    return grouped


def _get_numa_node(bdf: str) -> Optional[int]:
    val = _read_sysfs_attr(bdf, 'numa_node')
    try:
        n = int(val)
        return n if n >= 0 else None
    except (TypeError, ValueError):
        return None


def _sysfs_device_path(bdf: str) -> Optional[List[str]]:
    """Resolve a PCI BDF to its full sysfs device path.

    E.g. '0000:03:00.0' -> '/sys/devices/pci0000:00/0000:00:00.0/.../0000:03:00.0'
    Returns the path components as a list, or None on failure.
    """
    try:
        real = os.path.realpath(f'/sys/bus/pci/devices/{bdf}')
        return real.split('/')
    except OSError:
        return None


def _pcie_distance(bdf_a: str, bdf_b: str) -> Optional[int]:
    """Compute PCIe topology distance between two PCI devices.

    Walks the sysfs device paths upward to find the nearest common ancestor.
    Distance = hops from A to ancestor + hops from B to ancestor.

    Returns distance (int) or None on failure.
    """
    path_a = _sysfs_device_path(bdf_a)
    path_b = _sysfs_device_path(bdf_b)
    if path_a is None or path_b is None:
        return None

    common_len = 0
    for i, (a, b) in enumerate(zip(path_a, path_b)):
        if a != b:
            break
        common_len = i + 1

    if common_len == 0:
        return None

    return (len(path_a) - common_len) + (len(path_b) - common_len)


def _find_closest_gpu(nic_bdf: str,
                      gpu_bdfs: List[str]) -> Tuple[Optional[str], Optional[int]]:
    """Find the GPU with the shortest PCIe topology distance to a NIC.

    Uses a composite sort key: (pcie_distance, numa_mismatch) so that
    when PCIe distances are equal (typical for cross-domain devices),
    GPUs on the same NUMA node are preferred.

    Returns (gpu_bdf, distance) or (None, None) if no GPU is reachable.
    """
    nic_numa = _get_numa_node(nic_bdf)
    candidates = []
    for gpu_bdf in gpu_bdfs:
        dist = _pcie_distance(nic_bdf, gpu_bdf)
        if dist is None:
            continue
        gpu_numa = _get_numa_node(gpu_bdf)
        numa_mismatch = 0 if (nic_numa is not None and gpu_numa == nic_numa) else 1
        candidates.append((dist, numa_mismatch, gpu_bdf))

    if not candidates:
        return None, None

    candidates.sort()
    best_dist, _, best_gpu = candidates[0]
    return best_gpu, best_dist


def _read_vpd_bulk(bdfs: List[str]) -> Tuple[Dict[str, Tuple[str, str]], List[str]]:
    """Read VPD from multiple devices. Returns (vpd_dict, failure_list)."""
    vpd, failures = {}, []
    for bdf in bdfs:
        sn, v3 = _read_vpd(bdf)
        if sn is None or v3 is None:
            failures.append(bdf)
        else:
            vpd[bdf] = (sn, v3)
    return vpd, failures


def _build_entry(nic_bdf: str, gpu_bdf: str, method: str,
                 nic_groups: Dict[str, List[str]],
                 rdma_map: Dict[str, List[str]],
                 details: Dict[str, Any]) -> DiscoveryEntry:
    ports = nic_groups[nic_bdf]
    return DiscoveryEntry(
        nic_bdf=nic_bdf,
        gpu_bdf=gpu_bdf,
        method=method,
        nic_ports=ports,
        rdma_devices=sorted(d for p in ports for d in rdma_map.get(p, [])),
        numa_node=_get_numa_node(nic_bdf),
        details=details,
    )


def discover_with_warnings() -> Tuple[List[DiscoveryEntry], List[str]]:
    """Discover NIC-to-GPU relationships and collect non-fatal warnings."""
    nic_bdfs, rdma_map = _find_rdma_nics()
    dma_bdfs = _find_dma_controllers()
    gpu_bdfs = _find_gpus()
    warnings: List[str] = []

    if not nic_bdfs:
        warnings.append("No RDMA-capable NICs found in /sys/class/infiniband/.")
        return [], warnings
    if not gpu_bdfs:
        warnings.append("No GPUs found.")
        return [], warnings

    nic_groups = _group_nic_ports(nic_bdfs)

    gpu_by_domain = {}
    for bdf in gpu_bdfs:
        gpu_by_domain.setdefault(_bdf_domain(bdf), []).append(bdf)

    # --- Pass 1: DMA-based matching via VPD ---
    nic_vpd, dma_vpd = {}, {}
    if dma_bdfs:
        nic_vpd, nic_failures = _read_vpd_bulk(nic_groups.keys())
        dma_vpd, dma_failures = _read_vpd_bulk(dma_bdfs)
        vpd_failures = nic_failures + dma_failures
        if vpd_failures:
            warnings.append(
                f"WARNING: Could not read VPD [SN]/[V3] from: "
                f"{', '.join(vpd_failures)}\n"
                f"These NICs will fall back to PCIe topology matching. "
                f"For DMA-based matching, re-run with root permissions."
            )

    results = []
    dma_matched_nics = set()
    matched_dmas = set()

    for nic_bdf in sorted(nic_vpd):
        nic_key = nic_vpd[nic_bdf]
        matched_dma = None
        for dma_bdf, dma_key in dma_vpd.items():
            if dma_bdf in matched_dmas:
                continue
            if nic_key == dma_key:
                matched_dma = dma_bdf
                matched_dmas.add(dma_bdf)
                break

        if matched_dma is None:
            continue

        dma_domain = _bdf_domain(matched_dma)
        domain_gpus = gpu_by_domain.get(dma_domain, [])
        gpu_bdf = domain_gpus[0] if domain_gpus else None

        if gpu_bdf is None:
            continue

        dma_matched_nics.add(nic_bdf)
        results.append(_build_entry(nic_bdf, gpu_bdf, 'dma', nic_groups, rdma_map, {
            'dma_bdf': matched_dma,
            'vpd_sn': nic_key[0],
            'vpd_v3': nic_key[1],
        }))

    # --- Pass 2: PCIe topology fallback for unmatched NICs ---
    for nic_bdf in sorted(nic_groups):
        if nic_bdf in dma_matched_nics:
            continue

        gpu_bdf, distance = _find_closest_gpu(nic_bdf, gpu_bdfs)
        if gpu_bdf is None:
            continue

        results.append(_build_entry(nic_bdf, gpu_bdf, 'topology', nic_groups, rdma_map, {
            'pcie_distance': distance,
        }))

    return results, warnings


def discover() -> List[Dict[str, Any]]:
    """Discover NIC-to-GPU relationships. Returns JSON-compatible dicts."""
    entries, warnings = discover_with_warnings()
    for warning in warnings:
        print(warning, file=sys.stderr)
    return [entry.to_dict() for entry in entries]


def lookup_gpu(discovery_results: List[Any], rdma_device: str) -> Optional[str]:
    """GPU BDF for the entry whose rdma_devices contains `rdma_device`
    (e.g. 'mlx5_0'), or None if no match. Accepts DiscoveryEntry or dict."""
    for entry in discovery_results:
        rdma_devices = (entry.rdma_devices if isinstance(entry, DiscoveryEntry)
                        else entry['rdma_devices'])
        if rdma_device in rdma_devices:
            return entry.gpu_bdf if isinstance(entry, DiscoveryEntry) else entry['gpu_bdf']
    return None


def main():
    try:
        print(json.dumps(discover()))
    except Exception as e:
        print(json.dumps({'error': str(e)}), file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    main()

"""
config_parser.py - Parse JSON config and expand hostname patterns
"""

import json
import re
from typing import List, Dict, Any, NamedTuple


_RANGE_RE = re.compile(r'\[(\d+)-(\d+)\]')
_LIST_RE = re.compile(r'\[([^\]]+)\]')


class ExpandedNodes(NamedTuple):
    """Result of expanding testNodes[] into per-host maps.

    Each map's value is normally a single (uniform) value applied to every
    occurrence of that hostname. If the same hostname appears more than once
    in testNodes[] with *different* values for a field (e.g. two RDMA NICs
    on one physical host), that value becomes a list instead, ordered by
    occurrence - see traffic_patterns.resolve_occurrence_aware().
    """
    hosts: List[str]
    device_map: Dict[str, Any]         # str -> str, or str -> List[str]
    gpu_map: Dict[str, Any]            # str -> dict, or str -> List[dict]
    perftest_args_map: Dict[str, Any]  # str -> str, or str -> List[str]
    typed_fields_map: Dict[str, Any]   # str -> dict, or str -> List[dict]
    peer_address_map: Dict[str, Any]   # str -> str, or str -> List[str]


def expand_hostnames(pattern: str) -> List[str]:
    """Expand a hostname pattern into a list of concrete hostnames.

    Supports two bracket notations:
      - Range:  "node-[01-04]"       -> ["node-01", "node-02", "node-03", "node-04"]
      - List:   "node-[a,b,c]"       -> ["node-a", "node-b", "node-c"]

    Zero-padding width is preserved from the range start value.
    A plain hostname without brackets is returned as a single-element list.
    """
    range_match = _RANGE_RE.search(pattern)
    if range_match:
        start = int(range_match.group(1))
        end = int(range_match.group(2))
        if start > end:
            raise ValueError(
                f"Invalid hostname range in {pattern!r}: "
                f"start {start} is greater than end {end}")
        width = len(range_match.group(1))
        prefix = pattern[:range_match.start()]
        suffix = pattern[range_match.end():]
        return [f"{prefix}{str(i).zfill(width)}{suffix}"
                for i in range(start, end + 1)]

    list_match = _LIST_RE.search(pattern)
    if list_match:
        items = [x.strip() for x in list_match.group(1).split(',')]
        prefix = pattern[:list_match.start()]
        suffix = pattern[list_match.end():]
        return [f"{prefix}{item}{suffix}" for item in items]

    return [pattern]


def split_host_list(hosts_str: str) -> List[str]:
    """Split a comma-separated host string, respecting brackets.

    Commas inside brackets are part of the pattern and not delimiters:
      "a,b"                  -> ["a", "b"]
      "node-[1,2,3],other"   -> ["node-[1,2,3]", "other"]
      "node-[01-04],x-[a,b]" -> ["node-[01-04]", "x-[a,b]"]
    """
    parts: List[str] = []
    current: List[str] = []
    depth = 0
    for ch in hosts_str:
        if ch == '[':
            depth += 1
            current.append(ch)
        elif ch == ']':
            depth -= 1
            current.append(ch)
        elif ch == ',' and depth == 0:
            parts.append(''.join(current).strip())
            current = []
        else:
            current.append(ch)
    if current:
        parts.append(''.join(current).strip())
    return [p for p in parts if p]


def _validate_gpu_device_id(value: Any) -> int:
    """Validate the optional gpuDeviceId from JSON; default to -1 when unset."""
    if value is None or value == '':
        return -1
    # bool is an int subclass; reject it explicitly like iterations does.
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError("testNodes[].gpuDeviceId must be an integer")
    if value < -1:
        raise ValueError("testNodes[].gpuDeviceId must be -1 or >= 0")
    return value


def _accumulate(target: Dict[str, Any], key: str, value: Any) -> None:
    """Record a per-host value; upgrade to a list on a second occurrence.

    A single occurrence stays a plain scalar/dict. Repeats become a list,
    appended in encounter order (see ExpandedNodes), regardless of whether
    the values are equal.
    """
    if key not in target:
        target[key] = value
    elif isinstance(target[key], list):
        target[key].append(value)
    else:
        target[key] = [target[key], value]


def expand_test_nodes(test_nodes: List[Dict[str, Any]]) -> "ExpandedNodes":
    """Expand testNode entries into host list, device/GPU maps, and per-host args.

    Each entry may contain:
      - "hostName": a hostname or pattern (expanded via expand_hostnames)
      - "deviceName": optional RDMA device (e.g. "mlx5_0")
      - "gpuType": optional GPU type ('cuda', 'rocm', 'neuron', 'hl', 'mlu', 'opencl')
      - "gpuDeviceId": optional numeric GPU device index (int)
      - "gpuDeviceBusId": optional PCIe bus ID string, takes precedence over gpuDeviceId
      - "perftestArgs": optional extra args appended after global perftestArgs
      - "peerAddress": optional connect address for peers of this host, distinct
        from hostName (which is only used for mpirun placement/SSH)
      - any name from TYPED_PERFTEST_FIELDS (typed perftest knobs)

    Returns:
        An ExpandedNodes named tuple - see its docstring for the map shapes.
        `hosts` may repeat a hostname (e.g. to mimic more logical hosts, or
        to drive multiple local RDMA NICs from one host).
    """
    # Deferred import to avoid a circular dependency with config_model.
    from .config_model import TYPED_PERFTEST_FIELDS
    typed_field_names = [f.name for f in TYPED_PERFTEST_FIELDS]

    hosts: List[str] = []
    device_map: Dict[str, Any] = {}
    gpu_map: Dict[str, Any] = {}
    perftest_args_map: Dict[str, Any] = {}
    typed_fields_map: Dict[str, Any] = {}
    peer_address_map: Dict[str, Any] = {}
    for entry in test_nodes:
        expanded = expand_hostnames(entry.get('hostName', ''))
        device = entry.get('deviceName', '')
        gpu_type = entry.get('gpuType', '')
        gpu_device_id = _validate_gpu_device_id(entry.get('gpuDeviceId', -1))
        gpu_device_bus_id = entry.get('gpuDeviceBusId', '')
        perftest_args = entry.get('perftestArgs', '')
        peer_address = entry.get('peerAddress', '')
        node_typed = {n: entry[n] for n in typed_field_names if n in entry}
        for h in expanded:
            hosts.append(h)
            if device:
                _accumulate(device_map, h, device)
            if perftest_args:
                _accumulate(perftest_args_map, h, perftest_args)
            if peer_address:
                _accumulate(peer_address_map, h, peer_address)
            if gpu_type or gpu_device_id >= 0 or gpu_device_bus_id:
                _accumulate(gpu_map, h, {
                    'gpuType': gpu_type,
                    'gpuDeviceId': gpu_device_id,
                    'gpuDeviceBusId': gpu_device_bus_id,
                })
            if node_typed:
                _accumulate(typed_fields_map, h, dict(node_typed))
    return ExpandedNodes(
        hosts, device_map, gpu_map, perftest_args_map, typed_fields_map,
        peer_address_map)


def parse_config(config_path: str) -> Dict[str, Any]:
    """Load a JSON configuration file without expanding test entries."""
    with open(config_path, 'r', encoding='utf-8') as f:
        return json.load(f)


def _validate_iterations(value: Any) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError("tests[].iterations must be an integer >= 1")
    if value < 1:
        raise ValueError("tests[].iterations must be >= 1")
    return value


def expand_test_configs(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Expand top-level tests[] into sequential single-test configs.

    Root-level fields act as defaults; each tests[] entry overrides them.
    Each test entry may include iterations=N, which expands to N independent
    run configs with iteration metadata.
    """
    tests = config.get('tests')
    root_defaults = {k: v for k, v in config.items() if k != 'tests'}
    if tests is None or tests == []:
        return [root_defaults]
    if not isinstance(tests, list):
        raise ValueError("tests must be an array")

    expanded: List[Dict[str, Any]] = []
    for test in tests:
        if not isinstance(test, dict):
            raise ValueError("tests[] entries must be objects")
        iterations = _validate_iterations(test.get('iterations', 1))
        test_body = dict(test)
        test_body.pop('iterations', None)
        for iteration in range(1, iterations + 1):
            expanded.append({
                **root_defaults,
                **test_body,
                'iteration': iteration,
                'iterations': iterations,
            })
    return expanded

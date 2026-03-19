"""
traffic_patterns.py - Resolve traffic patterns to process/rank assignments

MPI rank order MUST list every server rank before any client rank so mpirun starts TCP
listeners before clients connect (required for perftest handshake).

Rank numbering starts at 1. Rank 0 is the native C `perftest_cluster_worker`
binary (src/perftest_cluster_worker.c) which runs as MPI rank 0 on the
launcher host.

Each pattern is a "pair generator" returning (server_pos, client_pos,
connection_id) tuples indexing into `hosts` by position rather than hostname
value - this is what lets the same hostname appear more than once with a
distinct identity per occurrence (e.g. one host driving two RDMA NICs; see
`RankAssignment.node_index` and `resolve_occurrence_aware`).
"""

from enum import Enum
from dataclasses import dataclass, field, replace
from typing import Any, Callable, List, Dict, Optional, Tuple


class TrafficPattern(Enum):
    ONE_TO_ONE = "O2O"
    ONE_TO_MANY = "O2M"
    MANY_TO_ONE = "M2O"
    ALL_TO_ALL = "A2A"
    BISECTION = "B"
    RING = "R"


@dataclass
class RankAssignment:
    rank: int
    hostname: str
    role: str               # 'server' or 'client'
    port: int
    peer_host: str
    connection_id: int
    device: str = ''
    stream_index: int = 0   # 0-based index within this connection's streams
    gpu_type: str = ''           # 'cuda', 'rocm', 'neuron', 'hl', 'mlu', 'opencl'
    gpu_device_id: int = -1      # -1 = not assigned
    gpu_device_bus_id: str = ''  # PCIe bus ID (preferred for data_direct)
    perftest_args: str = ''      # Extra per-host perftest args appended after global args
    # Resolved typed perftest fields for this rank (per-host > top-level).
    typed_perftest_fields: Dict[str, Any] = field(default_factory=dict)
    # Position in `hosts` this assignment came from; -1 if unset (e.g. built
    # directly in a test). See resolve_occurrence_aware() below.
    node_index: int = -1
    # Position in `hosts` the peer (peer_host) came from; -1 if unset. Lets
    # peer_address resolve to the exact peer occurrence, not just its hostname.
    peer_node_index: int = -1
    # Connect address for peer_host, resolved from peerAddress (e.g. the
    # peer's RDMA-capable interface IP). '' = fall back to peer_host.
    peer_address: str = ''


# A connection: (server_pos, client_pos, connection_id) - positions index
# into the `hosts` list passed to resolve_pattern().
ConnPair = Tuple[int, int, int]


def _build_from_pairs(pairs: List[ConnPair], hosts: List[str],
                      base_port: int) -> List[RankAssignment]:
    """Convert connection pairs to RankAssignments (servers before clients).

    Port for connection N is `base_port + N`. Ranks start at 1.
    """
    assignments: List[RankAssignment] = []
    rank = 1

    for server_pos, client_pos, cid in pairs:
        assignments.append(RankAssignment(
            rank=rank, hostname=hosts[server_pos], role='server',
            port=base_port + cid, peer_host=hosts[client_pos], connection_id=cid,
            node_index=server_pos, peer_node_index=client_pos))
        rank += 1

    for server_pos, client_pos, cid in pairs:
        assignments.append(RankAssignment(
            rank=rank, hostname=hosts[client_pos], role='client',
            port=base_port + cid, peer_host=hosts[server_pos], connection_id=cid,
            node_index=client_pos, peer_node_index=server_pos))
        rank += 1

    return assignments


# --- Pair generators (one per traffic pattern) -----------------------------

def _pairs_one_to_one(hosts: List[str]) -> List[ConnPair]:
    if len(hosts) != 2:
        raise ValueError("ONE_TO_ONE requires exactly 2 hosts")
    return [(0, 1, 0)]


def _pairs_one_to_many(hosts: List[str]) -> List[ConnPair]:
    """First host is the 'one' (perftest client/initiator); the rest are servers."""
    if len(hosts) < 2:
        raise ValueError("ONE_TO_MANY requires at least 2 hosts")
    return [(i, 0, i - 1) for i in range(1, len(hosts))]


def _pairs_many_to_one(hosts: List[str]) -> List[ConnPair]:
    """First host is the 'one' (server hub); the rest are clients."""
    if len(hosts) < 2:
        raise ValueError("MANY_TO_ONE requires at least 2 hosts")
    return [(0, i, i - 1) for i in range(1, len(hosts))]


def _pairs_all_to_all(hosts: List[str]) -> List[ConnPair]:
    """Every ordered pair (s, c) where hosts[s] != hosts[c] becomes one connection.

    Comparing the dereferenced hostnames (not positions) preserves the
    existing "exclude same-hostname pairs" behavior even when a hostname
    occupies more than one position (multi-NIC same host): two occurrences
    of the same physical host never get paired with each other, but each
    still connects to every *other* hostname's occurrences.
    """
    if len(hosts) < 2:
        raise ValueError("ALL_TO_ALL requires at least 2 hosts")
    n = len(hosts)
    ordered = [(s, c) for s in range(n) for c in range(n) if hosts[s] != hosts[c]]
    return [(s, c, cid) for cid, (s, c) in enumerate(ordered)]


def _pairs_bisection(hosts: List[str]) -> List[ConnPair]:
    """Split in half: second half are servers, first half are clients."""
    if len(hosts) < 2 or len(hosts) % 2 != 0:
        raise ValueError("BISECTION requires even number of hosts (minimum 2)")
    mid = len(hosts) // 2
    return [(mid + i, i, i) for i in range(mid)]


def _pairs_ring(hosts: List[str]) -> List[ConnPair]:
    """Each host i sends to host (i+1) % n.

    Connection cid=i is "server hosts[(i+1) % n]" <- "client hosts[i]".
    """
    if len(hosts) < 3:
        raise ValueError("RING requires at least 3 hosts (use O2O for 2 hosts)")
    n = len(hosts)
    return [((i + 1) % n, i, i) for i in range(n)]


_PATTERN_TO_PAIRS: Dict[TrafficPattern, Callable[[List[str]], List[ConnPair]]] = {
    TrafficPattern.ONE_TO_ONE:  _pairs_one_to_one,
    TrafficPattern.ONE_TO_MANY: _pairs_one_to_many,
    TrafficPattern.MANY_TO_ONE: _pairs_many_to_one,
    TrafficPattern.ALL_TO_ALL:  _pairs_all_to_all,
    TrafficPattern.BISECTION:   _pairs_bisection,
    TrafficPattern.RING:        _pairs_ring,
}


# --- Occurrence-aware per-host value resolution ----------------------------
#
# device_map/gpu_map/perftest_args_map/typed_fields_map/peer_address_map are
# hostname-keyed dicts holding either a single (uniform) value or, if
# config_parser upgraded it (see ExpandedNodes), a per-occurrence list. The
# helpers below resolve either shape.

def build_occurrence_map(pairs: List[Tuple[str, int]]) -> Dict[int, int]:
    """Map each (hostname, node_index) to a 0-based occurrence number among
    all node_index values seen for that hostname, ascending. Duplicate
    pairs are collapsed; `pairs` need not be sorted.
    """
    positions_by_host: Dict[str, List[int]] = {}
    for hostname, idx in pairs:
        bucket = positions_by_host.setdefault(hostname, [])
        if idx not in bucket:
            bucket.append(idx)
    occurrence: Dict[int, int] = {}
    for positions in positions_by_host.values():
        for occ_num, pos in enumerate(sorted(positions)):
            occurrence[pos] = occ_num
    return occurrence


def resolve_occurrence_aware(value_map: Dict[str, Any], hostname: str,
                             node_index: int, occurrence_map: Dict[int, int],
                             default: Any) -> Any:
    """Resolve `value_map[hostname]`, honoring a per-occurrence list value.

    A plain value applies uniformly to every occurrence of `hostname`. A
    list value supplies one entry per occurrence, in declaration order;
    `node_index`/`occurrence_map` (see build_occurrence_map()) pick which.
    """
    val = value_map.get(hostname, default)
    if isinstance(val, list):
        if not val:
            return default
        occ = occurrence_map.get(node_index, 0)
        return val[occ] if occ < len(val) else val[-1]
    return val


# --- Public API -----------------------------------------------------------

def resolve_pattern(hosts: List[str], pattern: TrafficPattern,
                    base_port: int = 18515,
                    device_map: Optional[Dict[str, Any]] = None,
                    streams: int = 1,
                    gpu_map: Optional[Dict[str, Any]] = None,
                    peer_address_map: Optional[Dict[str, Any]] = None
                    ) -> List[RankAssignment]:
    pair_fn = _PATTERN_TO_PAIRS.get(pattern)
    if pair_fn is None:
        raise ValueError(f"Unknown pattern: {pattern}")

    assignments = _build_from_pairs(pair_fn(hosts), hosts, base_port)

    if streams > 1:
        assignments = _multiply_streams(assignments, streams, base_port)

    if device_map or gpu_map or peer_address_map:
        occ_map = build_occurrence_map(
            [(a.hostname, a.node_index) for a in assignments])

    if device_map:
        for a in assignments:
            a.device = resolve_occurrence_aware(
                device_map, a.hostname, a.node_index, occ_map, '')

    if gpu_map:
        for a in assignments:
            g = resolve_occurrence_aware(
                gpu_map, a.hostname, a.node_index, occ_map, None)
            if g:
                a.gpu_type = g.get('gpuType', '')
                a.gpu_device_id = g.get('gpuDeviceId', -1)
                a.gpu_device_bus_id = g.get('gpuDeviceBusId', '')

    if peer_address_map:
        for a in assignments:
            a.peer_address = resolve_occurrence_aware(
                peer_address_map, a.peer_host, a.peer_node_index, occ_map, '')

    return assignments


def _multiply_streams(base_assignments: List[RankAssignment],
                      streams: int, base_port: int) -> List[RankAssignment]:
    """Replicate each connection into N streams on consecutive ports.

    Takes the single-stream assignments (servers first, then clients) and
    produces N copies of each, maintaining servers-before-clients ordering.
    Port formula: base_port + (connection_id * streams) + stream_index
    Ranks start at 1 (rank 0 is the perftest_cluster_worker C binary).
    """
    servers = [a for a in base_assignments if a.role == 'server']
    clients = [a for a in base_assignments if a.role == 'client']

    result: List[RankAssignment] = []
    rank = 1

    for group in (servers, clients):
        for base in group:
            for s in range(streams):
                # typed_perftest_fields is re-copied to avoid a shared dict.
                result.append(replace(
                    base,
                    rank=rank,
                    port=base_port + (base.connection_id * streams) + s,
                    stream_index=s,
                    typed_perftest_fields=dict(base.typed_perftest_fields)))
                rank += 1

    return result

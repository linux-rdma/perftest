"""
result_schema.py - Typed dataclasses for the perftest_cluster_worker JSON result file.

The native C `perftest_cluster_worker` (src/perftest_cluster_worker.c) writes
a JSON file to `--output-file` after MPI_Gather; the orchestrator reads it
after mpirun exits. This module is the single source of truth for that JSON
contract on the Python side — keep the field names, types, and defaults in
sync with the emit_json_bw / emit_json_lat functions in
perftest_cluster_worker.c.

Two top-level shapes, distinguished by `result_kind`:

    BW:  {"result_kind": "bw",
          "size": <bytes>,
          "bw_unit": "MB/s" | "Gb/s",     # mirrors --report_gbits
          "ranks": [{"rank": N, "bw": F, "msgrate": F}, ...]}
                 # One entry per CLIENT-side worker (perftest_cluster_worker
                 # filters role==1). Half-duplex: bw/msgrate are the client's
                 # own measurement (the only side that measures anything real).
                 # Duplex (-b): each side measures its own direction, so
                 # bw/msgrate are the SUM of the client's and its paired
                 # server's measurements (see emit_json_bw() in
                 # perftest_cluster_worker.c), matching standalone perftest's
                 # total bidirectional figure.

    LAT: {"result_kind": "lat",
          "workers": [{"rank": N, "role": N, "size": N, "iters": N,
                       "test_type": N, "t_avg": F, "t_min": F, "t_max": F,
                       "t_typical": F, "stdev": F, "p99": F, "p99_9": F,
                       "tps": F}, ...]}
                 # all workers; orchestrator filters by role at render time

Use `parse_result(data)` to convert a freshly `json.load`-ed dict into the
appropriate typed result. Robust to missing / extra keys: missing fields
fall back to type-appropriate defaults so a malformed result file fails
gracefully (rendering shows zeros) rather than KeyError-ing.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List, Union


def _as_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _as_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _dict_items(value: Any) -> List[Dict[str, Any]]:
    """Dict entries from a JSON list, ignoring malformed shapes."""
    if not isinstance(value, list):
        return []
    return [item for item in value if isinstance(item, dict)]


# ---------------------------------------------------------------------------
# BW
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class BwRankResult:
    """One CLIENT-side perftest worker's BW result.

    Note: the unit of `bw` is whatever perftest computed (MB/s by default,
    Gb/s if --report_gbits was passed). The actual unit string is on the
    parent BwResult.bw_unit field — render it from there, never hard-code.
    """
    rank: int
    bw: float       # value in BwResult.bw_unit
    msgrate: float  # Mpps, always; rounded to 6 decimals by perftest_cluster_worker

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> BwRankResult:
        return cls(
            rank=_as_int(d.get('rank')),
            bw=_as_float(d.get('bw')),
            msgrate=_as_float(d.get('msgrate')),
        )


@dataclass(frozen=True)
class DvWorker:
    """One validating rank's data-validation result.

    Receivers only: server for half-duplex WRITE, client for half-duplex READ,
    both peers for duplex.
    """
    rank: int
    role: int           # 0 = server, 1 = client
    passed: bool
    errors: int
    bytes_validated: int
    chunks: int

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> DvWorker:
        return cls(
            rank=_as_int(d.get('rank')),
            role=_as_int(d.get('role')),
            passed=bool(d.get('passed', 0)),
            errors=_as_int(d.get('errors')),
            bytes_validated=_as_int(d.get('bytes')),
            chunks=_as_int(d.get('chunks')),
        )


@dataclass(frozen=True)
class DvSummary:
    """Aggregate data-validation status for a run (top-level JSON block)."""
    enabled: bool = False
    passed: bool = True
    errors: int = 0
    workers: List[DvWorker] = field(default_factory=list)

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> DvSummary:
        if not isinstance(d, dict) or not d.get('enabled'):
            return cls()
        return cls(
            enabled=True,
            passed=bool(d.get('passed', 0)),
            errors=_as_int(d.get('errors')),
            workers=[DvWorker.from_dict(w) for w in _dict_items(d.get('workers'))],
        )


@dataclass(frozen=True)
class BwResult:
    size: int = 0             # message size in bytes
    # 'MB/s' or 'Gb/s'; default tracks perftest's own default (MBS) so old
    # perftest_cluster_worker JSON files still render correctly.
    bw_unit: str = 'MB/s'
    ranks: List[BwRankResult] = field(default_factory=list)
    data_validation: DvSummary = field(default_factory=DvSummary)
    result_kind: str = 'bw'

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> BwResult:
        return cls(
            size=_as_int(d.get('size')),
            bw_unit=str(d.get('bw_unit', 'MB/s')),
            ranks=[BwRankResult.from_dict(r)
                   for r in _dict_items(d.get('ranks'))],
            data_validation=DvSummary.from_dict(d.get('data_validation')),
        )


# ---------------------------------------------------------------------------
# LAT
# ---------------------------------------------------------------------------

# Roles emitted by perftest into cluster_lat_report.role
ROLE_SERVER = 0
ROLE_CLIENT = 1

# Test types emitted into cluster_lat_report.test_type
TEST_TYPE_ITERATIONS = 0
TEST_TYPE_DURATION = 1


@dataclass(frozen=True)
class LatWorkerResult:
    """One worker's latency result. Both SERVER and CLIENT roles are
    emitted by perftest_cluster_worker; the orchestrator filters by role at render
    time. In duration mode only `t_avg` and `tps` are populated."""
    rank: int
    role: int           # ROLE_SERVER (0) or ROLE_CLIENT (1)
    size: int
    iters: int
    test_type: int      # TEST_TYPE_ITERATIONS or TEST_TYPE_DURATION
    t_avg: float        # usec
    t_min: float        # usec, iter mode only
    t_max: float        # usec, iter mode only
    t_typical: float    # usec, iter mode only (median)
    stdev: float        # usec, iter mode only
    p99: float          # usec, iter mode only
    p99_9: float        # usec, iter mode only
    tps: float          # transactions/sec, duration mode only

    @property
    def is_duration(self) -> bool:
        return self.test_type == TEST_TYPE_DURATION

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> LatWorkerResult:
        return cls(
            rank=_as_int(d.get('rank')),
            role=_as_int(d.get('role')),
            size=_as_int(d.get('size')),
            iters=_as_int(d.get('iters')),
            test_type=_as_int(d.get('test_type'), TEST_TYPE_ITERATIONS),
            t_avg=_as_float(d.get('t_avg')),
            t_min=_as_float(d.get('t_min')),
            t_max=_as_float(d.get('t_max')),
            t_typical=_as_float(d.get('t_typical')),
            stdev=_as_float(d.get('stdev')),
            p99=_as_float(d.get('p99')),
            p99_9=_as_float(d.get('p99_9')),
            tps=_as_float(d.get('tps')),
        )


@dataclass(frozen=True)
class LatResult:
    workers: List[LatWorkerResult] = field(default_factory=list)
    result_kind: str = 'lat'

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> LatResult:
        return cls(
            workers=[LatWorkerResult.from_dict(w)
                     for w in _dict_items(d.get('workers'))],
        )

    def clients(self) -> List[LatWorkerResult]:
        return [w for w in self.workers if w.role == ROLE_CLIENT]


# ---------------------------------------------------------------------------
# Dispatcher
# ---------------------------------------------------------------------------

ClusterResult = Union[BwResult, LatResult]


def parse_result(data: Dict[str, Any]) -> ClusterResult:
    kind = data.get('result_kind', 'bw')
    if kind == 'lat':
        return LatResult.from_dict(data)
    if kind not in ('bw', None, ''):
        raise ValueError(f"Unknown result_kind: {kind}")
    return BwResult.from_dict(data)

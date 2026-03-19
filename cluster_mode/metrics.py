"""
metrics.py - Connection grouping and aggregation helpers for cluster results.
"""

from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Dict, Iterable, List

from .result_schema import BwRankResult, LatWorkerResult
from .traffic_patterns import RankAssignment


@dataclass(frozen=True)
class BwStream:
    bw: float
    msgrate: float
    port: int
    stream_index: int


@dataclass
class BwConnection:
    src: str
    dst: str
    streams: List[BwStream] = field(default_factory=list)

    @property
    def bw_total(self) -> float:
        return sum(s.bw for s in self.streams)

    @property
    def msgrate_total(self) -> float:
        return sum(s.msgrate for s in self.streams)


@dataclass(frozen=True)
class LatStream:
    data: LatWorkerResult
    port: int
    stream_index: int


@dataclass
class LatConnection:
    src: str
    dst: str
    streams: List[LatStream] = field(default_factory=list)


def logical_connection_count(assignments: Iterable[RankAssignment]) -> int:
    return len({a.connection_id for a in assignments if a.role == 'server'})


def rank_map(assignments: Iterable[RankAssignment]) -> Dict[int, RankAssignment]:
    return {a.rank: a for a in assignments}


def _positive(values: Iterable[float]) -> List[float]:
    return [v for v in values if v > 0]


def _unmatched_connection_id(rank: int) -> int:
    """Keep unmatched result ranks distinct instead of merging under -1."""
    return -rank if rank > 0 else -1


def group_bw_connections(
    ranks: Iterable[BwRankResult],
    assignments: Iterable[RankAssignment],
) -> "OrderedDict[int, BwConnection]":
    rank_to_assignment = rank_map(assignments)
    grouped: "OrderedDict[int, BwConnection]" = OrderedDict()

    for result in ranks:
        assignment = rank_to_assignment.get(result.rank)
        cid = (assignment.connection_id if assignment
               else _unmatched_connection_id(result.rank))
        if cid not in grouped:
            grouped[cid] = BwConnection(
                src=assignment.hostname if assignment else f"rank-{result.rank}",
                dst=assignment.peer_host if assignment else '?',
                streams=[],
            )
        grouped[cid].streams.append(BwStream(
            bw=result.bw,
            msgrate=result.msgrate,
            port=assignment.port if assignment else 0,
            stream_index=assignment.stream_index if assignment else 0,
        ))

    return grouped


def group_lat_connections(
    clients: Iterable[LatWorkerResult],
    assignments: Iterable[RankAssignment],
) -> "OrderedDict[int, LatConnection]":
    rank_to_assignment = rank_map(assignments)
    grouped: "OrderedDict[int, LatConnection]" = OrderedDict()

    for result in clients:
        assignment = rank_to_assignment.get(result.rank)
        cid = (assignment.connection_id if assignment
               else _unmatched_connection_id(result.rank))
        if cid not in grouped:
            grouped[cid] = LatConnection(
                src=assignment.hostname if assignment else f"rank-{result.rank}",
                dst=assignment.peer_host if assignment else '?',
                streams=[],
            )
        grouped[cid].streams.append(LatStream(
            data=result,
            port=assignment.port if assignment else 0,
            stream_index=assignment.stream_index if assignment else 0,
        ))

    return grouped


def lat_connection_stats(conn: LatConnection, is_duration: bool) -> Dict[str, float]:
    datas = [s.data for s in conn.streams]
    if not datas:
        return {}

    t_avgs = _positive(d.t_avg for d in datas)
    stats = {
        't_avg': sum(t_avgs) / len(t_avgs) if t_avgs else 0.0,
        't_avg_worst': max(t_avgs) if t_avgs else 0.0,
    }
    if is_duration:
        stats['tps'] = sum(d.tps for d in datas)
    else:
        t_typicals = _positive(d.t_typical for d in datas)
        stats.update({
            't_typical': (sum(t_typicals) / len(t_typicals)
                          if t_typicals else 0.0),
            't_min': min((d.t_min for d in datas if d.t_min > 0),
                         default=0.0),
            't_max': max(d.t_max for d in datas),
            'p99': max(d.p99 for d in datas),
            'p99_9': max(d.p99_9 for d in datas),
        })
    return stats


def lat_summary_stats(clients: Iterable[LatWorkerResult],
                      is_duration: bool) -> Dict[str, float]:
    client_list = list(clients)
    t_avgs = [c.t_avg for c in client_list if c.t_avg > 0]
    stats = {
        't_avg': sum(t_avgs) / len(t_avgs) if t_avgs else 0.0,
        't_avg_worst': max(t_avgs) if t_avgs else 0.0,
    }
    if is_duration:
        stats['tps_total'] = sum(c.tps for c in client_list)
    else:
        stats['t_min'] = min((c.t_min for c in client_list if c.t_min > 0),
                             default=0.0)
        stats['t_max'] = max((c.t_max for c in client_list), default=0.0)
    return stats

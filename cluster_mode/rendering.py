"""
rendering.py - Human and JSON rendering for cluster-mode results.
"""

from __future__ import annotations

import json
import sys
from datetime import datetime
from typing import Any, Dict, List, Optional

from .config_parser import expand_hostnames
from .metrics import (
    group_bw_connections,
    group_lat_connections,
    lat_connection_stats,
    lat_summary_stats,
    logical_connection_count,
)
from .mpi_command import _build_gpu_flags
from .perftest_output import parse_perftest_params
from .result_schema import BwResult, LatResult
from .traffic_patterns import (
    RankAssignment, build_occurrence_map, resolve_occurrence_aware,
)


def fio_section(label: str) -> str:
    return f"\n=== {label} ===\n"


def fio_kv(key: str, value: str, indent: int = 2) -> str:
    return f"{' ' * indent}{key:<14}: {value}"


def _to_int_if_possible(value: str) -> Any:
    try:
        return int(value)
    except ValueError:
        return value


def _as_gpu_device_id(value: Any) -> int:
    if isinstance(value, bool):
        return -1
    try:
        return int(value)
    except (TypeError, ValueError):
        return -1


def _cfg_for_json(cfg: Dict[str, Any]) -> Dict[str, Any]:
    """JSON-friendly copy of cfg: tuple devices -> dict list, ON/OFF -> bool."""
    out = dict(cfg)
    devices = out.get('devices') or []
    out['devices'] = [{'host': host, 'device': dev} for host, dev in devices]
    for key in ('bidirectional', 'rdma_cm', 'no_enhanced_reorder'):
        if key in out:
            out[key] = (out[key] == 'ON')
    return out


def build_test_config(config: Dict[str, Any],
                      assignments: List[RankAssignment],
                      gpu_map: Dict[str, Dict[str, Any]],
                      params_dict: Dict[str, str],
                      result: "BwResult | LatResult") -> Dict[str, Any]:
    unique_hosts = sorted({a.hostname for a in assignments})
    cfg: Dict[str, Any] = {
        'timestamp':    datetime.now().astimezone().isoformat(timespec='seconds'),
        'binary':       config.get('perftestBinary', ''),
        'pattern':      config.get('trafficPattern', 'O2O'),
        'hosts':        unique_hosts,
        'connections':  logical_connection_count(assignments),
        'streams':      config.get('streams', 1),
        'message_size': getattr(result, 'size', 0),
    }

    cfg['devices'] = sorted({(a.hostname, a.device) for a in assignments
                             if a.device})

    cfg['bidirectional']       = 'ON' if config.get('bidirectional')      else 'OFF'
    cfg['rdma_cm']             = 'ON' if config.get('useRdmaCm')          else 'OFF'
    cfg['no_enhanced_reorder'] = 'ON' if config.get('noEnhancedReorder')  else 'OFF'

    int_keys = {'qps', 'tx_depth', 'rx_depth', 'mtu', 'iters', 'duration',
                'post_list', 'cq_mod', 'gid_index', 'outstanding_reads'}
    for key in ('duration', 'iters', 'qps', 'tx_depth', 'rx_depth', 'mtu',
                'post_list', 'cq_mod', 'gid_index', 'outstanding_reads',
                'link_type', 'transport_type', 'connection_type', 'srq'):
        value = params_dict.get(key)
        if value is not None:
            cfg[key] = _to_int_if_possible(value) if key in int_keys else value

    cfg.setdefault('post_list', 1)
    cfg.setdefault('cq_mod', 1)

    gpu_type = config.get('gpuType', '')
    if gpu_type:
        cfg['gpu_type'] = gpu_type
    if config.get('dataDirectMode'):
        cfg['data_direct'] = True
        cfg['dmabuf'] = True
    elif config.get('cudaDmabuf'):
        cfg['dmabuf'] = True

    gpu_mapping = (_build_resolved_gpu_mapping(assignments, gpu_map)
                   or _build_gpu_mapping(config))
    if gpu_mapping:
        cfg['gpu_mapping'] = gpu_mapping
        mapped_types = {m['gpu_type'] for m in gpu_mapping if m['gpu_type']}
        if 'gpu_type' not in cfg and len(mapped_types) == 1:
            cfg['gpu_type'] = mapped_types.pop()

    if cfg.get('gpu_type') == 'cuda':
        cfg['gpu_memory'] = config.get('cudaMemType') or 'auto (vmm)'
    elif config.get('cudaMemType'):
        cfg['gpu_memory'] = config['cudaMemType']

    return cfg


def _configured_gpu_value(node: Dict[str, Any], config: Dict[str, Any],
                          key: str, default: Any) -> Any:
    return node[key] if key in node else config.get(key, default)


def _build_resolved_gpu_mapping(
    assignments: List[RankAssignment],
    gpu_map: Dict[str, Any],
) -> List[Dict[str, Any]]:
    """Build the per-node GPU mapping table shown in the rendered report.

    Deduplicates by (host, device) rather than just host, so a multi-NIC
    host gets one row per NIC instead of only its first occurrence.
    """
    mapping: List[Dict[str, Any]] = []
    seen = set()
    occ_map = build_occurrence_map(
        [(a.hostname, a.node_index) for a in assignments])
    for assignment in assignments:
        host = assignment.hostname
        key = (host, assignment.device)
        if key in seen:
            continue
        seen.add(key)

        configured = resolve_occurrence_aware(
            gpu_map, host, assignment.node_index, occ_map, {}) or {}
        gpu_type = assignment.gpu_type or configured.get('gpuType', '')
        dev_id = (assignment.gpu_device_id if assignment.gpu_device_id >= 0
                  else _as_gpu_device_id(configured.get('gpuDeviceId', -1)))
        bus_id = (assignment.gpu_device_bus_id
                  or configured.get('gpuDeviceBusId', ''))
        if gpu_type or dev_id >= 0 or bus_id:
            mapping.append({
                'host':             host,
                'gpu_type':         gpu_type,
                'gpu_device_id':    dev_id,
                'gpu_device_bus_id': bus_id,
            })
    return mapping


def _build_gpu_mapping(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    mapping = []
    seen_hosts = set()
    for node in config.get('testNodes', []):
        gpu_type = _configured_gpu_value(node, config, 'gpuType', '')
        dev_id = _as_gpu_device_id(
            _configured_gpu_value(node, config, 'gpuDeviceId', -1))
        bus_id = _configured_gpu_value(node, config, 'gpuDeviceBusId', '')
        if gpu_type or dev_id >= 0 or bus_id:
            for host in expand_hostnames(node.get('hostName', '')):
                if host in seen_hosts:
                    continue
                seen_hosts.add(host)
                mapping.append({
                    'host':             host,
                    'gpu_type':         gpu_type,
                    'gpu_device_id':    dev_id,
                    'gpu_device_bus_id': bus_id,
                })
    return mapping


def _gpu_config_by_host(config: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    return {entry['host']: entry for entry in _build_gpu_mapping(config)}


def render_test_header(cfg: Dict[str, Any], out, *,
                       title: str,
                       verbose: bool,
                       is_bw: bool = True,
                       is_duration: Optional[bool] = None) -> None:
    print(fio_section(title), file=out, end='')

    print(fio_kv('timestamp', cfg['timestamp']), file=out)
    print(fio_kv('binary', cfg['binary']), file=out)
    print(fio_kv('pattern', cfg['pattern']), file=out)
    hosts = cfg['hosts']
    print(fio_kv('hosts', f"{len(hosts)}  ({', '.join(hosts)})"), file=out)

    devices = cfg.get('devices') or []
    if devices:
        print(fio_kv('devices', ''), file=out)
        for host, dev in devices:
            print(f"      {host} : {dev}", file=out)

    print(fio_kv('connections', str(cfg['connections'])), file=out)
    print(fio_kv('streams', str(cfg['streams'])), file=out)

    show_duration = (is_duration is not False) and ('duration' in cfg)
    show_iters = (is_duration is not True) and ('iters' in cfg)
    if show_duration:
        print(fio_kv('duration', f"{cfg['duration']} s"), file=out)
    elif show_iters:
        print(fio_kv('iters', str(cfg['iters'])), file=out)

    print(fio_kv('message size', f"{cfg['message_size']} B"), file=out)

    if is_bw:
        print(fio_kv('bidirectional', cfg['bidirectional']), file=out)

    if 'connection_type' in cfg:
        print(fio_kv('connection_type', cfg['connection_type']), file=out)

    for key in ('qps', 'tx_depth', 'rx_depth'):
        if key in cfg:
            print(fio_kv(key, str(cfg[key])), file=out)
    if is_bw and 'post_list' in cfg:
        print(fio_kv('post_list', str(cfg['post_list'])), file=out)
    if is_bw and 'cq_mod' in cfg:
        print(fio_kv('cq_mod', str(cfg['cq_mod'])), file=out)
    if 'mtu' in cfg:
        print(fio_kv('mtu', str(cfg['mtu'])), file=out)

    print(fio_kv('rdma_cm', cfg['rdma_cm']), file=out)

    render_gpu_config_lines(cfg, out)

    if verbose:
        for key in ('link_type', 'transport_type', 'srq'):
            if key in cfg:
                print(fio_kv(key, str(cfg[key])), file=out)
        if 'gid_index' in cfg:
            print(fio_kv('gid_index', str(cfg['gid_index'])), file=out)
        binary = cfg.get('binary', '')
        if 'outstanding_reads' in cfg and binary.startswith('ib_read_'):
            print(fio_kv('outstanding_reads', str(cfg['outstanding_reads'])),
                  file=out)
        print(fio_kv('no_enhanced_reorder', cfg['no_enhanced_reorder']),
              file=out)


def render_gpu_config_lines(cfg: Dict[str, Any], out) -> None:
    if 'gpu_type' in cfg:
        print(fio_kv('gpu_type', cfg['gpu_type']), file=out)
    if 'gpu_memory' in cfg:
        print(fio_kv('gpu_memory', cfg['gpu_memory']), file=out)
    if cfg.get('dmabuf'):
        print(fio_kv('dmabuf', 'yes'), file=out)
    if cfg.get('data_direct'):
        print(fio_kv('data_direct', 'yes'), file=out)

    gpu_mapping = cfg.get('gpu_mapping', [])
    if not gpu_mapping:
        return

    device_ids = [m['gpu_device_id'] for m in gpu_mapping
                  if m['gpu_device_id'] >= 0]
    bus_ids = [m['gpu_device_bus_id'] for m in gpu_mapping
               if m['gpu_device_bus_id']]
    if len(set(device_ids)) == 1 and not bus_ids:
        print(fio_kv('gpu_device_id', str(device_ids[0])), file=out)
        return

    print(fio_kv('gpu_mapping', ''), file=out)
    for mapping in gpu_mapping:
        gpu_type = mapping.get('gpu_type') or cfg.get('gpu_type', 'gpu')
        dev_str = (f"{gpu_type}:{mapping['gpu_device_id']}"
                   if mapping['gpu_device_id'] >= 0 else gpu_type)
        bus_str = (f"  ({mapping['gpu_device_bus_id']})"
                   if mapping['gpu_device_bus_id'] else '')
        print(f"    {mapping['host']:<24} {dev_str}{bus_str}", file=out)


def render_assignment_table(assignments: List[RankAssignment],
                            config: Dict[str, Any],
                            out=None) -> None:
    if out is None:
        out = sys.stdout
    gpu_by_host = _gpu_config_by_host(config)
    if not gpu_by_host:
        return

    print("[Cluster] Resolved device assignments:", file=out)
    seen = set()
    for assignment in assignments:
        if assignment.hostname in seen:
            continue
        seen.add(assignment.hostname)
        rdma_str = f"rdma={assignment.device}" if assignment.device else "rdma=<default>"
        gpu = gpu_by_host.get(assignment.hostname, {})
        gpu_str = ''
        if gpu.get('gpu_type'):
            gpu_str = f"gpu={gpu['gpu_type']}:{gpu['gpu_device_id']}"
            if gpu.get('gpu_device_bus_id'):
                gpu_str += f"  ({gpu['gpu_device_bus_id']})"
        flags = _build_gpu_flags(assignment, config)
        flags_str = f"  flags: {' '.join(flags)}" if flags else ''
        print(f"  {assignment.hostname:<24} {rdma_str:<18} {gpu_str}{flags_str}",
              file=out)


def _dv_by_connection(dv, assignments: List[RankAssignment]) -> Dict[int, list]:
    """Group validating ranks by their connection_id (a connection has one
    validating end for half-duplex, two for bidirectional)."""
    rank_to_cid = {a.rank: a.connection_id for a in assignments}
    by_cid: Dict[int, list] = {}
    for w in dv.workers:
        by_cid.setdefault(rank_to_cid.get(w.rank, -1), []).append(w)
    return by_cid


def _dv_status_str(workers: list) -> str:
    """Combine one or two validating ends into a status string."""
    passed = all(w.passed for w in workers)
    errors = sum(w.errors for w in workers)
    return 'PASSED' if passed else f"FAILED ({errors} mismatch(es))"


def render_bw_results(result: BwResult,
                      params_lines: List[str],
                      config: Dict[str, Any],
                      assignments: List[RankAssignment],
                      gpu_map: Dict[str, dict],
                      verbose: bool = False,
                      out=None) -> None:
    if out is None:
        out = sys.stdout
    if not result.ranks:
        return

    total_bw = sum(c.bw for c in result.ranks)
    total_msgrate = sum(c.msgrate for c in result.ranks)
    num_logical = logical_connection_count(assignments)
    avg_bw = total_bw / num_logical if num_logical > 0 else 0

    cfg = build_test_config(config, assignments, gpu_map,
                            parse_perftest_params(params_lines), result)
    unit = result.bw_unit

    render_test_header(cfg, out, title='Cluster BW Test',
                       verbose=verbose, is_bw=True)

    dv_by_cid = (_dv_by_connection(result.data_validation, assignments)
                 if result.data_validation.enabled else {})

    print(fio_section('Results'), file=out, end='')
    for cid, conn in group_bw_connections(result.ranks, assignments).items():
        n = len(conn.streams)
        print(f"  {conn.src} -> {conn.dst}", file=out)
        print(fio_kv('bw', f"{conn.bw_total:.2f} {unit}", indent=4), file=out)
        print(fio_kv('msgrate', f"{conn.msgrate_total:.6f} Mpps", indent=4),
              file=out)
        if cid in dv_by_cid:
            print(fio_kv('data_validation', _dv_status_str(dv_by_cid[cid]),
                         indent=4), file=out)
        if verbose and n > 1:
            for stream in sorted(conn.streams, key=lambda x: x.stream_index):
                idx = stream.stream_index + 1
                print(f"      stream {idx}/{n} (:{stream.port})  "
                      f"bw: {stream.bw:.2f} {unit}   "
                      f"msgrate: {stream.msgrate:.6f} Mpps", file=out)

    print(fio_section('Summary'), file=out, end='')
    print(fio_kv('bw_total', f"{total_bw:.2f} {unit}"), file=out)
    print(fio_kv('bw_avg', f"{avg_bw:.2f} {unit}"), file=out)
    print(fio_kv('msgrate', f"{total_msgrate:.6f} Mpps"), file=out)
    if result.data_validation.enabled:
        dv = result.data_validation
        status = ('PASSED' if dv.passed
                  else f"FAILED ({dv.errors} mismatch(es))")
        print(fio_kv('data_validation', status), file=out)
    print(file=out)


def render_bw_json(result: BwResult,
                   params_lines: List[str],
                   config: Dict[str, Any],
                   assignments: List[RankAssignment],
                   gpu_map: Dict[str, dict],
                   out=None) -> None:
    if out is None:
        out = sys.stdout

    num_logical = logical_connection_count(assignments)
    total_bw = sum(c.bw for c in result.ranks)
    total_msgrate = sum(c.msgrate for c in result.ranks)
    avg_bw = total_bw / num_logical if num_logical > 0 else 0
    cfg = build_test_config(config, assignments, gpu_map,
                            parse_perftest_params(params_lines), result)

    dv_by_cid = (_dv_by_connection(result.data_validation, assignments)
                 if result.data_validation.enabled else {})

    connections = []
    for cid, conn in group_bw_connections(result.ranks, assignments).items():
        streams = [{
            'port':         stream.port,
            'stream_index': stream.stream_index + 1,
            'bw':           round(stream.bw, 2),
            'msgrate':      round(stream.msgrate, 6),
        } for stream in sorted(conn.streams, key=lambda x: x.stream_index)]
        entry = {
            'src':           conn.src,
            'dst':           conn.dst,
            'bw_total':      round(sum(s['bw'] for s in streams), 2),
            'msgrate_total': round(sum(s['msgrate'] for s in streams), 6),
            'streams':       streams,
        }
        if cid in dv_by_cid:
            ends = dv_by_cid[cid]
            entry['data_validation'] = {
                'passed': all(w.passed for w in ends),
                'errors': sum(w.errors for w in ends),
                'ends':   len(ends),
            }
        connections.append(entry)

    payload = {
        'result_kind':        'bw',
        'test_config':        _cfg_for_json(cfg),
        'workers':            len(assignments),
        'mpi_ranks':          len(assignments) + 1,
        'bw_unit':            result.bw_unit,
        'bw_total':           round(total_bw, 2),
        'bw_avg':             round(avg_bw, 2),
        'msgrate_total':      round(total_msgrate, 6),
        'connections_detail': connections,
    }
    if result.data_validation.enabled:
        payload['data_validation'] = _dv_for_json(result.data_validation,
                                                  assignments)

    print(json.dumps(payload, indent=2), file=out)


def _dv_for_json(dv, assignments: List[RankAssignment]) -> Dict[str, Any]:
    """Render the data-validation summary, mapping each validating rank to its
    connection (src/dst) via the rank assignments."""
    rank_to_a = {a.rank: a for a in assignments}
    workers = []
    for w in dv.workers:
        a = rank_to_a.get(w.rank)
        workers.append({
            'rank':          w.rank,
            'role':          'client' if w.role == 1 else 'server',
            'src':           a.hostname if a else f'rank-{w.rank}',
            'dst':           a.peer_host if a else '?',
            'connection_id': a.connection_id if a else -1,
            'passed':        w.passed,
            'errors':        w.errors,
            'bytes':         w.bytes_validated,
            'chunks':        w.chunks,
        })
    return {
        'enabled': True,
        'passed':  dv.passed,
        'errors':  dv.errors,
        'workers': workers,
    }


def render_lat_results(result: LatResult,
                       params_lines: List[str],
                       config: Dict[str, Any],
                       assignments: List[RankAssignment],
                       gpu_map: Dict[str, dict],
                       verbose: bool = False,
                       out=None) -> None:
    if out is None:
        out = sys.stdout

    clients = result.clients()
    if not clients:
        return

    is_duration = any(w.is_duration for w in clients)
    cfg = build_test_config(config, assignments, gpu_map,
                            parse_perftest_params(params_lines), result)
    if cfg['message_size'] == 0:
        cfg['message_size'] = clients[0].size

    render_test_header(cfg, out, title='Cluster Latency Test',
                       verbose=verbose, is_bw=False, is_duration=is_duration)

    print(fio_section('Results'), file=out, end='')
    for conn in group_lat_connections(clients, assignments).values():
        n = len(conn.streams)
        stats = lat_connection_stats(conn, is_duration)
        print(f"  {conn.src} -> {conn.dst}", file=out)
        print(fio_kv('t_avg', f"{stats['t_avg']:.3f} usec", indent=4), file=out)
        if n > 1:
            print(fio_kv('t_avg_worst', f"{stats['t_avg_worst']:.3f} usec",
                         indent=4), file=out)
        if is_duration:
            print(fio_kv('tps', f"{stats['tps']:.2f}", indent=4), file=out)
        else:
            print(fio_kv('t_typical', f"{stats['t_typical']:.3f} usec", indent=4),
                  file=out)
            print(fio_kv('t_min', f"{stats['t_min']:.3f} usec", indent=4), file=out)
            print(fio_kv('t_max', f"{stats['t_max']:.3f} usec", indent=4), file=out)
            print(fio_kv('p99', f"{stats['p99']:.3f} usec", indent=4), file=out)
            print(fio_kv('p99.9', f"{stats['p99_9']:.3f} usec", indent=4), file=out)

        if verbose and n > 1:
            for stream in sorted(conn.streams, key=lambda x: x.stream_index):
                data = stream.data
                idx = stream.stream_index + 1
                if is_duration:
                    print(f"      stream {idx}/{n} (:{stream.port})  "
                          f"t_avg: {data.t_avg:.3f} usec  tps: {data.tps:.2f}",
                          file=out)
                else:
                    print(f"      stream {idx}/{n} (:{stream.port})  "
                          f"t_avg: {data.t_avg:.3f}  t_min: {data.t_min:.3f}  "
                          f"t_max: {data.t_max:.3f}  p99: {data.p99:.3f} usec",
                          file=out)

    summary = lat_summary_stats(clients, is_duration)
    print(fio_section('Summary'), file=out, end='')
    print(fio_kv('connections', str(logical_connection_count(assignments))), file=out)
    print(fio_kv('t_avg', f"{summary['t_avg']:.3f} usec"), file=out)
    if len([c.t_avg for c in clients if c.t_avg > 0]) > 1:
        print(fio_kv('t_avg_worst', f"{summary['t_avg_worst']:.3f} usec"), file=out)
    if is_duration:
        print(fio_kv('tps_total', f"{summary['tps_total']:.2f}"), file=out)
    else:
        print(fio_kv('t_min', f"{summary['t_min']:.3f} usec"), file=out)
        print(fio_kv('t_max', f"{summary['t_max']:.3f} usec"), file=out)
    print(file=out)


def render_lat_json(result: LatResult,
                    params_lines: List[str],
                    config: Dict[str, Any],
                    assignments: List[RankAssignment],
                    gpu_map: Dict[str, dict],
                    out=None) -> None:
    if out is None:
        out = sys.stdout

    clients = result.clients()
    cfg = build_test_config(config, assignments, gpu_map,
                            parse_perftest_params(params_lines), result)
    if cfg['message_size'] == 0 and clients:
        cfg['message_size'] = clients[0].size

    rank_to_assignment = {a.rank: a for a in assignments}
    connections = []
    for client in clients:
        assignment = rank_to_assignment.get(client.rank)
        connections.append({
            'src':          assignment.hostname if assignment else f"rank-{client.rank}",
            'dst':          assignment.peer_host if assignment else '?',
            'rank':         client.rank,
            'port':         assignment.port if assignment else 0,
            'stream_index': (assignment.stream_index + 1) if assignment else 0,
            'size':         client.size,
            'iters':        client.iters,
            'test_type':    'duration' if client.is_duration else 'iterations',
            't_avg':        client.t_avg,
            't_min':        client.t_min,
            't_max':        client.t_max,
            't_typical':    client.t_typical,
            'stdev':        client.stdev,
            'p99':          client.p99,
            'p99_9':        client.p99_9,
            'tps':          client.tps,
        })

    print(json.dumps({
        'result_kind':        'lat',
        'test_config':        _cfg_for_json(cfg),
        'workers':            len(assignments),
        'mpi_ranks':          len(assignments) + 1,
        'connections_detail': connections,
    }, indent=2), file=out)

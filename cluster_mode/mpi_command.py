"""
mpi_command.py - Build mpirun MPMD command from rank assignments.

Architecture:
  - Rank 0: native C perftest_cluster_worker binary
    (src/perftest_cluster_worker.c), always on launcher host.
  - Ranks 1..N: C perftest workers, one per RankAssignment.

"""

import os
import shlex
import socket
import tempfile
from collections import Counter
from typing import List, Dict, Any, Optional, Tuple
from .traffic_patterns import RankAssignment
from .config_model import TYPED_PERFTEST_FIELDS

_LAUNCHER_HOSTNAME = socket.gethostname()


def _q(value: Any) -> str:
    return shlex.quote(str(value))


def _quote_args(args: str) -> str:
    if not args:
        return ''
    return ' '.join(_q(token) for token in shlex.split(args))

# GPU types whose flag takes a numeric device ID
_GPU_NUMERIC_ID_FLAG: Dict[str, str] = {
    'cuda':   '--use_cuda',
    'rocm':   '--use_rocm',
    'neuron': '--use_neuron',
    'mlu':    '--use_mlu',
    'opencl': '--use_opencl',
}

# GPU types whose flag takes a PCIe bus address string instead of a numeric ID
_GPU_BUS_ID_FLAG: Dict[str, str] = {
    'cuda': '--use_cuda_bus_id',
    'hl':   '--use_hl',   # Habana Labs always uses PCIe string, never numeric
}

# CUDA memory type name -> perftest --cuda_mem_type numeric value
_CUDA_MEM_TYPE_MAP: Dict[str, int] = {'device': 0, 'managed': 1, 'pinned': 2}


def _build_typed_perftest_flags(resolved: Dict[str, Any]) -> List[str]:
    """Render resolved typed JSON fields into perftest CLI tokens.

    Booleans emit just the flag when truthy and nothing when falsy.
    Other kinds emit "<flag> <value>". Short flag is preferred when
    available; otherwise the long flag is used. Fields absent from
    `resolved` are skipped.
    """
    tokens: List[str] = []
    for f in TYPED_PERFTEST_FIELDS:
        if f.name not in resolved:
            continue
        value = resolved[f.name]
        flag = f.short_flag or f.long_flag
        if flag is None:
            continue
        if f.kind == 'bool':
            if value:
                tokens.append(flag)
            continue
        if value is None or value == '':
            continue
        tokens.extend([flag, str(value)])
    return tokens


def _build_gpu_flags(assignment: RankAssignment, config: Dict[str, Any]) -> List[str]:
    """Build the list of GPU-related flags for a single C worker rank.

    Bus ID takes precedence over numeric device ID when both are set.
    CUDA-specific extras (dmabuf, data_direct, mem_type) are appended
    when the corresponding config keys are set.
    """
    if not assignment.gpu_type:
        return []

    flags: List[str] = []
    typ = assignment.gpu_type

    if typ in _GPU_BUS_ID_FLAG and assignment.gpu_device_bus_id:
        flags.append(f'{_GPU_BUS_ID_FLAG[typ]}={assignment.gpu_device_bus_id}')
    elif typ in _GPU_NUMERIC_ID_FLAG:
        if assignment.gpu_device_id < -1:
            raise ValueError(
                f"gpuDeviceId must be -1 or >= 0, got {assignment.gpu_device_id}")
        dev_id = assignment.gpu_device_id if assignment.gpu_device_id >= 0 else 0
        flags.append(f'{_GPU_NUMERIC_ID_FLAG[typ]}={dev_id}')

    if typ == 'cuda':
        if config.get('cudaDmabuf') or config.get('dataDirectMode'):
            flags.append('--use_cuda_dmabuf')
        if config.get('dataDirectMode'):
            flags.append('--use_data_direct')
        mem_type = config.get('cudaMemType', '')
        if mem_type:
            flags.append(f'--cuda_mem_type={_CUDA_MEM_TYPE_MAP.get(mem_type, 0)}')

    return flags


def _build_worker_segment(a: RankAssignment, binary: str,
                          perftest_args: str,
                          config: Dict[str, Any]) -> str:
    """Build a single MPMD worker segment (one C worker, ranks 1..N).

    Token order: binary, -d device, GPU flags, -p port, typed perftest
    flags, global perftestArgs, per-host perftestArgs, and (for clients)
    the peer connect address. Every dynamic value is shell-quoted.
    """
    parts = [_q(binary)]

    if a.device:
        parts.extend(['-d', _q(a.device)])

    parts.extend(_q(flag) for flag in _build_gpu_flags(a, config))

    parts.extend(['-p', _q(a.port)])

    parts.extend(_q(token)
                 for token in _build_typed_perftest_flags(a.typed_perftest_fields))

    if perftest_args:
        parts.append(_quote_args(perftest_args))

    if a.perftest_args:
        parts.append(_quote_args(a.perftest_args))

    if a.role == 'client':
        # peer_address (from peerAddress) overrides peer_host for the
        # connect string only - e.g. the peer's RDMA-capable interface IP,
        # when it differs from the hostname used for mpirun/SSH placement.
        parts.append(_q(a.peer_address or a.peer_host))

    return f'-np 1 -H {_q(a.hostname)} ' + ' '.join(parts)


def _build_global_flags(config: Dict[str, Any], libnuma_ok: bool) -> str:
    """Build the space-joined global mpirun flags (empty entries dropped).

    --bind-to selection: mpiBindTo unset or 'auto' selects the libnuma-based
    default - 'none' when the launcher has libnuma (so each perftest worker
    self-binds to its NIC-local NUMA node) and no --bind-to when it does not
    (leaving Open MPI's default binding, which is better than fully unbound).
    Any other mpiBindTo value is passed to mpirun --bind-to verbatim.
    """
    timeout = config.get('mpirunTimeout')
    mpi_prefix = config.get('mpiPrefix', '')
    subnet = config.get('mpiSubnet')
    bind_to = config.get('mpiBindTo')
    if not bind_to or str(bind_to).strip().lower() == 'auto':
        bind_to = 'none' if libnuma_ok else ''
    return ' '.join(filter(None, [
        f'--timeout {_q(timeout)}' if timeout else '',
        f'--prefix {_q(mpi_prefix)}' if mpi_prefix else '',
        f'--mca btl_tcp_if_include {_q(subnet)}' if subnet else '',
        f'--bind-to {_q(bind_to)}' if bind_to else '',
        '--oversubscribe',
        '--allow-run-as-root',
    ]))


def build_mpirun_command(config: Dict[str, Any],
                         assignments: List[RankAssignment],
                         result_file: Optional[str] = None,
                         libnuma_available: Optional[bool] = None) -> Tuple[str, str]:
    """Build the full MPMD mpirun command string.

    Prepends a native C rank-0 segment (perftest_cluster_worker) before all
    C worker segments, using colon-separated MPMD syntax. `result_file`
    defaults to a generated temp path if not given. `libnuma_available`
    controls the default --bind-to (see _build_global_flags); when None it is
    probed on the launcher via mpi_env.libnuma_available().

    Returns (command_string, result_file_path).
    """
    if libnuma_available is None:
        from .mpi_env import libnuma_available as _probe_libnuma
        libnuma_available = _probe_libnuma()
    binary = config.get('perftestBinary', 'ib_write_bw')
    perftest_args = config.get('perftestArgs', '')

    if result_file is None:
        result_file = os.path.join(tempfile.gettempdir(),
                                   f'perftest_cluster_result_{os.getpid()}.json')

    result_kind = 'lat' if binary.endswith('_lat') else 'bw'
    num_workers = len(assignments)

    cluster_binary_dir = config.get('perftestClusterWorkerDir', '')
    if cluster_binary_dir:
        worker_bin = f'{cluster_binary_dir.rstrip("/")}/perftest_cluster_worker'
    else:
        worker_bin = 'perftest_cluster_worker'

    worker_cmd = (
        f'{_q(worker_bin)}'
        f' --result-kind {_q(result_kind)}'
        f' --num-workers {_q(num_workers)}'
        f' --output-file {_q(result_file)}'
    )

    rank_cmds = [f'-np 1 -H {_q(_LAUNCHER_HOSTNAME)} {worker_cmd}']
    rank_cmds.extend(
        _build_worker_segment(a, binary, perftest_args, config)
        for a in assignments
    )

    mpirun_bin = config.get('mpirunPath', 'mpirun')
    global_flags = _build_global_flags(config, libnuma_available)

    cmd = f"{_q(mpirun_bin)} {global_flags} \\\n  " + " \\\n  : \\\n  ".join(rank_cmds)
    return cmd, result_file

"""
config_model.py - Typed normalization and validation for cluster-mode config.
"""

from __future__ import annotations

import os
import re
import shlex
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple

from .config_parser import (
    expand_hostnames, expand_test_configs, parse_config, split_host_list,
)
from .traffic_patterns import TrafficPattern


GPU_TYPES = {'cuda', 'rocm', 'neuron', 'hl', 'mlu', 'opencl'}


@dataclass(frozen=True)
class ConfigField:
    name: str
    typ: str
    default: str
    description: str


@dataclass(frozen=True)
class TypedPerftestField:
    """A perftest CLI flag exposed as a typed JSON field.

    Either short_flag or long_flag (or both) must be set. For booleans the
    field emits the flag with no value when true; otherwise it emits
    "<flag> <value>" using the short form when available.
    """
    name: str
    kind: str                           # 'int' | 'str' | 'bool'
    short_flag: Optional[str]
    long_flag: Optional[str]
    default: str
    description: str


# Single source of truth for perftest flags promoted to typed JSON fields.
# Both top-level and per-testNode entries accept the same names; per-node
# values override top-level at command-build time.
TYPED_PERFTEST_FIELDS: List[TypedPerftestField] = [
    TypedPerftestField('messageSize',       'int',  '-s', '--size',
                       '-', 'Perftest -s message size in bytes.'),
    TypedPerftestField('qpsPerProcess',     'int',  '-q', '--qp',
                       '-', 'Perftest -q QPs per worker process.'),
    TypedPerftestField('duration',          'int',  '-D', '--duration',
                       '-', 'Perftest -D run duration in seconds (BW).'),
    TypedPerftestField('perftestIters',     'int',  '-n', '--iters',
                       '-', 'Perftest -n iterations (LAT).'),
    TypedPerftestField('bidirectional',     'bool', '-b', None,
                       'false', 'Perftest -b bidirectional traffic.'),
    TypedPerftestField('reportGbits',       'bool', None, '--report_gbits',
                       'false', 'Report BW in Gb/s.'),
    TypedPerftestField('connectionType',    'str',  '-c', '--connection',
                       '-', 'Perftest -c connection type (RC, UC, UD, XRC, DC, SRD).'),
    TypedPerftestField('mtu',               'int',  '-m', '--mtu',
                       '-', 'Perftest -m MTU.'),
    TypedPerftestField('inlineSize',        'int',  '-I', '--inline_size',
                       '-', 'Perftest -I inline size.'),
    TypedPerftestField('txDepth',           'int',  '-t', '--tx-depth',
                       '-', 'Perftest -t tx depth.'),
    TypedPerftestField('rxDepth',           'int',  '-r', '--rx-depth',
                       '-', 'Perftest -r rx depth.'),
    TypedPerftestField('postList',          'int',  '-l', '--post_list',
                       '-', 'Perftest -l post list size.'),
    TypedPerftestField('cqModeration',      'int',  '-Q', '--cq-mod',
                       '-', 'Perftest -Q CQ moderation.'),
    TypedPerftestField('useRdmaCm',         'bool', '-R', '--rdma_cm',
                       'false', 'Use RDMA CM for connection setup.'),
    TypedPerftestField('noEnhancedReorder', 'bool', None, '--no_enhanced_reorder',
                       'false', 'Disable enhanced OOO recv WRs.'),
    TypedPerftestField('dataValidation',     'bool', None, '--data_validation',
                       'false', 'Per-message data validation (WRITE/READ BW, RC, tx_depth>=32).'),
    TypedPerftestField('dataValidationDebug', 'bool', None, '--data_validation_debug',
                       'false', 'Verbose data validation debug output.'),
    TypedPerftestField('numaNode',           'int',  None, '--numa_node',
                       '-', 'Bind the test process to this NUMA node (perftest --numa_node).'),
    TypedPerftestField('disableNuma',        'bool', None, '--disable_numa',
                       'false', 'Disable perftest NUMA auto-detection/binding.'),
    TypedPerftestField('gidIndex',           'int',  '-x', '--gid-index',
                       '-', 'Perftest -x GID index (RoCE: selects the GID / RoCE version).'),
]


def _typed_field_config_entries() -> List[ConfigField]:
    """Render TYPED_PERFTEST_FIELDS as ConfigField rows for the help table."""
    return [
        ConfigField(f.name, f.kind, f.default, f.description)
        for f in TYPED_PERFTEST_FIELDS
    ]


def _kebab(name: str) -> str:
    """camelCase -> kebab-case, e.g. 'messageSize' -> 'message-size'."""
    return re.sub(r'(?<!^)(?=[A-Z])', '-', name).lower()


def typed_field_option_strings(f: TypedPerftestField) -> List[str]:
    """Argparse option strings for a typed field: kebab-case canonical flag,
    plus the perftest-native long flag as an alias (deduped)."""
    canonical = f'--{_kebab(f.name)}'
    opts = [canonical]
    if f.long_flag and f.long_flag != canonical:
        opts.append(f.long_flag)
    return opts


TOP_LEVEL_FIELDS = [
    ConfigField('name', 'string', 'perftestBinary',
                'Optional test name used in multi-test banners.'),
    ConfigField('testNodes', 'array', 'required',
                'Host definitions; each item supports hostName/deviceName/GPU fields.'),
    ConfigField('tests', 'array', '-',
                'Sequential test entries; root fields are defaults for each entry.'),
    ConfigField('iterations', 'int', '1',
                'Repeat this test entry N times; each iteration is a separate run.'),
    ConfigField('iteration', 'int', 'generated',
                'Generated current iteration index for expanded test runs.'),
    ConfigField('trafficPattern', 'string', 'O2O',
                'Traffic pattern: O2O, O2M, M2O, A2A, B, R.'),
    ConfigField('perftestBinary', 'string', 'ib_write_bw',
                'Perftest binary name or absolute/shared path.'),
    ConfigField('perftestArgs', 'string', '',
                'Extra perftest arguments, e.g. "-s 65536 -D 10 --report_gbits".'),
    ConfigField('port', 'int', '18515',
                'Base TCP port; cluster mode assigns per-rank/per-stream ports.'),
    ConfigField('streams', 'int', '1',
                'Parallel streams per logical connection.'),
    ConfigField('mpiSubnet', 'string', '-',
                'Subnet for Open MPI btl_tcp_if_include.'),
    ConfigField('mpirunTimeout', 'int', '-',
                'mpirun --timeout value in seconds.'),
    ConfigField('mpirunPath', 'string', 'mpirun',
                'Launcher-local mpirun binary path or command name.'),
    ConfigField('mpiPrefix', 'string', '-',
                'Open MPI prefix passed as mpirun --prefix for remote runtime setup.'),
    ConfigField('perftestClusterWorkerDir', 'string', '-',
                'Directory containing perftest_cluster_worker if it is not on PATH.'),
    ConfigField('verbose', 'bool', 'false',
                'Enable detailed cluster output and per-stream reporting.'),
    ConfigField('gpuType', 'string', '-',
                'Global GPU type: cuda, rocm, neuron, hl, mlu, opencl.'),
    ConfigField('gpuDeviceId', 'int', '-1',
                'Global numeric GPU device index. Mutually exclusive with autoDetect.'),
    ConfigField('gpuDeviceBusId', 'string', '-',
                'Global GPU PCIe bus ID; takes precedence over gpuDeviceId. '
                'Mutually exclusive with autoDetect.'),
    ConfigField('cudaMemType', 'string', '-',
                'CUDA memory type: device, managed, pinned.'),
    ConfigField('cudaDmabuf', 'bool', 'false',
                'Enable --use_cuda_dmabuf for CUDA ranks.'),
    ConfigField('dataDirectMode', 'bool', 'false',
                'Enable Data Direct mode; requires gpuType=cuda.'),
    ConfigField('autoDetect', 'bool', 'false',
                'SSH-probe hosts for NIC/GPU affinity and populate GPU bus IDs. '
                'Rejected together with an explicit gpuDeviceId/gpuDeviceBusId '
                '(global or per-node) - pick one or the other.'),
    ConfigField('mpiBindTo', 'string', 'auto',
                'mpirun --bind-to value. Default auto: none when the launcher '
                'has libnuma (perftest self-binds NIC-local), otherwise Open '
                'MPI default binding. Set none/core/numa to force a value.'),
    ConfigField('jsonOutputFile', 'string', '-',
                'Save the full JSON result report to this file, independent '
                'of --output-format (table stays on stdout/stderr). '
                'Mutually exclusive with jsonOutputDir.'),
    ConfigField('jsonOutputDir', 'string', '-',
                'Save one generated JSON result report per executed test '
                'into this directory. Mutually exclusive with jsonOutputFile.'),
] + _typed_field_config_entries()

TEST_NODE_FIELDS = [
    ConfigField('hostName', 'string', 'required',
                'Hostname or pattern such as node-[01-04].'),
    ConfigField('deviceName', 'string', '-',
                'RDMA device name for this host, e.g. mlx5_0 or rocep8s0f0.'),
    ConfigField('perftestArgs', 'string', '-',
                'Extra perftest args for ranks on this host; appended after global perftestArgs.'),
    ConfigField('gpuType', 'string', '-',
                'Per-host GPU type; overrides global gpuType.'),
    ConfigField('gpuDeviceId', 'int', '-1',
                'Per-host numeric GPU device index. Mutually exclusive with autoDetect.'),
    ConfigField('gpuDeviceBusId', 'string', '-',
                'Per-host GPU PCIe bus ID; takes precedence over gpuDeviceId. '
                'Mutually exclusive with autoDetect.'),
    ConfigField('peerAddress', 'string', '-',
                'Address peers use to connect to this host (e.g. its RDMA-capable '
                'interface IP), overriding hostName for that purpose only - '
                'hostName is still used for mpirun/SSH placement. Required on '
                'every testNodes[] entry when useRdmaCm is set.'),
] + _typed_field_config_entries()


def _format_field_table(title: str, fields: List[ConfigField]) -> List[str]:
    lines = [title, '-' * len(title)]
    lines.append(f"{'Field':<18} {'Type':<8} {'Default':<12} Description")
    lines.append(f"{'-' * 18} {'-' * 8} {'-' * 12} {'-' * 40}")
    for field in fields:
        lines.append(
            f"{field.name:<18} {field.typ:<8} {field.default:<12} "
            f"{field.description}")
    return lines


def format_json_config_help() -> str:
    """Return user-facing help for every supported JSON configuration entry."""
    lines = ["Supported cluster-mode JSON fields", ""]
    lines.extend(_format_field_table("Top-level fields", TOP_LEVEL_FIELDS))
    lines.append("")
    lines.extend(_format_field_table("testNodes[] fields", TEST_NODE_FIELDS))
    lines.extend([
        "",
        "Notes:",
        "  - Common perftest flags have typed JSON equivalents (see fields above).",
        "  - perftestArgs is for everything else; raw flags that have a typed",
        "    equivalent (e.g. -s/--size, -q/--qp, -b, -p/--port) are rejected.",
        "  - Per-node typed values override top-level for ranks on that host.",
        "  - A top-level tests array runs entries sequentially.",
        "  - tests[].iterations repeats a test entry as separate sequential runs.",
    ])
    return "\n".join(lines)


def _forbidden_perftest_flags() -> List[Tuple[str, Optional[str], Optional[str], str]]:
    """List flags that must not appear inside perftestArgs.

    Returns a list of (json_field_name, short_flag, long_flag, hint) tuples.
    Includes the orchestrator-owned port and every flag exposed as a typed
    JSON field by TYPED_PERFTEST_FIELDS.
    """
    forbidden: List[Tuple[str, Optional[str], Optional[str], str]] = [
        ('port', '-p', '--port',
         "Use the JSON port field; the orchestrator manages per-rank ports."),
        ('runAllSizes', '-a', '--all',
         "Cluster mode runs one message size per test; -a/--all only "
         "reports/gathers for that size and deadlocks the MPI collectives "
         "for every other size, hanging the whole cluster."),
    ]
    for f in TYPED_PERFTEST_FIELDS:
        forbidden.append((
            f.name, f.short_flag, f.long_flag,
            f"Use the JSON {f.name} field instead.",
        ))
    return forbidden


def _token_matches_flag(token: str,
                        short_flag: Optional[str],
                        long_flag: Optional[str]) -> bool:
    """Return True if `token` is `<flag>`, `<short><value>`, or `<long>=<value>`."""
    if short_flag is not None:
        if token == short_flag:
            return True
        # Attached short form: -s65536, -ccx (anything after the short flag).
        if (len(short_flag) == 2 and token.startswith(short_flag)
                and len(token) > 2 and token[2] != '-'):
            return True
    if long_flag is not None:
        if token == long_flag or token.startswith(long_flag + '='):
            return True
    return False


# Data validation is only supported by these verbs; enforce the same
# constraint set as src/perftest_parameters.c so cluster runs fail fast with a
# clear message instead of a cryptic per-rank error mid-run.
_DV_BINARIES = {'ib_write_bw', 'ib_read_bw'}
_DV_FORBIDDEN_RAW = (
    '-a', '--all', '--run_infinitely', '--mr_per_qp',
    '--use-null-mr', '--gpu_touch',
)


def _data_validation_enabled(data: Dict[str, Any]) -> bool:
    if data.get('dataValidation'):
        return True
    return any(node.get('dataValidation') for node in data.get('testNodes', []))


def validate_data_validation(data: Dict[str, Any]) -> None:
    """Reject data-validation configs perftest cannot run (write/read BW, RC,
    tx_depth>=32, host or CUDA memory, no incompatible flags)."""
    if not _data_validation_enabled(data):
        return

    binary = os.path.basename(str(data.get('perftestBinary', '') or ''))
    if binary not in _DV_BINARIES:
        raise ValueError(
            "dataValidation requires perftestBinary ib_write_bw or ib_read_bw, "
            f"got {binary or '(unset)'}")

    def _as_int(value: Any) -> Optional[int]:
        if value is None or value == '':
            return None
        try:
            return int(value)
        except (TypeError, ValueError):
            return None

    for node in (data.get('testNodes') or [{}]):
        conn = node.get('connectionType', data.get('connectionType'))
        if conn and conn != 'RC':
            raise ValueError(
                f"dataValidation requires connectionType RC, got {conn}")
        tx = _as_int(node.get('txDepth', data.get('txDepth')))
        if tx is not None and tx < 32:
            raise ValueError(
                f"dataValidation requires txDepth >= 32, got {tx}")
        pl = _as_int(node.get('postList', data.get('postList')))
        if pl is not None and pl > 1:
            raise ValueError(
                f"dataValidation is incompatible with postList > 1, got {pl}")
        gtype = node.get('gpuType', data.get('gpuType'))
        if gtype and gtype != 'cuda':
            raise ValueError(
                "dataValidation requires gpuType cuda or host memory, "
                f"got {gtype}")

    args_blobs = [data.get('perftestArgs', '')]
    args_blobs += [n.get('perftestArgs', '') for n in data.get('testNodes', [])]
    for blob in args_blobs:
        if not blob:
            continue
        try:
            tokens = shlex.split(blob)
        except ValueError:
            continue
        for tok in tokens:
            if tok.split('=', 1)[0] in _DV_FORBIDDEN_RAW:
                raise ValueError(
                    f"dataValidation is incompatible with "
                    f"{tok.split('=', 1)[0]} in perftestArgs")


def _typed_overrides_from_args(args: Any) -> Dict[str, Any]:
    """Collect only the typed perftest fields explicitly set on the CLI.

    Uses getattr(..., None) so this stays robust against argparse.Namespace
    objects (e.g. in tests) that omit some/all typed-field attributes;
    unset fields (None) are excluded so they never clobber a JSON value.
    """
    return {
        f.name: getattr(args, f.name, None)
        for f in TYPED_PERFTEST_FIELDS
        if getattr(args, f.name, None) is not None
    }


def validate_perftest_args(perftest_args: str) -> None:
    """Reject per-rank options the orchestrator or a typed JSON field owns."""
    if not perftest_args:
        return
    try:
        tokens = shlex.split(perftest_args)
    except ValueError as exc:
        raise ValueError(f"Invalid --perftest-args quoting: {exc}")

    forbidden = _forbidden_perftest_flags()
    for token in tokens:
        for name, short_flag, long_flag, hint in forbidden:
            if _token_matches_flag(token, short_flag, long_flag):
                flag_repr = '/'.join(filter(None, [short_flag, long_flag]))
                raise ValueError(
                    f"Do not pass {flag_repr} in perftestArgs. {hint}"
                )


@dataclass(frozen=True)
class ClusterConfig:
    """Normalized cluster-mode configuration.

    All CLI and file input should pass through this model first.
    """
    data: Dict[str, Any]

    @classmethod
    def from_file(cls, path: str) -> "ClusterConfig":
        config = cls(parse_config(path)).normalized()
        # A multi-test template (top-level tests[]) has root fields that are
        # merely defaults to be merged into each tests[] entry; validating
        # the unmerged root shape here would reject perfectly valid
        # templates (e.g. no root-level perftestBinary because every test
        # entry sets its own). Each expanded entry is validated individually
        # by expand_tests() instead.
        if not config._is_multi_test_template():
            config = config.validated()
        return config

    @classmethod
    def from_cli(cls, *, hosts: str, pattern: str, binary: str,
                 port: int, streams: int, perftest_args: str) -> "ClusterConfig":
        expanded: List[str] = []
        for part in split_host_list(hosts):
            expanded.extend(expand_hostnames(part))
        return cls({
            'testNodes': [{'hostName': h} for h in expanded],
            'trafficPattern': pattern,
            'perftestBinary': binary,
            'port': port,
            'streams': streams,
            'perftestArgs': perftest_args,
        }).normalized().validated()

    @classmethod
    def from_args(cls, args) -> "ClusterConfig":
        typed_overrides = _typed_overrides_from_args(args)
        if args.file:
            config = cls.from_file(args.file)
            overrides = dict(typed_overrides)
            if args.mpi_subnet:
                overrides['mpiSubnet'] = args.mpi_subnet
            if args.streams != 1:
                overrides['streams'] = args.streams
            if args.perftest_args:
                overrides['perftestArgs'] = args.perftest_args
            if args.verbose:
                overrides['verbose'] = True
            # --json-output/--json-output-dir are mutually exclusive on the
            # CLI (argparse group), but the JSON file may have set the
            # *other* one; a CLI value must win and clear that leftover
            # field rather than leave both set (validated() would then
            # reject the merged config).
            clear_keys: List[str] = []
            if getattr(args, 'json_output', None):
                overrides['jsonOutputFile'] = args.json_output
                clear_keys.append('jsonOutputDir')
            elif getattr(args, 'json_output_dir', None):
                overrides['jsonOutputDir'] = args.json_output_dir
                clear_keys.append('jsonOutputFile')
            config = config.with_updates(_clear_keys=clear_keys, **overrides)
        elif args.hosts:
            config = cls.from_cli(
                hosts=args.hosts,
                pattern=args.pattern,
                binary=args.binary,
                port=args.port,
                streams=args.streams,
                perftest_args=args.perftest_args,
            )
            if typed_overrides:
                config = config.with_updates(**typed_overrides)
            if args.mpi_subnet:
                config = config.with_updates(mpiSubnet=args.mpi_subnet)
            if args.verbose:
                config = config.with_updates(verbose=True)
            if getattr(args, 'json_output', None):
                config = config.with_updates(jsonOutputFile=args.json_output)
            elif getattr(args, 'json_output_dir', None):
                config = config.with_updates(jsonOutputDir=args.json_output_dir)
        else:
            raise ValueError("Either --file or --hosts is required")

        return config.normalized()

    def normalized(self) -> "ClusterConfig":
        data = dict(self.data)
        data.setdefault('trafficPattern', 'O2O')
        data.setdefault('perftestBinary', 'ib_write_bw')
        data.setdefault('port', 18515)
        data.setdefault('streams', 1)
        data.setdefault('perftestArgs', '')
        data.setdefault('testNodes', [])
        data.setdefault('mpirunPath', 'mpirun')
        data.setdefault('mpiPrefix', '')
        data.setdefault('verbose', False)
        return ClusterConfig(data)

    def expand_tests(self) -> List["ClusterConfig"]:
        configs = []
        for expanded in expand_test_configs(self.data):
            cfg = ClusterConfig(expanded).normalized()
            data = cfg.to_dict()
            if not data.get('name'):
                data['name'] = data.get('perftestBinary', 'test')
            configs.append(ClusterConfig(data).validated())
        return configs

    def with_updates(self, _clear_keys: Optional[List[str]] = None,
                     **updates: Any) -> "ClusterConfig":
        data = dict(self.data)
        for key in (_clear_keys or []):
            data.pop(key, None)
        data.update({k: v for k, v in updates.items() if v is not None})
        config = ClusterConfig(data).normalized()
        if not config._is_multi_test_template():
            config = config.validated()
        return config

    def _is_multi_test_template(self) -> bool:
        """True for a top-level tests[] config whose root fields are only
        defaults to be merged per-entry."""
        tests = self.data.get('tests')
        return isinstance(tests, list) and len(tests) > 0

    def validated(self) -> "ClusterConfig":
        nodes = self.data.get('testNodes', [])
        if not nodes:
            raise ValueError("At least one test node/host is required")
        for node in nodes:
            host = node.get('hostName')
            if not isinstance(host, str) or not host.strip():
                raise ValueError(
                    "Each test node must define a non-empty string hostName")
            node_args = node.get('perftestArgs', '')
            if node_args not in (None, '') and not isinstance(node_args, str):
                raise ValueError("testNodes[].perftestArgs must be a string")
            validate_perftest_args(node_args)
            peer_address = node.get('peerAddress', '')
            if peer_address not in (None, '') and not isinstance(peer_address, str):
                raise ValueError("testNodes[].peerAddress must be a string")

        raw_port = self.data.get('port', 18515)
        try:
            port = int(raw_port)
        except (TypeError, ValueError):
            raise ValueError(
                f"Invalid --port value: {raw_port!r} (must be an integer)")
        if port <= 0 or port > 65535:
            raise ValueError(f"Invalid --port value: {port}")

        raw_pattern = self.data.get('trafficPattern', 'O2O')
        try:
            TrafficPattern(raw_pattern)
        except ValueError:
            valid = ', '.join(p.value for p in TrafficPattern)
            raise ValueError(
                f"Invalid trafficPattern: {raw_pattern!r} (expected one of "
                f"{valid})")

        raw_streams = self.data.get('streams', 1)
        try:
            streams = int(raw_streams)
        except (TypeError, ValueError):
            raise ValueError(
                f"Invalid --streams value: {raw_streams!r} (must be an integer)")
        if streams < 1:
            raise ValueError(f"Invalid --streams value: {streams}")

        binary = self.data.get('perftestBinary', '')
        if not binary:
            raise ValueError("perftestBinary/--binary must not be empty")

        mpirun_path = self.data.get('mpirunPath', 'mpirun')
        if not mpirun_path:
            raise ValueError("mpirunPath must not be empty")

        gpu_type = self.data.get('gpuType', '')
        if gpu_type and gpu_type not in GPU_TYPES:
            raise ValueError(f"Unsupported GPU type: {gpu_type}")
        for node in self.data.get('testNodes', []):
            node_gpu_type = node.get('gpuType', '')
            if node_gpu_type and node_gpu_type not in GPU_TYPES:
                raise ValueError(f"Unsupported GPU type: {node_gpu_type}")

        if self.data.get('autoDetect'):
            # Reject rather than silently pick a precedence rule - checked
            # at both levels since host_needs_probe() would otherwise skip
            # probing any host with a concrete value set here.
            def _is_explicit_gpu_id(value: Any) -> bool:
                return value not in (None, '', -1)

            if (_is_explicit_gpu_id(self.data.get('gpuDeviceId', -1))
                    or self.data.get('gpuDeviceBusId')):
                raise ValueError(
                    "autoDetect is incompatible with an explicit "
                    "gpuDeviceId/gpuDeviceBusId - auto-detect's purpose is "
                    "choosing the GPU for each NIC itself. Remove "
                    "gpuDeviceId/gpuDeviceBusId, or set autoDetect to "
                    "false and pin the GPU explicitly.")
            for node in self.data.get('testNodes', []):
                if (_is_explicit_gpu_id(node.get('gpuDeviceId', -1))
                        or node.get('gpuDeviceBusId')):
                    raise ValueError(
                        "autoDetect is incompatible with an explicit "
                        "per-node gpuDeviceId/gpuDeviceBusId (host "
                        f"{node.get('hostName')!r}) - auto-detect's "
                        "purpose is choosing the GPU for each NIC itself. "
                        "Remove that host's gpuDeviceId/gpuDeviceBusId, or "
                        "set autoDetect to false.")

        validate_perftest_args(self.data.get('perftestArgs', ''))

        if self.data.get('dataDirectMode'):
            # Per-node aware: a per-node gpuType overrides the global one
            # for that host (same fallback rule as validate_data_validation).
            effective_types = [node.get('gpuType', gpu_type)
                              for node in self.data.get('testNodes', [])]
            for effective_type in effective_types:
                if effective_type and effective_type != 'cuda':
                    raise ValueError(
                        f"dataDirectMode requires gpuType cuda, "
                        f"got {effective_type}")
            if not any(effective_types):
                raise ValueError("dataDirectMode requires gpuType cuda")

        nodes = self.data.get('testNodes', [])
        rdma_cm_requested = bool(self.data.get('useRdmaCm')) or any(
            node.get('useRdmaCm') for node in nodes)
        if rdma_cm_requested:
            # useRdmaCm resolves the connect address via rdma_resolve_addr()
            # to pick the connection's device/GID - a hostName that doesn't
            # map to the RDMA fabric would silently route over the wrong
            # link rather than fail loudly. Requiring peerAddress on every
            # node (not just servers - roles aren't known before
            # resolve_pattern() runs) makes that structurally impossible,
            # including the partial-declaration case for a repeated
            # hostname (multi-NIC same host).
            missing = [node.get('hostName', '?') for node in nodes
                      if not node.get('peerAddress', '')]
            if missing:
                raise ValueError(
                    "useRdmaCm requires peerAddress on every testNodes[] "
                    "entry (RDMA CM resolves this address to pick the "
                    "connection's device/GID; hostName may not correspond "
                    f"to the RDMA-capable interface). Missing on: {missing}")

        validate_data_validation(self.data)

        self._validate_numa()

        self._validate_gid_index()

        json_output_file = self.data.get('jsonOutputFile', '')
        json_output_dir = self.data.get('jsonOutputDir', '')
        if (json_output_file not in (None, '')
                and not isinstance(json_output_file, str)):
            raise ValueError("jsonOutputFile must be a string")
        if (json_output_dir not in (None, '')
                and not isinstance(json_output_dir, str)):
            raise ValueError("jsonOutputDir must be a string")
        if json_output_file and json_output_dir:
            raise ValueError(
                "jsonOutputFile and jsonOutputDir are mutually exclusive; "
                "use a single explicit file, or a directory for one "
                "generated report per test.")

        data = dict(self.data)
        data['port'] = port
        data['streams'] = streams

        if str(data.get('mpiBindTo', '')).strip().lower() == 'auto':
            data.pop('mpiBindTo', None)
        return ClusterConfig(data)

    def _validate_numa(self) -> None:
        """Validate numaNode/disableNuma/mpiBindTo (mirrors perftest's C-side
        rules in src/perftest_parameters.c)."""
        nodes = self.data.get('testNodes', [])

        def _check_numa_node(value: Any, where: str) -> None:
            if value is None:
                return
            # bool is a subclass of int; reject it explicitly.
            if isinstance(value, bool) or not isinstance(value, int):
                raise ValueError(
                    f"numaNode must be an integer >= 0 ({where}), got {value!r}")
            if value < 0:
                raise ValueError(
                    f"numaNode must be >= 0 ({where}), got {value}")

        global_numa = self.data.get('numaNode')
        global_disable = self.data.get('disableNuma')
        _check_numa_node(global_numa, "top-level")
        for node in nodes:
            _check_numa_node(node.get('numaNode'), f"host {node.get('hostName')!r}")

        # numaNode and disableNuma are mutually exclusive; check the effective
        # per-node values (per-node overrides top-level, same fallback rule as
        # dataDirectMode above).
        for node in nodes:
            eff_numa = node.get('numaNode', global_numa)
            eff_disable = node.get('disableNuma', global_disable)
            if eff_numa is not None and eff_disable:
                raise ValueError(
                    "numaNode and disableNuma cannot both be set (host "
                    f"{node.get('hostName')!r}); perftest rejects "
                    "--numa_node together with --disable_numa.")
        if not nodes and global_numa is not None and global_disable:
            raise ValueError(
                "numaNode and disableNuma cannot both be set; perftest "
                "rejects --numa_node together with --disable_numa.")

        bind_to = self.data.get('mpiBindTo')
        if bind_to not in (None, '') and not isinstance(bind_to, str):
            raise ValueError("mpiBindTo must be a string")

    def _validate_gid_index(self) -> None:
        """Validate gidIndex (perftest -x). -1 is perftest's auto sentinel, so
        an explicit value must be an int >= 0 (omit the field for auto)."""
        def _check(value: Any, where: str) -> None:
            if value is None:
                return
            # bool is a subclass of int; reject it explicitly.
            if isinstance(value, bool) or not isinstance(value, int):
                raise ValueError(
                    f"gidIndex must be an integer >= 0 ({where}), got {value!r}")
            if value < 0:
                raise ValueError(
                    f"gidIndex must be >= 0 ({where}); omit it for auto")

        _check(self.data.get('gidIndex'), "top-level")
        for node in self.data.get('testNodes', []):
            _check(node.get('gidIndex'), f"host {node.get('hostName')!r}")

    def to_dict(self) -> Dict[str, Any]:
        return dict(self.data)

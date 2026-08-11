"""
orchestrator.py - Core orchestration logic for perftest cluster mode
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from collections import Counter
from typing import List, Dict, Any, Optional

from .traffic_patterns import (
    TrafficPattern, RankAssignment, resolve_pattern,
    build_occurrence_map, resolve_occurrence_aware,
)
from .config_model import ClusterConfig, TYPED_PERFTEST_FIELDS
from .config_parser import expand_test_nodes
from .discovery import run_auto_detect
from .mpi_command import build_mpirun_command
from .mpi_env import build_env, detect_openmpi_root, libnuma_available
from .metrics import logical_connection_count
from .perftest_output import (
    CLUSTER_PHASE_PREFIX, PARAM_LINE_RE, TEST_NAME_RE, SEPARATOR_RE,
)
from .rendering import (
    render_assignment_table, render_bw_json, render_bw_results,
    render_lat_json, render_lat_results,
)
from .remote import check_versions_on_hosts
from .result_schema import BwResult, LatResult, parse_result

# Raw GPU flags that should not appear in --perftest-args when using the new GPU config system
_RAW_GPU_FLAGS_RE = re.compile(
    r'--use_(cuda|cuda_bus_id|cuda_dmabuf|cuda_pcie_mapping|data_direct|'
    r'rocm|rocm_dmabuf|neuron|neuron_dmabuf|hl|mlu|mlu_dmabuf|opencl)\b'
)


class Orchestrator:
    def __init__(self):
        self.config: Dict[str, Any] = {}
        self.rank_assignments: List[RankAssignment] = []
        self.gpu_map: Dict[str, dict] = {}

    def load_config(self, config_path: str) -> None:
        self.config = ClusterConfig.from_file(config_path).validated().to_dict()

    def set_cli_config(self, hosts: str, pattern: str,
                       binary: str = 'ib_write_bw', port: int = 18515,
                       streams: int = 1, perftest_args: str = '') -> None:
        self.config = ClusterConfig.from_cli(
            hosts=hosts, pattern=pattern, binary=binary,
            port=port, streams=streams, perftest_args=perftest_args,
        ).validated().to_dict()

    def resolve_ranks(self) -> List[RankAssignment]:
        (hosts, device_map, gpu_map, perftest_args_map,
         typed_fields_map, peer_address_map) = expand_test_nodes(
            self.config.get('testNodes', []))
        pattern = TrafficPattern(self.config.get('trafficPattern', 'O2O'))
        base_port = self.config.get('port', 18515)
        streams = self.config.get('streams', 1)

        # Merge top-level GPU defaults into per-host gpu_map at field level
        # (not all-or-nothing), so a per-node gpuDeviceBusId doesn't silently
        # shadow the top-level gpuType and leave _build_gpu_flags skipping
        # the host. A gpu_map entry may be a single dict or a list of dicts
        # (one per occurrence, for a repeated hostname with per-NIC GPU
        # overrides); merge globals into every dict in either shape.
        global_gpu_type = self.config.get('gpuType', '')
        global_gpu_id = self.config.get('gpuDeviceId', -1)
        global_gpu_bus_id = self.config.get('gpuDeviceBusId', '')
        if global_gpu_type or global_gpu_id >= 0 or global_gpu_bus_id:
            def _merge_defaults(entry: Dict[str, Any]) -> Dict[str, Any]:
                entry = dict(entry)
                if not entry.get('gpuType', ''):
                    entry['gpuType'] = global_gpu_type
                if entry.get('gpuDeviceId', -1) < 0:
                    entry['gpuDeviceId'] = global_gpu_id
                if not entry.get('gpuDeviceBusId', ''):
                    entry['gpuDeviceBusId'] = global_gpu_bus_id
                return entry

            def _has_gpu(entry: Dict[str, Any]) -> bool:
                return bool(entry.get('gpuType') or entry.get('gpuDeviceId', -1) >= 0
                           or entry.get('gpuDeviceBusId'))

            for h in hosts:
                existing = gpu_map.get(h, {})
                if isinstance(existing, list):
                    merged = [_merge_defaults(e) for e in existing]
                    if any(_has_gpu(e) for e in merged):
                        gpu_map[h] = merged
                else:
                    merged = _merge_defaults(existing)
                    if _has_gpu(merged):
                        gpu_map[h] = merged

        # Top-level typed fields are the test-level defaults (already merged
        # from tests[] by expand_test_configs). Per-host values override them.
        global_typed = {
            f.name: self.config[f.name]
            for f in TYPED_PERFTEST_FIELDS
            if f.name in self.config
        }

        self.gpu_map = gpu_map
        self.rank_assignments = resolve_pattern(
            hosts, pattern, base_port, device_map, streams, gpu_map=gpu_map,
            peer_address_map=peer_address_map,
        )

        # Per-rank ports (base_port + connection_id*streams + stream_index)
        # aren't bounded upstream. Left unchecked, an overflowing port would
        # be silently truncated to 16 bits in the C layer (sin_port) and
        # could collide with another connection's port - hard to diagnose on
        # real hardware, so fail fast here instead.
        max_port = max((a.port for a in self.rank_assignments), default=0)
        if max_port > 65535:
            num_logical = logical_connection_count(self.rank_assignments)
            raise ValueError(
                f"Port range overflow: {num_logical} connection(s) x "
                f"{streams} stream(s) starting at port {base_port} would "
                f"need ports up to {max_port}, which exceeds the valid TCP "
                f"port range (1-65535). Lower 'port'/--port or 'streams', or "
                f"use fewer hosts/connections for this traffic pattern.")

        occ_map = build_occurrence_map(
            [(a.hostname, a.node_index) for a in self.rank_assignments])
        for assignment in self.rank_assignments:
            assignment.perftest_args = resolve_occurrence_aware(
                perftest_args_map, assignment.hostname, assignment.node_index,
                occ_map, '')
            host_typed = resolve_occurrence_aware(
                typed_fields_map, assignment.hostname, assignment.node_index,
                occ_map, {})
            assignment.typed_perftest_fields = {**global_typed, **host_typed}
        return self.rank_assignments

    def _build_rank_map(self) -> Dict[int, RankAssignment]:
        return {a.rank: a for a in self.rank_assignments}

    def _get_num_logical_connections(self) -> int:
        return logical_connection_count(self.rank_assignments)

    # ------------------------------------------------------------------
    # Validation
    # ------------------------------------------------------------------

    def _validate_config(self, out=None) -> None:
        """Validate GPU config before launch. Warns or raises ValueError on errors."""
        if out is None:
            out = sys.stdout

        self.config = ClusterConfig(self.config).normalized().validated().to_dict()

        perftest_args = self.config.get('perftestArgs', '')
        gpu_type = self.config.get('gpuType', '')
        auto_detect = self.config.get('autoDetect', False)

        # Warn about mixing raw GPU flags with structured JSON GPU config.
        raw_matches = _RAW_GPU_FLAGS_RE.findall(perftest_args)
        has_structured_gpu = bool(
            gpu_type
            or self.config.get('gpuDeviceId', -1) >= 0
            or self.config.get('gpuDeviceBusId', '')
            or self.config.get('cudaMemType', '')
            or self.config.get('cudaDmabuf', False)
            or self.config.get('dataDirectMode', False)
            or any(
                n.get('gpuType', '') or n.get('gpuDeviceId', -1) >= 0
                or n.get('gpuDeviceBusId', '')
                for n in self.config.get('testNodes', [])
            )
        )
        if raw_matches and has_structured_gpu:
            flag_name = f'--use_{raw_matches[0]}'
            print(
                f"[Cluster] WARNING: {flag_name} detected in --perftest-args.\n"
                f"  Structured GPU config is also present in JSON.\n"
                f"  Raw GPU flags apply globally and can conflict with per-host "
                f"JSON GPU assignments.",
                file=out,
            )

        # GPU type set but no device assignment and no auto-detect -> warn, default to 0.
        if gpu_type and not auto_detect:
            global_dev_id = self.config.get('gpuDeviceId', -1)
            global_bus_id = self.config.get('gpuDeviceBusId', '')
            any_per_node = any(
                n.get('gpuDeviceId', -1) >= 0 or n.get('gpuDeviceBusId', '')
                for n in self.config.get('testNodes', [])
            )
            if not any_per_node and global_dev_id < 0 and not global_bus_id:
                print(
                    f"[Cluster] WARNING: gpuType {gpu_type} specified without "
                    f"gpuDeviceId/gpuDeviceBusId. Defaulting to device 0 on all hosts. "
                    f"Use gpuDeviceId/gpuDeviceBusId or autoDetect in JSON to "
                    f"select a specific GPU.",
                    file=out,
                )

    def _check_perftest_versions(self, out=None) -> None:
        """Check perftest version consistency across all worker hosts; warn if mismatched."""
        if out is None:
            out = sys.stdout
        binary = self.config.get('perftestBinary', 'ib_write_bw')
        unique_hosts = sorted(set(a.hostname for a in self.rank_assignments))
        if len(unique_hosts) < 2:
            return
        versions, errors = check_versions_on_hosts(unique_hosts, binary)
        if errors:
            print("[Cluster] WARNING: could not determine perftest version "
                  "on some hosts (treated separately from a version "
                  "mismatch):", file=out)
            for host in sorted(errors):
                print(f"  {host}: {errors[host]}", file=out)
        unique_versions = set(versions.values())
        if len(unique_versions) > 1:
            print("[Cluster] WARNING: perftest version mismatch across hosts:", file=out)
            for host in sorted(versions):
                print(f"  {host}: {versions[host]}", file=out)
            print("  This may cause compatibility issues.", file=out)

    def _sync_gpu_map_to_config(self) -> None:
        """Persist resolved GPU choices into config so rendering uses config only.

        `gpu` may be a single dict (applied to every testNodes[] entry for
        that hostname) or a list of dicts (applied positionally, one per
        occurrence).
        """
        if not self.gpu_map:
            return

        nodes = self.config.setdefault('testNodes', [])
        for host, gpu in self.gpu_map.items():
            matching_nodes = [n for n in nodes if n.get('hostName') == host]

            if isinstance(gpu, list):
                while len(matching_nodes) < len(gpu):
                    new_node = {'hostName': host}
                    nodes.append(new_node)
                    matching_nodes.append(new_node)
                pairs = list(zip(matching_nodes, gpu))
            else:
                if not matching_nodes:
                    matching_nodes = [{'hostName': host}]
                    nodes.append(matching_nodes[0])
                # A uniform (non-list) value applies to every occurrence.
                pairs = [(node, gpu) for node in matching_nodes]

            for target, g in pairs:
                if not (g.get('gpuType') or g.get('gpuDeviceId', -1) >= 0
                        or g.get('gpuDeviceBusId', '')):
                    continue
                if g.get('gpuType'):
                    target['gpuType'] = g['gpuType']
                if g.get('gpuDeviceId', -1) >= 0:
                    target['gpuDeviceId'] = g['gpuDeviceId']
                if g.get('gpuDeviceBusId', ''):
                    target['gpuDeviceBusId'] = g['gpuDeviceBusId']

    def _save_json_report(self, result, params_lines: List[str],
                          path: str) -> Optional[str]:
        """Render the full JSON result report to `path`, independent of the
        terminal --output-format. Creates parent directories as needed and
        writes atomically (temp file + rename) so a write failure or
        interruption never leaves a partial/corrupt report at `path`.

        Returns an error message on failure, None on success.
        """
        directory = os.path.dirname(path) or '.'
        tmp_path = None
        try:
            os.makedirs(directory, exist_ok=True)
            fd, tmp_path = tempfile.mkstemp(
                dir=directory, prefix='.jsonout-', suffix='.tmp')
            with os.fdopen(fd, 'w') as f:
                if isinstance(result, LatResult):
                    render_lat_json(result, params_lines, self.config,
                                    self.rank_assignments, self.gpu_map, out=f)
                else:
                    render_bw_json(result, params_lines, self.config,
                                   self.rank_assignments, self.gpu_map, out=f)
            os.replace(tmp_path, path)
            tmp_path = None
        except OSError as exc:
            return str(exc)
        finally:
            if tmp_path is not None:
                try:
                    os.unlink(tmp_path)
                except OSError:
                    pass
        return None

    # ------------------------------------------------------------------
    # Run
    # ------------------------------------------------------------------

    def run(self, dry_run: bool = False, verbose: bool = False,
            output_format: str = 'table',
            json_output_path: Optional[str] = None) -> int:
        is_json = output_format == 'json'
        info_out = sys.stderr if is_json else sys.stdout

        # Use verbose from config if not explicitly passed via CLI
        if not verbose and self.config.get('verbose'):
            verbose = True

        self._validate_config(info_out)

        def _mpirun_resolvable(path: str) -> bool:
            if os.path.isabs(path):
                return os.path.isfile(path) and os.access(path, os.X_OK)
            return shutil.which(path) is not None

        mpirun_path = self.config.get('mpirunPath', 'mpirun')
        mpi_env = None

        # mpirun not resolvable via config/PATH: fall back to auto-detecting
        # a usable OpenMPI install (same algorithm as set_mpi) instead of
        # requiring the launcher to source it first. Never overrides an
        # explicit mpirunPath.
        if mpirun_path == 'mpirun' and not _mpirun_resolvable(mpirun_path):
            root = detect_openmpi_root()
            if root:
                print(f"[Cluster] Auto-detected OpenMPI at {root}", file=info_out)
                mpirun_path = os.path.join(root, 'bin', 'mpirun')
                self.config['mpirunPath'] = mpirun_path
                # Only fill mpiPrefix if unset - lets remote ranks pick up
                # the same PATH/LD_LIBRARY_PATH via mpirun --prefix.
                if not self.config.get('mpiPrefix'):
                    self.config['mpiPrefix'] = root
                mpi_env = build_env(root)

        if not _mpirun_resolvable(mpirun_path):
            print(f"\nERROR: mpirun not found or not executable: {mpirun_path}",
                  file=sys.stderr)
            print("Auto-detection also failed. Set JSON mpirunPath, add Open "
                  "MPI bin/ to PATH, or source set_mpi.", file=sys.stderr)
            sys.exit(1)

        self.resolve_ranks()

        if (any(a.typed_perftest_fields.get('numaNode') is not None
                for a in self.rank_assignments)
                and not self.config.get('mpiBindTo')
                and not libnuma_available()):
            print("[Cluster] WARNING: numaNode is set but libnuma.so was not "
                  "found on the launcher. perftest treats an explicit "
                  "--numa_node without libnuma as fatal, so workers will "
                  "likely fail. Install libnuma (numactl-devel/libnuma-dev) "
                  "on all hosts, or remove numaNode.", file=info_out)

        if self.config.get('autoDetect'):
            self.gpu_map = run_auto_detect(
                self.rank_assignments, self.gpu_map, info_out)
            self._sync_gpu_map_to_config()

        self._check_perftest_versions(info_out)

        host_counts = Counter(a.hostname for a in self.rank_assignments)
        num_logical = self._get_num_logical_connections()
        num_workers = len(self.rank_assignments)
        streams = self.config.get('streams', 1)
        pattern_name = self.config.get('trafficPattern', 'O2O')

        command, result_file = build_mpirun_command(self.config, self.rank_assignments)

        print(f"[Cluster] Traffic: {pattern_name} | "
              f"Hosts: {len(host_counts)} | "
              f"Connections: {num_logical} | "
              f"Streams: {streams} | "
              f"Workers: {num_workers} | "
              f"MPI Ranks: {num_workers + 1}", file=info_out)

        if dry_run or verbose:
            render_assignment_table(self.rank_assignments, self.config, info_out)

        if dry_run:
            print(f"\n  Command:\n  {command}\n", file=info_out)
            print("[DRY RUN] Command not executed", file=info_out)
            return 0

        proc = subprocess.Popen(
            ['bash', '-c', command],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            env=mpi_env,
        )

        # Drain stderr on a background thread concurrently with the stdout
        # loop below - mpirun/orted/PMIx/ssh can write enough stderr to fill
        # its OS pipe buffer, which would deadlock this loop if only stdout
        # were read (the classic two-pipe subprocess deadlock).
        stderr_chunks: List[str] = []

        def _drain_stderr() -> None:
            if proc.stderr:
                stderr_chunks.append(proc.stderr.read())

        stderr_thread = threading.Thread(target=_drain_stderr, daemon=True)
        stderr_thread.start()

        params_lines: List[str] = []
        params_captured = False
        in_params_block = False

        for line in proc.stdout:
            line = line.rstrip('\n')

            if line.startswith(CLUSTER_PHASE_PREFIX):
                print(line, file=info_out, flush=True)
                continue

            if not params_captured:
                if TEST_NAME_RE.search(line):
                    in_params_block = True
                    params_lines.append(line)
                    continue
                if in_params_block:
                    if SEPARATOR_RE.match(line.strip()):
                        params_captured = True
                        in_params_block = False
                    elif PARAM_LINE_RE.match(line):
                        params_lines.append(line)

        proc.wait()
        stderr_thread.join()
        stderr_output = ''.join(stderr_chunks)

        if proc.returncode != 0:
            print(f"\n[Cluster] ERROR: Test failed (exit code {proc.returncode})",
                  file=sys.stderr)
            if stderr_output.strip():
                for err_line in stderr_output.strip().splitlines():
                    if err_line.strip().startswith('---'):
                        continue
                    print(f"  {err_line}", file=sys.stderr)
            return proc.returncode

        result_data: Optional[Dict[str, Any]] = None
        read_error: Optional[str] = None
        try:
            with open(result_file) as f:
                result_data = json.load(f)
        except (OSError, json.JSONDecodeError) as exc:
            read_error = str(exc)
        finally:
            try:
                os.unlink(result_file)
            except OSError:
                pass

        # mpirun exiting 0 doesn't mean success: a missing/empty/unparseable
        # result file means nothing was gathered, which would otherwise
        # silently report success with no results (e.g. in CI).
        rc = proc.returncode
        if read_error is not None:
            print(f"\n[Cluster] ERROR: Could not read result file {result_file}: "
                  f"{read_error}", file=sys.stderr)
            rc = 1
        elif not result_data:
            print(f"\n[Cluster] ERROR: Result file {result_file} was empty; "
                  f"mpirun exited 0 but no results were gathered.",
                  file=sys.stderr)
            rc = 1
        else:
            try:
                result = parse_result(result_data)
            except ValueError as exc:
                print(f"\n[Cluster] ERROR: Could not parse result data: {exc}",
                      file=sys.stderr)
                rc = 1
            else:
                if is_json:
                    if isinstance(result, LatResult):
                        render_lat_json(result, params_lines, self.config,
                                        self.rank_assignments, self.gpu_map)
                    else:
                        render_bw_json(result, params_lines, self.config,
                                       self.rank_assignments, self.gpu_map)
                else:
                    if isinstance(result, LatResult):
                        render_lat_results(result, params_lines, self.config,
                                           self.rank_assignments, self.gpu_map,
                                           verbose=verbose)
                    else:
                        render_bw_results(result, params_lines, self.config,
                                          self.rank_assignments, self.gpu_map,
                                          verbose=verbose)

                # Persist the full JSON report independent of the terminal
                # --output-format (table stays on stdout). Saved even if
                # data validation below ends up failing the run - the
                # traffic itself completed and its numbers are still real.
                if json_output_path:
                    save_error = self._save_json_report(
                        result, params_lines, json_output_path)
                    if save_error:
                        print(f"\n[Cluster] ERROR: Could not save JSON report "
                              f"to {json_output_path}: {save_error}",
                              file=sys.stderr)
                        rc = 1
                    else:
                        print(f"[Cluster] JSON report saved to "
                              f"{json_output_path}", file=info_out)

                # Traffic completed (mpirun exit 0) but a receiver detected
                # corruption -> fail the run with a clear message.
                if (isinstance(result, BwResult)
                        and result.data_validation.enabled
                        and not result.data_validation.passed):
                    print(f"\n[Cluster] ERROR: DATA VALIDATION FAILED "
                          f"({result.data_validation.errors} mismatch(es) across "
                          f"{len(result.data_validation.workers)} validating rank(s))",
                          file=sys.stderr)
                    rc = 1

        print(f"[Cluster] Done (exit {rc})", file=info_out)
        return rc

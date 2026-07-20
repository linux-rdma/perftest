# Perftest Cluster Mode

Cluster mode transforms perftest from a point-to-point benchmarking tool into a
**multi-host, orchestrated network benchmarking framework**.  A single command
launches coordinated perftest processes across many hosts, synchronizes them via
MPI barriers, collects results, and presents unified output — all without manual
server/client setup on each node.

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [Architecture](#architecture)
3. [Traffic Patterns](#traffic-patterns)
4. [Configuration](#configuration)
5. [CLI Reference](#cli-reference)
6. [GPU / Accelerator Support](#gpu--accelerator-support)
7. [NIC–GPU Auto-Detection](#nicgpu-auto-detection)
8. [Output Formats](#output-formats)
9. [MPI Integration](#mpi-integration)
10. [Hostname Patterns](#hostname-patterns)
11. [Examples](#examples)
12. [Prerequisites](#prerequisites)
13. [Installation](#installation)
14. [Troubleshooting](#troubleshooting)

---

## Quick Start

### Setup Checklist (on every host)

Before running any test, do this on **every** host that will participate
(launcher and workers alike):

1. **Build and install perftest** — `./autogen.sh && ./configure && make -j
   && sudo make install`. Details: [Installation](#installation).
2. **Set up the Open MPI environment** — `source set_mpi.sh` (must be
   `source`d, not executed). Optional on the launcher (it auto-detects Open
   MPI), required on every worker host. Details:
   [OpenMPI Environment Setup](#openmpi-environment-setup).
3. **Set up passwordless SSH** from the launcher to every other host
   (`ssh-keygen` + `ssh-copy-id <remote-host>`). Details:
   [SSH Key Setup](#ssh-key-setup).

Once all three are done on all hosts, run a test from the launcher host:

**Two-host bandwidth test:**

```bash
perftest_cluster run --hosts host-a,host-b --binary ib_write_bw
```

**From a JSON config file:**

```bash
perftest_cluster run -f cluster_mode/examples/one_to_one.json --verbose
```

**Dry-run (print the mpirun command without executing):**

```bash
perftest_cluster run -f config.json --dry-run
```

**Human-readable output, plus a saved JSON report:**

```bash
perftest_cluster run --hosts host-a,host-b --json-output /var/log/perftest/run.json
```

---

## Architecture

Cluster mode builds an **MPMD (Multiple Program, Multiple Data) `mpirun`
command** with three tiers of processes:

```
 Orchestrator (Python, launcher host)
       │
       ▼
   mpirun MPMD
       ├── Rank 0:  C perftest_cluster_worker  (launcher host)
       ├── Rank 1:  C perftest binary  (server, host-a)
       ├── Rank 2:  C perftest binary  (client, host-b)
       └── ...
```

**Rank 0** is always the native C `perftest_cluster_worker` binary
(`src/perftest_cluster_worker.c`, built and installed by `make install` alongside
`ib_write_bw` and friends).  It participates in the same MPI communicator
as the C workers, synchronizing through four barriers and collecting
results via `MPI_Gather` (see [MPI Integration](#mpi-integration) for the
dynamic `libmpi.so` loading shared by all ranks).

**Ranks 1..N** are native C perftest binaries (`ib_write_bw`, `ib_read_lat`,
etc.).  Server ranks are always listed before client ranks so TCP listeners start
before clients attempt to connect.

### Barrier Sequence

```
  BARRIER 0 — Sync before TCP handshake
  BARRIER 1 — All RDMA resources created
  BARRIER 2 — All QPs connected (RTR/RTS)
  BARRIER 3 — Ready for traffic
     ↓
  [ Traffic runs ]
     ↓
  MPI_Gather — Rank 0 collects results from all workers
```

### Key Components

| File | Role |
|------|------|
| `cluster_mode/__main__.py` | CLI entry point |
| `cluster_mode/config_model.py` | Typed config normalization and validation |
| `cluster_mode/orchestrator.py` | High-level orchestration: rank resolution, discovery, mpirun execution, result dispatch |
| `cluster_mode/traffic_patterns.py` | Pattern → rank assignment logic |
| `cluster_mode/config_parser.py` | JSON config parsing, hostname expansion |
| `cluster_mode/mpi_command.py` | Builds the full `mpirun` MPMD command string |
| `cluster_mode/metrics.py` | Shared BW/LAT connection grouping and aggregation |
| `cluster_mode/rendering.py` | Table and JSON result rendering |
| `cluster_mode/remote.py` | Local/SSH command execution helpers |
| `cluster_mode/discovery.py` | GPU auto-detect orchestration |
| `cluster_mode/perftest_output.py` | Perftest stdout parameter parsing |
| `cluster_mode/result_schema.py` | Result dataclasses and JSON result parsing |
| `cluster_mode/mpi_env.py` | Launcher-side OpenMPI auto-detection |
| `src/perftest_cluster_worker.c` | MPI rank 0: barriers, gather, JSON result emit |
| `cluster_mode/nic_gpu_discovery.py` | NIC↔GPU affinity probing (runs on remote hosts) |
| `src/mpi_loader.c` | C-side dynamic MPI loading via `dlopen` (shared by rank 0 and ranks 1..N) |
| `src/perftest_cluster.{c,h}` | Cluster glue: result structs, ABI version, gather helpers, barrier protocol |

---

## Traffic Patterns

| Pattern | Code | Description | Min Hosts |
|---------|------|-------------|-----------|
| One-to-One | `O2O` | Single connection: host A ↔ host B | 2 |
| One-to-Many | `O2M` | First host (client/initiator) connects to all others (servers) | 2 |
| Many-to-One | `M2O` | All hosts (clients) connect to the first host (server) | 2 |
| All-to-All | `A2A` | Every host connects to every other host | 2 |
| Bisection | `B` | Hosts split in half; first half connects to second half | 2 (even) |
| Ring | `R` | Each host connects to the next in a ring topology | 3 |

### Streams

Each traffic pattern supports a **streams** multiplier (`--streams N` or
`"streams": N` in config).  This replicates every logical connection N times on
consecutive ports, increasing parallelism without changing the topology.

---

## Configuration

Cluster mode accepts configuration via **JSON file** (`-f`) or **CLI
arguments** (`--hosts`, `--pattern`, etc.).  When both are provided, CLI
arguments override or supplement the JSON config.

### JSON Config Schema

```json
{
  "testNodes": [
    {
      "hostName": "node-01",
      "deviceName": "mlx5_0",
      "peerAddress": "10.220.95.11",
      "gpuType": "cuda",
      "gpuDeviceId": 0,
      "gpuDeviceBusId": "0000:3b:00.0"
    }
  ],

  "trafficPattern": "O2O",
  "perftestBinary": "ib_write_bw",
  "perftestArgs": "-s 65536 -D 10",
  "port": 18515,
  "streams": 1,
  "mpiSubnet": "10.220.95.0/24",
  "mpirunTimeout": 120,
  "mpirunPath": "/usr/lib64/openmpi/bin/mpirun",
  "mpiPrefix": "/usr/lib64/openmpi",

  "gpuType": "cuda",
  "gpuDeviceId": 0,
  "gpuDeviceBusId": "0000:3b:00.0",
  "cudaMemType": "device",
  "cudaDmabuf": true,
  "dataDirectMode": true,
  "autoDetect": true,

  "perftestClusterWorkerDir": "/usr/local/bin",

  "jsonOutputFile": "/var/log/perftest/run.json"
}
```

#### Top-Level Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `testNodes` | array | required | Host definitions (supports hostname patterns) |
| `tests` | array | none | Sequential test entries; root fields are defaults for each entry |
| `name` | string | `perftestBinary` | Optional test name used in sequential test banners |
| `iterations` | int | `1` | Repeat a test entry N times as separate sequential runs |
| `trafficPattern` | string | `"O2O"` | Traffic pattern code |
| `perftestBinary` | string | `"ib_write_bw"` | Perftest binary name or path |
| `perftestArgs` | string | `""` | Extra arguments passed to the binary |
| `port` | int | `18515` | Base port (orchestrator assigns per-rank ports) |
| `streams` | int | `1` | Parallel streams per connection |
| `mpiSubnet` | string | none | Subnet for `--mca btl_tcp_if_include` |
| `mpirunTimeout` | int | none | `mpirun --timeout` value in seconds |
| `mpirunPath` | string | `"mpirun"` | Launcher-local `mpirun` binary path or command name |
| `mpiPrefix` | string | none | Open MPI install prefix passed as `mpirun --prefix` for remote runtime setup |
| `mpiBindTo` | string | `auto` | `mpirun --bind-to` value. `auto` (the default) is equivalent to omitting the field: `none` when the launcher has libnuma (perftest self-binds NIC-local), otherwise Open MPI default binding. Any other value (e.g. `none`/`core`/`numa`) is passed to `mpirun --bind-to` verbatim (see [NUMA Affinity](#numa-affinity)) |
| `perftestClusterWorkerDir` | string | none | Directory with `perftest_cluster_worker` (see [Custom Install Location](#custom-install-location-for-perftest_cluster_worker)) |
| `jsonOutputFile` | string | none | Save the full JSON result report to this file (see [Persistent JSON Reports](#persistent-json-reports)). Mutually exclusive with `jsonOutputDir` |
| `jsonOutputDir` | string | none | Save one generated JSON result report per executed test into this directory. Mutually exclusive with `jsonOutputFile` |

#### Per-Node Fields (inside `testNodes[]`)

| Field | Type | Description |
|-------|------|-------------|
| `hostName` | string | Hostname or pattern (e.g., `"node-[01-04]"`); used only for `mpirun`/SSH placement |
| `deviceName` | string | RDMA device name (e.g., `"mlx5_0"`) |
| `perftestArgs` | string | Extra perftest args for ranks on this host; appended after global `perftestArgs` |
| `peerAddress` | string | Address peers use to connect to this host (e.g. its RDMA-capable interface IP), overriding `hostName` for that purpose only. Required on every `testNodes[]` entry when `useRdmaCm` is set - see [Peer Connect Address](#peer-connect-address-peeraddress) |
| `gpuType` | string | GPU type: `cuda`, `rocm`, `neuron`, `hl`, `mlu`, `opencl` |
| `gpuDeviceId` | int | Numeric GPU device index. Mutually exclusive with `autoDetect` |
| `gpuDeviceBusId` | string | PCIe bus ID (takes precedence over numeric ID). Mutually exclusive with `autoDetect` |
| `numaNode` | int | Bind ranks on this host to this NUMA node (perftest `--numa_node`); overrides the global value. Mutually exclusive with `disableNuma` (see [NUMA Affinity](#numa-affinity)) |
| `disableNuma` | bool | Disable perftest NUMA auto-detection/binding for ranks on this host (perftest `--disable_numa`) |
| `gidIndex` | int | Per-host GID index (perftest `-x`); overrides the global value. RoCE fabrics often need a specific index (e.g. 3 for RoCE v2) |

#### GPU Fields (global)

| Field | Type | Description |
|-------|------|-------------|
| `gpuType` | string | Default GPU type for all hosts |
| `gpuDeviceId` | int | Default GPU device index. Mutually exclusive with `autoDetect` |
| `gpuDeviceBusId` | string | Default GPU PCIe bus ID. Mutually exclusive with `autoDetect` |
| `cudaMemType` | string | CUDA memory type: `device`, `managed`, `pinned`, `vmm`, `auto` (default `auto` = non-localized VMM, falls back to device) |
| `cudaDmabuf` | bool | Enable CUDA DMA-BUF |
| `dataDirectMode` | bool | Enable Data Direct mode |
| `autoDetect` | bool | SSH-probe each host for NIC↔GPU affinity. Rejected together with an explicit `gpuDeviceId`/`gpuDeviceBusId` (global or per-node) |

#### `tests` Array

Configs may optionally contain a `"tests"` array.  Each entry is run
sequentially.  Root-level fields act as defaults, and each test entry can
override them.  A test entry may set `"iterations": N`; every iteration is
expanded into a separate run and reported with its own banner.  Ports can be
reused safely because tests run one at a time.  Execution stops at the first
failing test.

Top-level `perftestArgs` applies to every worker.  `testNodes[].perftestArgs`
is appended after the global args for ranks on that host, including hosts
expanded from patterns.  Use node-level args carefully: many perftest options
must match between peers.  `-p` / `--port` is not allowed in either location;
use the JSON `port` field instead.

`--output-format json` is supported only for a single expanded test run.
Use table output for sequential multi-test configs.

For saving reports from a multi-test config, use `jsonOutputDir` (one file per
test) rather than a single `jsonOutputFile` — see
[Persistent JSON Reports](#persistent-json-reports).

---

## CLI Reference

```
perftest_cluster run [options]
```

### Input

| Flag | Description |
|------|-------------|
| `-f`, `--file FILE` | JSON configuration file |
| `--hosts HOST_LIST` | Comma-separated hostnames (supports patterns) |

One of `--file` or `--hosts` is required.

### Test Parameters

| Flag | Default | Description |
|------|---------|-------------|
| `--pattern PATTERN` | `O2O` | Traffic pattern: `O2O`, `O2M`, `M2O`, `A2A`, `B`, `R` |
| `--binary NAME` | `ib_write_bw` | Perftest binary name or path |
| `--port PORT` | `18515` | Base port number |
| `--streams N` | `1` | Parallel streams per connection |
| `--perftest-args ARGS` | `""` | Extra, non-typed arguments for the perftest binary |
| `--mpi-subnet CIDR` | none | Subnet for MPI TCP transport |
| `--help-json` | n/a | Show supported JSON configuration fields and exit |

### Typed Perftest Parameters

Every typed JSON field (`TYPED_PERFTEST_FIELDS` in
[cluster_mode/config_model.py](config_model.py)) also has a matching
CLI flag, auto-generated so the CLI can never drift from the JSON schema. Each
flag has a **kebab-case canonical name** plus, where perftest already defines
one, the **perftest-native long flag as an alias**:

| JSON field | CLI flag(s) | Type | perftest flag |
|------------|-------------|------|----------------|
| `messageSize` | `--message-size`, `--size` | int | `-s` |
| `qpsPerProcess` | `--qps-per-process`, `--qp` | int | `-q` |
| `duration` | `--duration` | int | `-D` |
| `perftestIters` | `--perftest-iters`, `--iters` | int | `-n` |
| `bidirectional` | `--bidirectional` | bool | `-b` |
| `reportGbits` | `--report-gbits`, `--report_gbits` | bool | `--report_gbits` |
| `connectionType` | `--connection-type`, `--connection` | str | `-c` |
| `mtu` | `--mtu` | int | `-m` |
| `inlineSize` | `--inline-size`, `--inline_size` | int | `-I` |
| `txDepth` | `--tx-depth` | int | `-t` |
| `rxDepth` | `--rx-depth` | int | `-r` |
| `postList` | `--post-list`, `--post_list` | int | `-l` |
| `cqModeration` | `--cq-moderation`, `--cq-mod` | int | `-Q` |
| `useRdmaCm` | `--use-rdma-cm`, `--rdma_cm` | bool | `-R` |
| `noEnhancedReorder` | `--no-enhanced-reorder`, `--no_enhanced_reorder` | bool | `--no_enhanced_reorder` |
| `dataValidation` | `--data-validation`, `--data_validation` | bool | `--data_validation` |
| `dataValidationDebug` | `--data-validation-debug`, `--data_validation_debug` | bool | `--data_validation_debug` |
| `numaNode` | `--numa-node`, `--numa_node` | int | `--numa_node` |
| `disableNuma` | `--disable-numa`, `--disable_numa` | bool | `--disable_numa` |
| `gidIndex` | `--gid-index` | int | `-x` |

```bash
perftest_cluster run --hosts h1,h2 --duration 10 --message-size 65536 --perftest-args "-u 10"
```

Notes:
- Unset typed flags default to `None` and are omitted entirely, so they never
  override a value set in a `-f` JSON file. When both are given, **the CLI
  value wins** (same precedence as `--streams`/`--mpi-subnet`).
- These flags are global (top-level) only; per-`testNodes[]` overrides still
  require JSON.
- `gidIndex` (perftest `-x`) is per-node overridable via JSON, so mixed RoCE
  fabrics can pin a different GID index per host (e.g. `3` for RoCE v2).
- Do not also pass a typed flag's equivalent inside `--perftest-args`
  (e.g. `--perftest-args "-D 5"` alongside `--duration 10`) — this is
  rejected, the same guard that already applies to `perftestArgs` in JSON.
- **`useRdmaCm` requires per-node `peerAddress`**, so it needs a JSON config
  (`-f`) and cannot be combined with `--hosts` — see
  [Peer Connect Address](#peer-connect-address-peeraddress).

### NUMA Affinity

Each perftest worker already binds itself to its NIC's NUMA node by default —
`set_process_affinity()` reads the device's `numa_node` from sysfs and pins
CPU + memory to it — **provided libnuma is available** (perftest `dlopen`s the
unversioned `libnuma.so`, which comes from the `numactl-devel` / `libnuma-dev`
package).

The catch under cluster mode is Open MPI: its default process binding
restricts each rank's CPU set, and perftest then auto-disables its own NUMA
binding when it detects that restriction. Cluster mode resolves this
automatically:

- **Launcher has libnuma** → cluster mode emits `mpirun --bind-to none`, so
  Open MPI stays out of the way and each worker self-binds to its NIC-local
  NUMA node.
- **Launcher lacks libnuma** → perftest cannot bind anyway, so cluster mode
  omits `--bind-to` and leaves Open MPI's default binding in place (better
  than fully unbound).

libnuma is detected once on the launcher and used as a proxy for the whole
cluster, so **install libnuma on all hosts** (`numactl-libs` +
`numactl-devel` on RPM, `libnuma1` + `libnuma-dev` on Debian) for a
homogeneous result.

Overrides:

- `mpiBindTo` (top-level) forces the `mpirun --bind-to` value regardless of
  detection — e.g. `"mpiBindTo": "core"` or `"numa"` to hand binding back to
  Open MPI, or `"none"` to force perftest ownership. The default `"auto"`
  (or omitting the field) selects the libnuma-based behavior described above.
- `numaNode` (global or per-node) pins ranks to a specific node via perftest
  `--numa_node`. Setting it without libnuma present makes perftest fail, so
  cluster mode prints a warning at launch in that case.
- `disableNuma` (global or per-node) turns off perftest's NUMA binding
  (`--disable_numa`). It is mutually exclusive with `numaNode`.

```json
{
  "testNodes": [
    {"hostName": "node-01", "deviceName": "mlx5_0", "numaNode": 0},
    {"hostName": "node-02", "deviceName": "mlx5_2", "numaNode": 1}
  ]
}
```

### Peer Connect Address (`peerAddress`)

`hostName` serves two different purposes today: it's the target for
`mpirun -H`/SSH placement, and — unless overridden — it's also the address a
client rank passes to perftest as its connect argument. For most setups those
coincide, so nothing extra is needed.

They diverge when `hostName` is a management hostname that doesn't route over
the RDMA-capable interface. This matters most for **RDMA CM**
(`useRdmaCm`/`-R`): perftest resolves that connect string via
`rdma_resolve_addr()` to pick the connection's device/GID, so a mismatched
address doesn't just fail to connect — it can silently establish the
connection over the wrong physical link. Set `peerAddress` per node
(e.g. its RDMA interface IP) to decouple the connect address from `hostName`:

```json
{
  "testNodes": [
    {"hostName": "node-01", "deviceName": "mlx5_0", "peerAddress": "10.220.95.11"},
    {"hostName": "node-02", "deviceName": "mlx5_0", "peerAddress": "10.220.95.12"}
  ],
  "useRdmaCm": true
}
```

Because a wrong-but-reachable address for RDMA CM fails silently rather than
loudly, **`useRdmaCm` requires `peerAddress` on every `testNodes[]` entry** —
checked before hostname-pattern expansion, so there's no way to set it on some
occurrences of a repeated hostname (e.g. one NIC of a multi-NIC host) and
forget it on others. Without `useRdmaCm`, `peerAddress` is optional; a client
without one falls back to `peer_host` (today's behavior, unchanged).

Notes:
- `peerAddress` does not support hostname-pattern expansion the way
  `hostName` does — a single `"hostName": "node-[01-04]"` entry with one
  `peerAddress` would apply that same address to all four expanded hosts.
  Give each concrete host its own `testNodes[]` entry instead.
- `peerAddress` only matters for the node acting as **server** in the
  resolved topology; setting it on a host that's only ever a client in a
  given traffic pattern is a harmless no-op.
- Distinct from `mpiSubnet` (which network interface Open MPI's own
  control-plane traffic uses) and unrelated to `mpiPrefix`/`mpirunPath`.

### GPU / Accelerator

GPU/accelerator settings are configured through JSON (or low-level
`--perftest-args`), not dedicated CLI flags — this avoids precedence conflicts
between global CLI options and per-host JSON assignments. See
[GPU / Accelerator Support](#gpu--accelerator-support).

### Deployment

| Flag | Description |
|------|-------------|
| `--binary-dir DIR` | Directory containing compiled cluster mode binaries (see [Custom Install Location](#custom-install-location-for-perftest_cluster_worker)) |

### Output Control

| Flag | Description |
|------|-------------|
| `--dry-run` | Print the mpirun command without executing |
| `--verbose` | Show per-stream detail |
| `--output-format FORMAT` | `table` (default, human-readable) or `json` (machine-parseable) |
| `--json-output FILE` | Also save the full JSON result report to FILE, independent of `--output-format` (see [Persistent JSON Reports](#persistent-json-reports)) |
| `--json-output-dir DIR` | Also save one generated JSON result report per executed test into DIR |

`--json-output` and `--json-output-dir` are mutually exclusive on the CLI.
A CLI value overrides and clears whichever of `jsonOutputFile`/`jsonOutputDir`
the JSON config set.

---

## GPU / Accelerator Support

Cluster mode supports RDMA with GPU memory for multiple accelerator types.
GPU flags are injected per-rank into the perftest command line.

### Supported GPU Types

| Type | Flag | Device ID Format |
|------|------|------------------|
| `cuda` | `--use_cuda` / `--use_cuda_bus_id` | Numeric or PCIe bus ID |
| `rocm` | `--use_rocm` | Numeric |
| `neuron` | `--use_neuron` | Numeric |
| `hl` | `--use_hl` | PCIe bus ID only |
| `mlu` | `--use_mlu` | Numeric |
| `opencl` | `--use_opencl` | Numeric |

PCIe bus ID takes precedence over numeric device ID when both are specified.

### CUDA-Specific Options

- **DMA-BUF** (`cudaDmabuf`): injects `--use_cuda_dmabuf`
- **Data Direct** (`dataDirectMode`): injects both `--use_cuda_dmabuf` and
  `--use_data_direct`
- **Memory Type** (`cudaMemType`): injects `--cuda_mem_type=` with
  `device` (0), `managed` (1), `pinned` (2), or `vmm` (5). `auto` (the default
  when unset) omits the flag so perftest uses its default: non-localized VMM
  with fallback to device

---

## NIC–GPU Auto-Detection

When `autoDetect` is enabled, the orchestrator SSHs the `nic_gpu_discovery.py`
script to each host and probes:

1. **RDMA devices** from `/sys/class/infiniband/`
2. **NVIDIA GPUs** from `lspci`
3. **Affinity matching** via:
   - VPD (Vital Product Data) for direct NIC↔GPU pairing
   - PCIe topology distance (shortest path wins)
   - NUMA tie-breaking

The discovered GPU PCIe bus ID is then assigned to the matching RDMA device,
enabling optimal GPUDirect placement without manual per-host configuration.

**`autoDetect` is mutually exclusive with an explicit `gpuDeviceId`/
`gpuDeviceBusId`** - global or per-node. Auto-detect's whole purpose is
choosing the most suitable GPU for each NIC itself, so combining it with a
hardcoded ID/bus is a contradictory instruction; the config validator
rejects it outright rather than picking a silent precedence rule. Either
let `autoDetect` choose the GPU for every host, or pin GPUs explicitly and
leave `autoDetect` unset/false.

---

## Output Formats

### Table (default)

Human-readable output with fio-style sections:

```
=== Cluster BW Test ===
  binary        : ib_write_bw
  pattern       : O2O
  hosts         : 2  (host-a, host-b)
  message size  : 65536 B
  ...

=== Results ===
  host-b -> host-a
    bw            : 22062.30 MB/s
    msgrate       : 0.352997 Mpps

=== Summary ===
  bw_total      : 22062.30 MB/s
  bw_avg        : 22062.30 MB/s
  msgrate       : 0.352997 Mpps
```

With `--verbose`, per-stream details are shown for each connection.

### JSON (`--output-format json`)

Machine-parseable output with full test configuration, per-connection detail,
and summary totals.  Human-readable `[Cluster]` status messages are printed to
stderr to avoid polluting the JSON output.

### Persistent JSON Reports

`--json-output FILE` / `--json-output-dir DIR` (or the equivalent
`jsonOutputFile`/`jsonOutputDir` JSON fields) save the same full JSON report
as `--output-format json` to disk, **independent of `--output-format`** — the
default human-readable table still goes to stdout, and a saved report is
written alongside it:

```bash
perftest_cluster run --hosts host-a,host-b --json-output /var/log/perftest/run.json
```

For a sequential/`iterations` config, `--json-output-dir`/`jsonOutputDir`
generates one collision-free filename per executed test, e.g.
`001-write-bw-20260714-161230.json`,
`002-write-bw-iteration-2-20260714-161230.json`, ... :

```bash
perftest_cluster run -f cluster_mode/examples/sequential_multi_test.json \
    --json-output-dir /var/log/perftest/
```

`--json-output`/`jsonOutputFile` is a single explicit path; using it for more
than one test in a sequential config is rejected up front (before any test
runs) rather than letting later tests silently overwrite earlier reports —
use `--json-output-dir` for more than one test, or set a distinct
`jsonOutputFile` per `tests[]` entry.

Notes:
- The destination directory (and any missing parent directories) is created
  automatically; the report itself is written atomically, so a failure or
  interruption mid-write never leaves a partial/corrupt file at the final path.
- Generated filenames end with a timestamp (`YYYYMMDD-HHMMSS`), one shared by
  every test in a single `perftest_cluster run` invocation. This is what keeps
  a later run of the same config from overwriting this run's reports — the
  index/name/iteration prefix stays first so a directory listing still groups
  the same logical test position together across runs.
- A report is saved whenever traffic completes and results are gathered,
  even if the run's exit code is still non-zero because of a data-validation
  failure — the traffic itself completed and the numbers are real.
- Nothing is written on `--dry-run`, or if the run fails before results are
  gathered (e.g. `mpirun` itself failing).
- A write failure (e.g. an unwritable path) prints a clear
  `Could not save JSON report to ...` error and fails the run.

---

## MPI Integration

### C-Side: Dynamic MPI Loading (`src/mpi_loader.c`)

The C perftest binary does **not** link against `libmpi.so` at build time.
Instead, it uses `dlopen` to load MPI dynamically at runtime:

1. **Environment check**: if `OMPI_COMM_WORLD_SIZE`, `PMIX_RANK`, or
   `PMI_RANK` are not set → not under `mpirun` → skip MPI entirely (zero
   overhead in standalone mode)
2. **`dlopen("libmpi.so")`**: if under `mpirun` but `dlopen` fails → fatal
   error with diagnostic message
3. **Symbol resolution**: `MPI_Init`, `MPI_Finalize`, `MPI_Barrier`,
   `MPI_Gather`, `MPI_Bcast`, and Open MPI constants (`ompi_mpi_comm_world`,
   `ompi_mpi_byte`) resolved via `dlsym`
4. **Signal preservation**: `SIGALRM` and `SIGUSR1` handlers saved before
   `MPI_Init` and restored after (MPI may overwrite them)

This design ensures:
- **Zero overhead** in standalone mode (no `dlopen`, no `MPI_Init`)
- **No build dependency** on MPI headers or libraries
- **Clear diagnostics** when MPI is expected but unavailable

### Rank 0: Native C `perftest_cluster_worker`

Rank 0 is the `perftest_cluster_worker` binary (`src/perftest_cluster_worker.c`),
using the same dynamic MPI loader (`src/mpi_loader.c`) described above.

It participates in the same four barriers as the C workers, performs
`MPI_Gather` to collect per-rank `cluster_bw_report` / `cluster_lat_report`
structs (defined canonically in `src/perftest_cluster.h`), and writes a
JSON result file consumed by the orchestrator after `mpirun` exits.

It also performs a cross-rank MPI version-match check via `MPI_Bcast` so
mixed-Open-MPI deployments fail fast instead of corrupting later wire
traffic.

### MPI Command Construction

The `mpi_command.py` module builds the full MPMD command:

```
[mpirunPath] [--timeout N] [--prefix PREFIX] [--mca btl_tcp_if_include SUBNET] --oversubscribe \
  -np 1 perftest_cluster_worker --result-kind bw --num-workers 2 --output-file /tmp/... \
  : -np 1 -H host-a ib_write_bw -d mlx5_0 -p 18515 \
  : -np 1 -H host-b ib_write_bw -d mlx5_0 -p 18515 host-a
```

`mpirunPath` controls only how the launcher finds `mpirun`; for example
`"/usr/lib64/openmpi/bin/mpirun"` when Open MPI is not in `PATH`.
`mpiPrefix` is optional and becomes `mpirun --prefix <mpiPrefix>`, which helps
remote hosts find Open MPI's `bin/` and `lib/` directories.  It does not locate
the perftest binary itself; `perftestBinary` must still be on the remote
`PATH` or be an absolute/shared path.

`--oversubscribe` is always added because rank 0 occupies an extra slot
on the launcher host that hostfile/slot counts don't account for.

The rank-0 `perftest_cluster_worker` path is resolved from `$PATH` or from
`perftestClusterWorkerDir` — see
[Custom Install Location](#custom-install-location-for-perftest_cluster_worker).

---

## Hostname Patterns

Hostnames support bracket expansion in both JSON configs and CLI:

| Pattern | Expansion |
|---------|-----------|
| `node-[01-04]` | `node-01`, `node-02`, `node-03`, `node-04` |
| `node-[a,b,c]` | `node-a`, `node-b`, `node-c` |
| `gpu-host-[1-8]` | `gpu-host-1` through `gpu-host-8` |

Zero-padding width is preserved from the range start value.
Commas inside brackets are treated as list separators, not host delimiters.

---

## Examples

Example configuration files are provided in `cluster_mode/examples/`:

| File | Pattern | Description |
|------|---------|-------------|
| `one_to_one.json` | O2O | Simple two-host bandwidth test |
| `all_to_all.json` | A2A | All-to-all with hostname range and MPI subnet |
| `ring.json` | R | Ring topology across 4 hosts |
| `bisection.json` | B | Bisection bandwidth across 8 hosts |
| `gpu_gpudirect.json` | B | Explicit GPU device assignment per host group |
| `gpu_data_direct.json` | B | Data Direct mode with per-host PCIe bus IDs |
| `gpu_auto_detect.json` | B | Automatic NIC↔GPU affinity detection |
| `sequential_multi_test.json` | O2O | Sequential BW and latency tests with iterations |
---

## Prerequisites

### All Hosts

- **Open MPI** installed with `mpirun`, `prted`, and `libmpi.so` accessible
- **`PATH`** includes the Open MPI `bin/` directory
- **`LD_LIBRARY_PATH`** includes the Open MPI `lib/`/`lib64/` directory
- **Passwordless SSH** between all participating hosts

The easiest way to set the Open MPI environment up on every host (including
non-interactive SSH sessions) is [set_mpi.sh](../set_mpi.sh) — see
["OpenMPI Environment Setup"](#openmpi-environment-setup) below.

### OpenMPI Environment Setup

[set_mpi.sh](../set_mpi.sh) detects a usable Open MPI installation and exports
`MPI_DIR`, `MPI_HOME`, `OMPI_HOME`, `PATH`, and `LD_LIBRARY_PATH` for it —
equivalent to adding the following to `~/.bashrc` on every host, with that
host's actual install path:

```bash
export PATH=/usr/lib64/openmpi/bin:$PATH
export LD_LIBRARY_PATH=/usr/lib64/openmpi/lib:${LD_LIBRARY_PATH:-}
```

It prefers a DOCA-style install under `/usr/mpi/gcc/openmpi-*` and otherwise
falls back to whatever Open MPI is on the system (RPM- or DEB-based), so the
same command works across hosts with different installs. Detection order:

1. The newest DOCA-style install under `/usr/mpi/gcc/openmpi-*`, if present.
2. Otherwise, whatever `mpirun` is already resolvable on the system (`PATH`,
   then a handful of common RPM/DEB install roots, then a bounded search).

It works the same way on RPM-based and DEB-based hosts — the difference is
just which install it happens to find. Re-running it (or sourcing it again)
never duplicates `PATH`/`LD_LIBRARY_PATH` entries.

> **Launcher auto-detection:** `perftest_cluster run` runs the same
> detection logic itself (`cluster_mode/mpi_env.py`, a Python port of the
> algorithm above) whenever `mpirun` isn't already resolvable via config or
> `PATH`, and uses whatever it finds for that run — no sourcing required on
> the launcher for the common case. When it fires, it also fills in
> `mpiPrefix` (if not already set in your JSON config) so `mpirun --prefix`
> passes the same `PATH`/`LD_LIBRARY_PATH` through to every remote rank,
> **provided every worker host has Open MPI installed at the same absolute
> path** as the one detected on the launcher. This auto-detection never
> overrides an explicit `mpirunPath`/`mpiPrefix`. It only covers the
> launcher process itself — every **worker** host still needs its own
> working Open MPI install (via `set_mpi.sh` or otherwise); auto-detection
> does not set anything up remotely.

**Current shell** (either form works; pick one):

```bash
# Source it directly - applies to this shell
source set_mpi.sh

# Or emit export statements and eval them
eval "$(set_mpi.sh --emit-exports)"
```

**DOCA OpenMPI present** (e.g. RHEL/DOCA host with
`/usr/mpi/gcc/openmpi-5.0.10.../`):

```bash
$ source set_mpi.sh
[set_mpi] Using OpenMPI at: /usr/mpi/gcc/openmpi-5.0.10rc2.2605050727
```

**DOCA OpenMPI absent** (falls back to whatever is installed, e.g. the
Ubuntu `openmpi-bin` package or an RHEL `openmpi` module):

```bash
# Ubuntu/Debian
sudo apt install -y openmpi-bin libopenmpi-dev
source set_mpi.sh   # falls back to the apt-installed mpirun

# RHEL/CentOS/Fedora
sudo dnf install -y openmpi openmpi-devel
source set_mpi.sh   # falls back to the dnf-installed mpirun
```

If no Open MPI installation can be found anywhere, the script fails clearly
(non-zero exit, message on stderr listing everywhere it looked) instead of
exporting anything; `--emit-exports` prints nothing to stdout in that case,
so a failed `eval "$(...)"` is a safe no-op rather than evaluating an error
message as a command.

**Persist system-wide** (every future login shell, on any host — requires
root):

```bash
sudo set_mpi.sh --install-system-wide
```

This installs a copy of the script and writes `/etc/profile.d/doca-openmpi.sh`,
which re-runs detection at every login and is a no-op if no Open MPI is
found (it will never break a login shell).

**Verify `mpirun` resolves correctly:**

```bash
set_mpi.sh --show      # what would be selected, without changing anything
set_mpi.sh --verify    # applies the environment and actually runs
                           # `which mpirun` + `mpirun --version`
```

### Launcher Host (additionally)

- **Python 3.10+** (orchestrator only; standard library, no third-party deps)
- **`perftest_cluster`**, **`perftest_cluster_worker`**, and **`perftest_nic_gpu_discover`** installed
  (produced by `sudo make install` alongside `ib_write_bw`).

> **Note:** MPI rank 0 is the native C `perftest_cluster_worker` binary.
> The orchestrator uses only the Python standard library; its sources live
> under `${libexecdir}/perftest/cluster_mode/` and are loaded by the
> `perftest_cluster` wrapper at runtime via `sys.path` injection.

### Worker Hosts

- Open MPI (see "All Hosts" above)
- The compiled perftest binary you intend to run (e.g. `ib_write_bw`)
- **`perftest_nic_gpu_discover`** if you intend to use JSON `"autoDetect": true` (otherwise optional)
- Passwordless SSH inbound from the launcher
- **No Python required for the test path itself.** `perftest_cluster_worker` only runs
  on the launcher host (rank 0); workers run only the C perftest binaries.
  `perftest_nic_gpu_discover` (Python) is invoked over SSH on each worker only when
  JSON `"autoDetect": true` is requested.

### SSH Key Setup

```bash
ssh-keygen -t rsa          # if no key exists
ssh-copy-id <remote-host>  # for each participating host
```

---

## Installation

Cluster mode is installed by the standard autotools flow alongside the
perftest workers:

```bash
./autogen.sh && ./configure && make -j && sudo make install
```

### What Gets Installed

| Artifact | Installed to | Source | Role |
|----------|--------------|--------|------|
| `perftest_cluster` | `$bindir` (e.g. `/usr/local/bin`) | `scripts/perftest_cluster.in` | Python wrapper; user-facing CLI (`perftest_cluster run …`) |
| `perftest_cluster_worker` | `$bindir` | [src/perftest_cluster_worker.c](../src/perftest_cluster_worker.c) | MPI rank 0 (launched by `mpirun`) |
| `perftest_nic_gpu_discover` | `$bindir` | [cluster_mode/nic_gpu_discovery.py](nic_gpu_discovery.py) | Per-host NIC↔GPU probe (invoked by orchestrator over SSH for JSON `"autoDetect": true`) |
| `cluster_mode/*.py` | `$libexecdir/perftest/cluster_mode/` | the Python package | Orchestrator implementation, loaded by the `perftest_cluster` wrapper at runtime |
| `ib_write_bw`, `ib_read_lat`, … | `$bindir` | `src/{write,read,send,atomic}_{bw,lat}.c` | MPI ranks 1..N (perftest workers) |

The `perftest_cluster` wrapper is a thin (~30 line) Python script that injects
`$libexecdir/perftest` into `sys.path` and dispatches to `cluster_mode.__main__`.
`autoconf` substitutes `$libexecdir` at build time so the wrapper points at
the right path regardless of `--prefix` / `--libexecdir` configure flags.

### Per-Host Deployment

`perftest_cluster`/`perftest_cluster_worker` are launcher-only; workers need
only the perftest binary plus Open MPI (and `perftest_nic_gpu_discover` if
using `autoDetect`). For the full per-host requirement breakdown see
[Prerequisites](#prerequisites) ("Launcher Host" / "Worker Hosts").

### Custom Install Location for `perftest_cluster_worker`

If `perftest_cluster_worker` is not on `$PATH` on the launcher host, point the
orchestrator at it via the JSON config:

```json
{
  "testNodes": ["host-a", "host-b"],
  "perftestClusterWorkerDir": "/opt/perftest/bin",
  "perftestBinary": "ib_write_bw"
}
```

When `perftestClusterWorkerDir` is set, `mpirun` launches
`<perftestClusterWorkerDir>/perftest_cluster_worker` as rank 0; otherwise it relies on `$PATH`.

---

## Troubleshooting

### `mpirun not found in PATH`

The launcher auto-detects Open MPI before reporting this, so it means no usable
install was found anywhere. Fix the launcher's Open MPI environment (see
[OpenMPI Environment Setup](#openmpi-environment-setup)), or set
`"mpirunPath": "/usr/lib64/openmpi/bin/mpirun"` in JSON.

### `prted: command not found` on remote hosts

Launcher auto-detection can't fix a worker's environment. Set up Open MPI on
that worker ([OpenMPI Environment Setup](#openmpi-environment-setup)), or set
`"mpiPrefix": "/usr/lib64/openmpi"` in JSON so `mpirun --prefix` carries it to
remote ranks.

### `FATAL: Launched under mpirun but cannot load libmpi.so`

`LD_LIBRARY_PATH` lacks the Open MPI library directory on one or more hosts
(the error shows the exact `dlerror()`). Fix that host's environment — see
[OpenMPI Environment Setup](#openmpi-environment-setup).

### `Couldn't connect to <host>:<port>`

A client rank tried to connect before the server was listening.  This typically
means MPI barriers aren't working on the client's host (caused by the
`libmpi.so` issue above).

### RDMA CM connects over the wrong network, or fails to resolve the address

Check that each entry's `peerAddress` is the correct RDMA-capable interface IP
for that host (not a management-network address). See
[Peer Connect Address](#peer-connect-address-peeraddress).

### `PMIX ERROR: PMIX_ERROR in file client/pmix_client_topology.c`

Non-fatal warning from PMIx about being unable to query hardware topology.  Does
not affect correctness.  Suppress with `--mca pmix_client_verbosity 0`.

#!/usr/bin/env python3
"""CLI entry point: python3 -m cluster_mode [options]"""

import argparse
import json
import sys
import traceback
from .orchestrator import Orchestrator
from .config_model import (
    ClusterConfig, TYPED_PERFTEST_FIELDS, format_json_config_help,
    typed_field_option_strings,
)
from .json_report import resolve_json_output_paths


def build_run_configs(args):
    """Build validated single-run configs from CLI/file args."""
    return [cfg.to_dict() for cfg in ClusterConfig.from_args(args).expand_tests()]


def _format_test_banner(index: int, total: int, config: dict) -> str:
    name = config.get('name') or config.get('perftestBinary', 'test')
    iteration = config.get('iteration', 1)
    iterations = config.get('iterations', 1)
    suffix = f" (iteration {iteration}/{iterations})" if iterations > 1 else ""
    return f"[Cluster] Test {index}/{total}: {name}{suffix}"


def run_configs(args, configs) -> int:
    """Run expanded configs sequentially; stop on first failure."""
    if args.output_format == 'json' and len(configs) > 1:
        raise ValueError(
            "--output-format json with multiple tests is not supported yet; "
            "run tests individually or use table output.")

    json_output_paths = resolve_json_output_paths(configs)

    total = len(configs)
    for index, (config, json_output_path) in enumerate(
            zip(configs, json_output_paths), start=1):
        if total > 1:
            print(_format_test_banner(index, total, config))
        orch = Orchestrator()
        orch.config = config
        rc = orch.run(dry_run=args.dry_run, verbose=args.verbose,
                      output_format=args.output_format,
                      json_output_path=json_output_path)
        if rc != 0:
            return rc
    return 0


def build_parser() -> argparse.ArgumentParser:
    """Build the cluster-mode CLI argument parser."""
    parser = argparse.ArgumentParser(
        description='Perftest Cluster Mode Orchestrator',
    )

    parser.add_argument('-f', '--file',
                        help='JSON configuration file')

    parser.add_argument('--hosts',
                        help='Comma-separated hostnames (supports patterns like node-[01-04])')
    parser.add_argument('--pattern',
                        choices=['O2O', 'O2M', 'M2O', 'A2A', 'B', 'R'],
                        default='O2O',
                        help='Traffic pattern')
    parser.add_argument('--binary',
                        default='ib_write_bw',
                        help='Perftest binary')
    parser.add_argument('--port', type=int, default=18515,
                        help='Base port number (orchestrator manages per-rank ports)')
    parser.add_argument('--streams', type=int, default=1,
                        help='Number of parallel streams per connection (default: 1)')
    parser.add_argument('--perftest-args', default='',
                        help='Arguments passed to perftest binary '
                             '(e.g. "-s 65536 -D 10 -b -q 4 --report_gbits")')
    parser.add_argument('--mpi-subnet', default=None,
                        help='Subnet for MPI btl_tcp_if_include (e.g. 10.220.95.0/24)')

    # Typed flags from TYPED_PERFTEST_FIELDS; default=None so unset != JSON override.
    typed_group = parser.add_argument_group(
        'perftest parameters (override JSON fields)')
    for field in TYPED_PERFTEST_FIELDS:
        opts = typed_field_option_strings(field)
        if field.kind == 'bool':
            typed_group.add_argument(*opts, dest=field.name,
                                     action='store_true', default=None,
                                     help=field.description)
        else:
            typed_group.add_argument(*opts, dest=field.name,
                                     type=int if field.kind == 'int' else str,
                                     default=None, metavar=field.name.upper(),
                                     help=field.description)

    parser.add_argument('--help-json', action='store_true',
                        help='Show supported JSON configuration fields and exit')

    parser.add_argument('--dry-run', action='store_true',
                        help='Print mpirun command without executing')
    parser.add_argument('--verbose', action='store_true',
                        help='Show per-stream detail in results')
    parser.add_argument('--output-format', choices=['table', 'json'],
                        default='table',
                        help='Output format: table (human-readable) or json (machine-parseable)')

    json_output_group = parser.add_mutually_exclusive_group()
    json_output_group.add_argument(
        '--json-output', dest='json_output', default=None, metavar='FILE',
        help='Save the full JSON result report to FILE, independent of '
             '--output-format (table output still goes to stdout/stderr).')
    json_output_group.add_argument(
        '--json-output-dir', dest='json_output_dir', default=None, metavar='DIR',
        help='Save one generated JSON result report per executed test into DIR.')

    return parser


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.help_json:
        print(format_json_config_help())
        return 0

    try:
        return run_configs(args, build_run_configs(args))

    except FileNotFoundError as e:
        print(f"\nERROR: Config file not found: {e.filename}", file=sys.stderr)
        return 1
    except json.JSONDecodeError as e:
        print(f"\nERROR: Invalid JSON in config file "
              f"(line {e.lineno}, col {e.colno}): {e.msg}", file=sys.stderr)
        return 1
    except ValueError as e:
        print(f"\nERROR: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nAborted.", file=sys.stderr)
        return 130
    except Exception as e:
        print(f"\nERROR: {type(e).__name__}: {e}", file=sys.stderr)
        if getattr(args, 'verbose', False):
            traceback.print_exc()
        return 1


if __name__ == '__main__':
    sys.exit(main())

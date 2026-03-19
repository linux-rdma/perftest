"""Persistent JSON report path resolution for cluster mode."""

import os
import re
from datetime import datetime
from typing import Dict, List, Optional

_SLUG_RE = re.compile(r'[^A-Za-z0-9]+')


def _slugify(name: str) -> str:
    slug = _SLUG_RE.sub('-', name).strip('-').lower()
    return slug or 'test'


def _generate_json_output_filename(index: int, total: int, config: dict,
                                   timestamp: str) -> str:
    """Filename for jsonOutputDir: {index}-{slug}[-iteration-N]-{timestamp}.json"""
    name = config.get('name') or config.get('perftestBinary', 'test')
    parts = [f"{index:03d}", _slugify(name)]
    if config.get('iterations', 1) > 1:
        parts.append(f"iteration-{config.get('iteration', 1)}")
    parts.append(timestamp)
    return '-'.join(parts) + '.json'


def resolve_json_output_paths(configs: List[dict]) -> List[Optional[str]]:
    """Resolve per-config JSON report paths; reject duplicate jsonOutputFile."""
    total = len(configs)
    timestamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    paths: List[Optional[str]] = []
    seen_files: Dict[str, int] = {}
    for index, config in enumerate(configs, start=1):
        json_dir = config.get('jsonOutputDir')
        json_file = config.get('jsonOutputFile')
        if json_dir is not None and not isinstance(json_dir, str):
            raise ValueError(f"jsonOutputDir must be a string, got {json_dir!r}")
        if json_file is not None and not isinstance(json_file, str):
            raise ValueError(f"jsonOutputFile must be a string, got {json_file!r}")
        if json_dir:
            filename = _generate_json_output_filename(
                index, total, config, timestamp)
            paths.append(os.path.join(json_dir, filename))
        elif json_file:
            norm = os.path.abspath(json_file)
            if norm in seen_files:
                raise ValueError(
                    f"jsonOutputFile {json_file!r} is used by both test "
                    f"{seen_files[norm]} and test {index}; give each test a "
                    f"distinct jsonOutputFile, or use jsonOutputDir instead.")
            seen_files[norm] = index
            paths.append(json_file)
        else:
            paths.append(None)
    return paths

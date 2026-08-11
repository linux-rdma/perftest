"""Auto-detect OpenMPI for cluster mode (same algorithm as set_mpi).

Detection only — callers apply the result. Override: DOCA_OPENMPI_ROOT_GLOB
(default /usr/mpi/gcc).
"""

from __future__ import annotations

import ctypes
import glob
import os
import re
import shutil
from functools import lru_cache
from typing import Dict, List, Optional


@lru_cache(maxsize=1)
def libnuma_available() -> bool:
    """True if libnuma is available on this host."""
    try:
        ctypes.CDLL("libnuma.so")
        return True
    except OSError:
        return False

_COMMON_ROOT_GLOBS = [
    '/usr/lib64/openmpi',
    '/usr/lib/openmpi',
    '/usr/lib/*/openmpi',
    '/opt/openmpi*',
    '/usr/local/openmpi*',
]

_BOUNDED_SEARCH_ROOTS = ('/usr', '/opt', '/usr/local')
_BOUNDED_SEARCH_MAX_DEPTH = 4


def _natural_sort_key(name: str) -> list:
    """Version-sort key (stdlib substitute for `sort -V`): splits into
    digit/non-digit chunks so 'openmpi-5.0.10' sorts after 'openmpi-4.1.6'.
    Each chunk is tagged (0, int) or (1, str) so chunks at the same position
    stay mutually comparable even with a differently-shaped suffix (e.g. an
    'rc2' tag), which would otherwise mix str/int and raise TypeError."""
    return [(0, int(chunk)) if chunk.isdigit() else (1, chunk)
            for chunk in re.split(r'(\d+)', name)]


def _is_executable(path: str) -> bool:
    return os.path.isfile(path) and os.access(path, os.X_OK)


def _find_doca_root() -> Optional[str]:
    """Newest openmpi-* under ${DOCA_OPENMPI_ROOT_GLOB:-/usr/mpi/gcc} that
    has a working bin/mpirun."""
    root_glob = os.environ.get('DOCA_OPENMPI_ROOT_GLOB', '/usr/mpi/gcc')
    if not os.path.isdir(root_glob):
        return None

    candidates = [d for d in glob.glob(os.path.join(root_glob, 'openmpi-*'))
                 if os.path.isdir(d)]
    candidates.sort(key=lambda d: _natural_sort_key(os.path.basename(d)),
                    reverse=True)

    for candidate in candidates:
        if _is_executable(os.path.join(candidate, 'bin', 'mpirun')):
            return candidate
    return None


def _root_from_mpirun(mpirun_path: str) -> Optional[str]:
    """Derive an OpenMPI root from a resolved mpirun path (root/bin/mpirun)."""
    resolved = os.path.realpath(mpirun_path)
    root = os.path.dirname(os.path.dirname(resolved))
    if _is_executable(os.path.join(root, 'bin', 'mpirun')):
        return root
    return None


def _find_common_root() -> Optional[str]:
    for pattern in _COMMON_ROOT_GLOBS:
        for candidate in sorted(glob.glob(pattern)):
            if _is_executable(os.path.join(candidate, 'bin', 'mpirun')):
                return candidate
    return None


def _find_bounded_root() -> Optional[str]:
    """Bounded filesystem search under /usr, /opt, /usr/local (depth <= 4)
    for an executable file named mpirun."""
    for base in _BOUNDED_SEARCH_ROOTS:
        if not os.path.isdir(base):
            continue
        base = os.path.normpath(base)
        for dirpath, dirnames, filenames in os.walk(base):
            rel = os.path.relpath(dirpath, base)
            depth = 0 if rel == '.' else rel.count(os.sep) + 1
            if depth >= _BOUNDED_SEARCH_MAX_DEPTH:
                dirnames[:] = []
                continue
            if 'mpirun' in filenames:
                candidate = os.path.join(dirpath, 'mpirun')
                if _is_executable(candidate):
                    root = _root_from_mpirun(candidate)
                    if root:
                        return root
    return None


def _find_fallback_root() -> Optional[str]:
    """mpirun already on PATH, else common install roots, else a bounded
    filesystem search."""
    mpirun_path = shutil.which('mpirun')
    if mpirun_path:
        root = _root_from_mpirun(mpirun_path)
        if root:
            return root

    root = _find_common_root()
    if root:
        return root

    return _find_bounded_root()


def detect_openmpi_root() -> Optional[str]:
    """Detect a usable OpenMPI install root: DOCA-style first, then
    generic fallbacks. Returns None if nothing usable was found."""
    return _find_doca_root() or _find_fallback_root()


def lib_dirs(root: str) -> List[str]:
    """Existing lib64/lib directories under an OpenMPI root, lib64 first."""
    dirs = []
    for name in ('lib64', 'lib'):
        candidate = os.path.join(root, name)
        if os.path.isdir(candidate):
            dirs.append(candidate)
    return dirs


def _prepend_unique(value: str, new_entry: str) -> str:
    """Prepend new_entry to a colon-separated PATH-like string, unless
    already present."""
    parts = value.split(':') if value else []
    if new_entry in parts:
        return value
    return new_entry + (':' + value if value else '')


def build_env(root: str,
             base_env: Optional[Dict[str, str]] = None) -> Dict[str, str]:
    """Return a copy of base_env (default os.environ) with <root>/bin
    prepended to PATH and <root>/lib[64] prepended to LD_LIBRARY_PATH."""
    env = dict(base_env if base_env is not None else os.environ)

    bin_dir = os.path.join(root, 'bin')
    env['PATH'] = _prepend_unique(env.get('PATH', ''), bin_dir)

    ld_library_path = env.get('LD_LIBRARY_PATH', '')
    for directory in lib_dirs(root):
        ld_library_path = _prepend_unique(ld_library_path, directory)
    if ld_library_path:
        env['LD_LIBRARY_PATH'] = ld_library_path

    return env

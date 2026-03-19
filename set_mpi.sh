#!/bin/bash
#
# set_mpi.sh - Detect and expose a usable OpenMPI installation.
#
# Finds a working `mpirun`, preferring DOCA-style installs under
# /usr/mpi/gcc/openmpi-* (the layout DOCA's OpenMPI package uses), and
# falling back to any other OpenMPI already present on the system
# (RPM- or DEB-based). Exposes MPI_DIR / MPI_HOME / OMPI_HOME / PATH /
# LD_LIBRARY_PATH, either to the current interactive shell or
# persistently for every future login shell via /etc/profile.d.
#
# USAGE
#   Apply to the current shell (pick one):
#     source set_mpi.sh
#     eval "$(set_mpi.sh --emit-exports)"
#
#   Inspect what would be selected, without changing anything:
#     set_mpi.sh --show
#
#   Apply and verify mpirun actually runs:
#     set_mpi.sh --verify
#
#   Install system-wide (persists across logins, requires root):
#     sudo set_mpi.sh --install-system-wide
#
# SAFETY
#   This script is meant to be sourced, so it never calls `exit` while
#   sourced (it uses `return` at the top level instead) and never enables
#   `set -e`/`set -u`/etc. - either would change the behavior of whatever
#   interactive shell sources it. It is also safe to source/run multiple
#   times: PATH and LD_LIBRARY_PATH entries are only ever prepended once.
#
# ENVIRONMENT OVERRIDES (mainly for testing; defaults match production)
#   DOCA_OPENMPI_ROOT_GLOB   Parent dir to scan for openmpi-* (default: /usr/mpi/gcc)
#   DOCA_OPENMPI_HELPER_DIR  Where --install-system-wide copies this script
#                            (default: /usr/local/lib/doca-openmpi)
#   DOCA_OPENMPI_PROFILE_D   Where --install-system-wide writes the profile
#                            script (default: /etc/profile.d)
#

# ── Sourced vs. executed ─────────────────────────────────────────────────────
# Determined up front: every exit path below needs to know whether `exit`
# is safe (executed as a subprocess) or would kill the caller's shell
# (sourced), and needs the script's own absolute path either way.

if [ -n "${BASH_SOURCE:-}" ] && [ "${BASH_SOURCE[0]}" != "${0}" ]; then
    _SET_MPI_SOURCED=1
else
    _SET_MPI_SOURCED=0
fi

_SET_MPI_SELF="$(readlink -f "${BASH_SOURCE[0]:-$0}" 2>/dev/null)"
[ -n "${_SET_MPI_SELF}" ] || _SET_MPI_SELF="${BASH_SOURCE[0]:-$0}"

: "${DOCA_OPENMPI_ROOT_GLOB:=/usr/mpi/gcc}"
: "${DOCA_OPENMPI_HELPER_DIR:=/usr/local/lib/doca-openmpi}"
: "${DOCA_OPENMPI_PROFILE_D:=/etc/profile.d}"

# ── PATH / LD_LIBRARY_PATH helpers ───────────────────────────────────────────

# $1 = dir, $2 = current value of a colon-separated PATH-like variable.
_set_mpi_path_has() {
    case ":$2:" in
        *":$1:"*) return 0 ;;
        *) return 1 ;;
    esac
}

_set_mpi_prepend_path() {
    local dir="$1"
    _set_mpi_path_has "$dir" "$PATH" || PATH="$dir:$PATH"
}

# Only prepends dirs that exist, so callers can pass both lib and lib64
# unconditionally and get whichever ones are actually present.
_set_mpi_prepend_ld_library_path() {
    local dir="$1"
    [ -d "$dir" ] || return 0
    if ! _set_mpi_path_has "$dir" "${LD_LIBRARY_PATH:-}"; then
        if [ -z "${LD_LIBRARY_PATH:-}" ]; then
            LD_LIBRARY_PATH="$dir"
        else
            LD_LIBRARY_PATH="$dir:$LD_LIBRARY_PATH"
        fi
    fi
}

# Lists existing lib dir(s) under an MPI root, lib64 first. Used only for
# informational output (--show); the actual export logic in
# _set_mpi_apply() checks existence itself and does not depend on this.
_set_mpi_lib_dirs() {
    local root="$1"
    [ -d "${root}/lib64" ] && printf '%s\n' "${root}/lib64"
    [ -d "${root}/lib" ] && printf '%s\n' "${root}/lib"
}

# ── Detection ─────────────────────────────────────────────────────────────

# Preferred: DOCA-style install under $DOCA_OPENMPI_ROOT_GLOB/openmpi-*.
# Picks the newest version directory (version-sort) that actually has a
# working bin/mpirun.
_set_mpi_find_doca() {
    local chosen
    [ -d "${DOCA_OPENMPI_ROOT_GLOB}" ] || return 1
    chosen=$(find "${DOCA_OPENMPI_ROOT_GLOB}" -maxdepth 1 -mindepth 1 \
                  -type d -name 'openmpi-*' 2>/dev/null | sort -V | tail -n 1)
    [ -n "${chosen}" ] || return 1
    [ -x "${chosen}/bin/mpirun" ] || return 1
    printf '%s\n' "${chosen}"
}

# Derives an OpenMPI root from a resolved mpirun path (root/bin/mpirun)
# and validates it.
_set_mpi_root_from_mpirun() {
    local mpirun_path="$1" resolved root
    resolved=$(readlink -f "${mpirun_path}" 2>/dev/null)
    [ -n "${resolved}" ] || resolved="${mpirun_path}"
    root=$(dirname "$(dirname "${resolved}")")
    [ -x "${root}/bin/mpirun" ] || return 1
    printf '%s\n' "${root}"
}

# Fallback when no DOCA-style install is found: whatever OpenMPI is
# already usable on this system, RPM- or DEB-based. Tries, in order:
#   1. mpirun already on PATH (respects the user/system's own setup)
#   2. a curated list of common vendor/distro install roots
#   3. a bounded filesystem search (fast; no full-disk scan)
_set_mpi_find_fallback() {
    local mpirun_path candidate root

    if mpirun_path=$(command -v mpirun 2>/dev/null) && [ -n "${mpirun_path}" ]; then
        if root=$(_set_mpi_root_from_mpirun "${mpirun_path}"); then
            printf '%s\n' "${root}"
            return 0
        fi
    fi

    # Debian/Ubuntu multiarch and other common layouts expand via glob;
    # an unmatched pattern is simply skipped by the existence check below.
    local common_roots=(
        /usr/lib64/openmpi
        /usr/lib/openmpi
        /usr/lib/*/openmpi
        /opt/openmpi*
        /usr/local/openmpi*
    )
    for candidate in "${common_roots[@]}"; do
        [ -x "${candidate}/bin/mpirun" ] || continue
        printf '%s\n' "${candidate}"
        return 0
    done

    candidate=$(find /usr /opt /usr/local -maxdepth 4 -type f -name mpirun \
                     -perm -u+x 2>/dev/null | head -n 1)
    if [ -n "${candidate}" ] && root=$(_set_mpi_root_from_mpirun "${candidate}"); then
        printf '%s\n' "${root}"
        return 0
    fi

    return 1
}

# Prints the selected MPI root on stdout (exactly one line) on success;
# returns 1 with no stdout output on failure.
_set_mpi_detect() {
    local root
    if root=$(_set_mpi_find_doca) && [ -n "${root}" ]; then
        printf '%s\n' "${root}"
        return 0
    fi
    if root=$(_set_mpi_find_fallback) && [ -n "${root}" ]; then
        printf '%s\n' "${root}"
        return 0
    fi
    return 1
}

_set_mpi_print_not_found() {
    cat >&2 <<EOF
ERROR: No usable OpenMPI installation found.

Searched:
  - DOCA-style installs under: ${DOCA_OPENMPI_ROOT_GLOB}/openmpi-*
  - 'mpirun' on the current PATH
  - Common install roots (/usr/lib64/openmpi, /usr/lib/openmpi,
    /usr/lib/*/openmpi, /opt/openmpi*, /usr/local/openmpi*)
  - A bounded search under /usr, /opt, /usr/local

Install OpenMPI and try again, e.g.:
  RPM-based (RHEL/CentOS/Fedora) : sudo dnf install -y openmpi openmpi-devel
                                    (or 'yum' on older releases)
  DEB-based (Debian/Ubuntu)      : sudo apt install -y openmpi-bin libopenmpi-dev

Or install the DOCA OpenMPI package so it lands under
${DOCA_OPENMPI_ROOT_GLOB}/openmpi-<version>/.
EOF
}

# Runs detection and, on success, exports MPI_DIR/MPI_HOME/OMPI_HOME/
# PATH/LD_LIBRARY_PATH into the CURRENT shell (no subshell - callers that
# want an isolated view must wrap this call in "( ... )" themselves).
_set_mpi_apply() {
    local root
    root=$(_set_mpi_detect) || return 1

    MPI_DIR="${root}"
    MPI_HOME="${root}"
    OMPI_HOME="${root}"
    export MPI_DIR MPI_HOME OMPI_HOME

    _set_mpi_prepend_path "${root}/bin"
    export PATH

    _set_mpi_prepend_ld_library_path "${root}/lib"
    _set_mpi_prepend_ld_library_path "${root}/lib64"
    export LD_LIBRARY_PATH

    return 0
}

# ── CLI actions ───────────────────────────────────────────────────────────

cmd_show() {
    local root libdirs
    if ! root=$(_set_mpi_detect); then
        _set_mpi_print_not_found
        return 1
    fi

    echo "OpenMPI installation selected:"
    echo "  MPI_DIR : ${root}"
    echo "  mpirun  : ${root}/bin/mpirun"

    libdirs=$(_set_mpi_lib_dirs "${root}")
    if [ -n "${libdirs}" ]; then
        echo "  lib dir(s):"
        while IFS= read -r d; do echo "    - ${d}"; done <<< "${libdirs}"
    else
        echo "  lib dir(s): (none found under ${root})"
    fi
    return 0
}

# Prints only 'export ...' lines on stdout, so this is safe to use as
# eval "$(set_mpi.sh --emit-exports)". Runs in a subshell: detection
# failure prints its diagnostic to stderr and produces no stdout output
# at all, so a failed eval is simply a no-op rather than evaluating an
# error message as a shell command.
cmd_emit_exports() {
    (
        if ! _set_mpi_apply; then
            _set_mpi_print_not_found
            exit 1
        fi
        printf 'export MPI_DIR=%q\n' "${MPI_DIR}"
        printf 'export MPI_HOME=%q\n' "${MPI_HOME}"
        printf 'export OMPI_HOME=%q\n' "${OMPI_HOME}"
        printf 'export PATH=%q\n' "${PATH}"
        printf 'export LD_LIBRARY_PATH=%q\n' "${LD_LIBRARY_PATH}"
    )
}

# Applies detection results to the CURRENT shell. Only meaningful when
# this script is sourced; when executed directly, the exports would only
# affect this subprocess, so a hint is printed instead of pretending it
# worked.
cmd_apply() {
    if ! _set_mpi_apply; then
        _set_mpi_print_not_found
        return 1
    fi

    if [ "${_SET_MPI_SOURCED}" -eq 1 ]; then
        echo "[set_mpi] Using OpenMPI at: ${MPI_DIR}" >&2
    else
        echo "[set_mpi] Detected OpenMPI at: ${MPI_DIR}" >&2
        echo "[set_mpi] NOTE: run directly (not sourced), these exports only" >&2
        echo "[set_mpi] apply to this subprocess. Use one of instead:" >&2
        echo "[set_mpi]   source ${_SET_MPI_SELF}" >&2
        echo "[set_mpi]   eval \"\$(${_SET_MPI_SELF} --emit-exports)\"" >&2
    fi
    return 0
}

# Applies the environment (in a subshell, so nothing leaks out of
# --verify itself) and confirms mpirun actually resolves and runs.
cmd_verify() {
    (
        if ! _set_mpi_apply; then
            _set_mpi_print_not_found
            exit 1
        fi
        echo "MPI_DIR: ${MPI_DIR}"
        printf 'which mpirun -> '
        if ! command -v mpirun; then
            echo "ERROR: mpirun not found on PATH after applying environment" >&2
            exit 1
        fi
        if mpirun --version; then
            echo "[set_mpi] OK: mpirun runs successfully."
        else
            echo "[set_mpi] ERROR: mpirun found but failed to run (see output above)." >&2
            exit 1
        fi
    )
}

# Installs a copy of this script plus /etc/profile.d/doca-openmpi.sh so
# every future login shell gets the environment automatically. Requires
# root. Safe to re-run: overwrites only the two files it owns.
cmd_install_system_wide() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "ERROR: --install-system-wide requires root (try: sudo ${_SET_MPI_SELF} --install-system-wide)" >&2
        return 1
    fi

    local helper_path="${DOCA_OPENMPI_HELPER_DIR}/set_mpi.sh"
    local profile_path="${DOCA_OPENMPI_PROFILE_D}/doca-openmpi.sh"

    mkdir -p "${DOCA_OPENMPI_HELPER_DIR}" || {
        echo "ERROR: could not create ${DOCA_OPENMPI_HELPER_DIR}" >&2; return 1; }
    cp -f "${_SET_MPI_SELF}" "${helper_path}" || {
        echo "ERROR: could not copy script to ${helper_path}" >&2; return 1; }
    chmod 0755 "${helper_path}"

    mkdir -p "${DOCA_OPENMPI_PROFILE_D}" || {
        echo "ERROR: could not create ${DOCA_OPENMPI_PROFILE_D}" >&2; return 1; }

    # Deliberately minimal and defensive: this runs on every login shell,
    # so a missing helper or missing OpenMPI must be a silent no-op, not
    # a login error.
    cat > "${profile_path}" <<EOF
# Installed by set_mpi.sh --install-system-wide.
# Safe to source multiple times; does nothing if no OpenMPI is found.
_doca_openmpi_helper="${helper_path}"
if [ -r "\${_doca_openmpi_helper}" ]; then
    eval "\$("\${_doca_openmpi_helper}" --emit-exports 2>/dev/null)"
fi
unset -v _doca_openmpi_helper
EOF
    chmod 0644 "${profile_path}"

    echo "[set_mpi] Installed helper : ${helper_path}"
    echo "[set_mpi] Installed profile: ${profile_path}"
    echo "[set_mpi] New login shells will pick this up automatically."
    echo "[set_mpi] Verify now with:   ${_SET_MPI_SELF} --verify"
    return 0
}

usage() {
    local name
    name="$(basename "${_SET_MPI_SELF}")"
    cat <<EOF
Usage: ${name} [OPTION]

Detect a usable OpenMPI installation (preferring DOCA-style installs under
\${DOCA_OPENMPI_ROOT_GLOB:-/usr/mpi/gcc}/openmpi-*, falling back to any other
OpenMPI already on the system) and expose it via MPI_DIR / MPI_HOME /
OMPI_HOME / PATH / LD_LIBRARY_PATH.

CURRENT SHELL
  source ${name}                       Apply directly to this shell
  eval "\$(${name} --emit-exports)"      Same, via eval

OPTIONS
  --show                 Show what would be selected; exports nothing
  --emit-exports         Print 'export ...' lines for eval (empty output on failure)
  --apply                Apply exports to the current shell (only useful when sourced)
  --verify               Apply, then run 'which mpirun' and 'mpirun --version'
  --install-system-wide  Install ${DOCA_OPENMPI_PROFILE_D}/doca-openmpi.sh (requires root)
  -h, --help             Show this help

EXAMPLES
  # RPM-based host with DOCA OpenMPI under /usr/mpi/gcc/openmpi-5.x
  source ${name} && mpirun --version

  # DEB-based host, no DOCA OpenMPI: falls back to the system package
  eval "\$(${name} --emit-exports)"

  # Persist for every future login shell (RPM or DEB, same command)
  sudo ${name} --install-system-wide
EOF
}

# ── Dispatch ──────────────────────────────────────────────────────────────

_set_mpi_main() {
    case "${1:-}" in
        --show) cmd_show ;;
        --emit-exports) cmd_emit_exports ;;
        --apply) cmd_apply ;;
        --verify) cmd_verify ;;
        --install-system-wide) cmd_install_system_wide ;;
        -h|--help) usage ;;
        "")
            if [ "${_SET_MPI_SOURCED}" -eq 1 ]; then
                cmd_apply
            else
                usage
            fi
            ;;
        *)
            echo "ERROR: unknown option: ${1}" >&2
            usage >&2
            return 1
            ;;
    esac
}

_set_mpi_main "$@"
_SET_MPI_RC=$?

if [ "${_SET_MPI_SOURCED}" -eq 1 ]; then
    return "${_SET_MPI_RC}"
else
    exit "${_SET_MPI_RC}"
fi

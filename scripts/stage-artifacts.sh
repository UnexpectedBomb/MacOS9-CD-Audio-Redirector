#!/bin/bash
#
# Copy built artifacts somewhere the OS 9 machine can reach them.
#
# This is a developer convenience and is NOT needed to build. If it is not
# configured it does nothing and the build carries on.
#
# ── CONFIGURATION ────────────────────────────────────────────────────────────
# Nothing about any particular machine lives in this file. Set either:
#
#   CD_STAGE_HOST   an scp destination, e.g. someone@host
#   CD_STAGE_DIR    the directory on that host, e.g. /home/someone/shared
#
# or, for a local path (a mounted share, a folder, anything):
#
#   CD_STAGE_DIR    on its own, with CD_STAGE_HOST unset
#
# in the environment, or in a file called `.stage-target` at the top of the
# working tree, which is untracked:
#
#   CD_STAGE_HOST=me@mymac.local
#   CD_STAGE_DIR=/Users/me/Shared/os9
#
# ── WHY IT TAKES THE ARTIFACT NAME AS AN ARGUMENT ────────────────────────────
# It used to hardcode one, e.g. BASES="CDPump_v3". When the target was bumped to
# v4 the script silently re-copied the stale v3 still sitting in the build
# directory, and printed a success line while doing it. Version-stamping an
# artifact only helps if the thing that copies it knows the stamp too, so the
# name comes from CMake and a copy that moves nothing is an error.
#
# Usage: stage-artifacts.sh <build-dir> <artifact-name> [more names...]

set -u

BUILD_DIR="${1:-}"
shift || true
BASES="$*"

[ -n "${BUILD_DIR}" ] || { echo "stage-artifacts: no build directory given" >&2; exit 1; }
[ -n "${BASES}" ]     || { echo "stage-artifacts: no artifact name given, refusing to guess" >&2; exit 1; }

# Optional untracked config at the top of the working tree.
ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel 2>/dev/null || true)"
if [ -n "${ROOT}" ] && [ -f "${ROOT}/.stage-target" ]; then
    # shellcheck disable=SC1090
    . "${ROOT}/.stage-target"
fi

: "${CD_STAGE_HOST:=}"
: "${CD_STAGE_DIR:=}"

if [ -z "${CD_STAGE_DIR}" ]; then
    echo "stage-artifacts: not configured (set CD_STAGE_DIR, or create .stage-target); skipping"
    exit 0
fi

copy_one() {
    src="$1"; dest_name="$2"
    if [ -n "${CD_STAGE_HOST}" ]; then
        scp -q "${src}" "${CD_STAGE_HOST}:${CD_STAGE_DIR}/${dest_name}"
    else
        cp "${src}" "${CD_STAGE_DIR}/${dest_name}"
    fi
}

# A remote destination that is unreachable is not an error worth failing a build
# over: the local build is still perfectly good.
if [ -n "${CD_STAGE_HOST}" ]; then
    if ! ssh -o ConnectTimeout=5 -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
            "${CD_STAGE_HOST}" true 2>/dev/null; then
        echo "stage-artifacts: ${CD_STAGE_HOST} unreachable, skipping (local build unaffected)"
        exit 0
    fi
fi

staged=0
for base in ${BASES}; do
    for ext in bin img; do
        f="${BUILD_DIR}/${base}.${ext}"
        [ -f "${f}" ] || continue
        copy_one "${f}" "${base}.${ext}" || { echo "stage-artifacts: copy of ${base}.${ext} failed" >&2; exit 1; }
        staged=$((staged + 1))
    done
done

# Say nothing succeeded when nothing did. A cheerful line over an empty copy is
# how a stale artifact gets tested for a whole cycle.
if [ "${staged}" -eq 0 ]; then
    echo "stage-artifacts: NOTHING STAGED - no ${BASES} .bin/.img in ${BUILD_DIR}" >&2
    exit 1
fi
echo "stage-artifacts: ${BASES} (${staged} file(s)) -> ${CD_STAGE_DIR}"

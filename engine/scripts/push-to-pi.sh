#!/bin/bash
# Stage CDEngineInstall on the Pi's share (/home/csell/shared/) so the OS 9 machine can
# grab it over Netatalk AFP. Direct scp with a plain name — no "Claude." prefix
# and no sudo staging. Non-fatal if the Pi is unreachable; the local build still
# succeeds.
#
# ⚠ The artifact base name is passed IN by CMake, never hardcoded here. It used to
# read BASES="CDPump_v3"; when the target became v4 this script silently re-pushed
# the stale v3 binary that was still sitting in the build directory, and reported
# success. Version-stamping the artifact only helps if the thing that copies it
# knows the stamp too.
set -u
BUILD_DIR="${1:-$(dirname "$0")/../build}"
BASES="${2:-}"
PI=claude@pi3.local
SHARE=/home/csell/shared

if [ -z "${BASES}" ]; then
    echo "push-to-pi: no artifact name given (arg 2), refusing to guess" >&2
    exit 1
fi

if ! ssh -o ConnectTimeout=5 -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
        "${PI}" true 2>/dev/null; then
    echo "push-to-pi: pi3.local unreachable, skipping (local build unaffected)"
    exit 0
fi

pushed=0
for base in ${BASES}; do
    bin="${BUILD_DIR}/${base}.bin"
    img="${BUILD_DIR}/${base}.img"
    [ -f "$bin" ] && { scp -q "$bin" "${PI}:${SHARE}/${base}.bin" || { echo "push-to-pi: scp ${base}.bin failed"; exit 0; }; pushed=$((pushed+1)); }
    [ -f "$img" ] && { scp -q "$img" "${PI}:${SHARE}/${base}.img" || { echo "push-to-pi: scp ${base}.img failed"; exit 0; }; pushed=$((pushed+1)); }
done

# Say nothing succeeded when nothing did: a "pushed OK" line over an empty copy is
# how a stale artifact gets tested for a whole cycle.
if [ "${pushed}" -eq 0 ]; then
    echo "push-to-pi: NOTHING PUSHED - no ${BASES}.bin/.img in ${BUILD_DIR}" >&2
    exit 1
fi
echo "push-to-pi: ${BASES} (${pushed} file(s)) in ${SHARE}/ on pi3.local"

#!/bin/bash
# Stage CDEngineInstall on the Pi's share (/home/csell/shared/) so the OS 9 machine can
# grab it over Netatalk AFP. Direct scp with a plain name — no "Claude." prefix
# and no sudo staging. Non-fatal if the Pi is unreachable; the local build still
# succeeds.
set -u
BUILD_DIR="${1:-$(dirname "$0")/../build}"
PI=claude@pi3.local
SHARE=/home/csell/shared
BASES="CDEngineInstall_v1"

if ! ssh -o ConnectTimeout=5 -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
        "${PI}" true 2>/dev/null; then
    echo "push-to-pi: pi3.local unreachable, skipping (local build unaffected)"
    exit 0
fi

for base in ${BASES}; do
    bin="${BUILD_DIR}/${base}.bin"
    img="${BUILD_DIR}/${base}.img"
    [ -f "$bin" ] && { scp -q "$bin" "${PI}:${SHARE}/${base}.bin" || { echo "push-to-pi: scp ${base}.bin failed"; exit 0; }; }
    [ -f "$img" ] && { scp -q "$img" "${PI}:${SHARE}/${base}.img" || { echo "push-to-pi: scp ${base}.img failed"; exit 0; }; }
done
echo "push-to-pi: CDEngineInstall in ${SHARE}/ on pi3.local"

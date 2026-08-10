#!/bin/bash
#
# Copy built artifacts into dist/ for publication, with build-machine identity
# scrubbed out of them, then verify that it worked.
#
# ── WHY THIS IS NEEDED ───────────────────────────────────────────────────────
# Retro68's newlib carries __FILE__ strings from its own sources, e.g.
#
#     /Users/<builduser>/Retro68/gcc/newlib/libc/stdlib/dtoa.c
#
# Those come from the PREBUILT libc that gets linked in, not from anything this
# project compiles, so -ffile-prefix-map cannot reach them. They end up inside
# every artifact that links the printf family. On a repository published under a
# handle, that puts the author's real account name into a downloadable binary.
#
# ── WHY A BYTE SUBSTITUTION IS SAFE HERE ─────────────────────────────────────
# The replacement is the SAME LENGTH as the text it replaces, so nothing moves:
# no offset, no length field and no structure changes. The bytes are inert string
# data inside libc error paths that this code never calls.
#
#   .bin  MacBinary II checksums the 128-byte HEADER only, not the data fork,
#         so a data-fork substitution leaves the header CRC valid.
#   .img  is an HFS volume in an Apple Partition Map wrapper. Neither checksums
#         file contents.
#
# It runs as a separate publish step rather than inside the build so that the
# binary tested on hardware and the binary shipped differ only in these bytes,
# deliberately and visibly, instead of silently.

set -eu

ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
cd "${ROOT}"

# The token to remove is the BUILD USER'S OWN ACCOUNT NAME, taken from the system
# rather than written down. Spelling it out here would have put the very string
# this script exists to remove into the published repository, which is exactly the
# leak, one level up. Override with CD_SCRUB_TOKEN if the toolchain was built by a
# different account than the one running this.
SCRUB_FROM="${CD_SCRUB_TOKEN:-$(id -un)}"

# The replacement must be EXACTLY as long, so nothing in the file moves. Pad or
# truncate a neutral word to match, whatever length the account name happens to be.
SCRUB_TO="$(printf '%s' "builduserbuilduserbuilduserbuilduser" | cut -c1-${#SCRUB_FROM})"

if [ -z "${SCRUB_FROM}" ] || [ ${#SCRUB_FROM} -lt 3 ]; then
    echo "publish-dist: implausible scrub token '${SCRUB_FROM}', refusing" >&2
    exit 1
fi
if [ ${#SCRUB_FROM} -ne ${#SCRUB_TO} ]; then
    echo "publish-dist: substitution changes length, refusing" >&2
    exit 1
fi

mkdir -p dist

publish() {
    src="$1"
    [ -f "${src}" ] || { echo "publish-dist: missing ${src}" >&2; exit 1; }
    out="dist/$(basename "${src}")"
    LC_ALL=C sed "s/${SCRUB_FROM}/${SCRUB_TO}/g" < "${src}" > "${out}"

    if [ "$(stat -f%z "${src}")" -ne "$(stat -f%z "${out}")" ]; then
        echo "publish-dist: ${out} changed size, refusing" >&2
        exit 1
    fi
    if LC_ALL=C grep -q "${SCRUB_FROM}" "${out}"; then
        echo "publish-dist: ${out} STILL contains '${SCRUB_FROM}'" >&2
        exit 1
    fi
    echo "  published $(basename "${out}")  ($(stat -f%z "${out}") bytes, scrubbed)"
}

publish engine/build/CDAudioRedirector_v9.bin
publish engine/build/CDAudioRedirector_v9.img
publish probes/cdplay/build/CDPlayProbe_v11.bin
publish probes/cdplay/build/CDPlayProbe_v11.img

echo "publish-dist: dist/ is clean"

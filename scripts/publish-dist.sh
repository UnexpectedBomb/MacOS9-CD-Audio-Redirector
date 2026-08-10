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
#
# ── WHY THE ARTIFACT NAMES ARE DERIVED, NOT WRITTEN DOWN ─────────────────────
# This script used to name the versions literally, e.g. CDAudioRedirector_v10.
# That is the same trap `stage-artifacts.sh` was rewritten to escape, and it is
# worse here because dist/ is what the public repository ships. CMake writes a
# NEW file on a version bump and leaves the previous one sitting in the build
# directory, so a hardcoded name still resolves: the stale binary would be found,
# scrubbed, verified, published, and reported as a success. That already happened
# once on this project with BASES="CDPump_v3".
#
# So the names come from the CMakeLists that produce them. A version bump is a
# single edit to `add_application(...)`, and this script follows it. If a prefix
# ever matches no target or several, it refuses rather than guessing.

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

# Read the current versioned target name out of a CMakeLists, given its unversioned
# prefix. Exactly one match is required: none means the prefix is wrong, several
# means the caller has to say which, and either way guessing is how a stale binary
# gets published.
artifact_name() {
    cmake_file="$1"; prefix="$2"
    [ -f "${cmake_file}" ] || { echo "publish-dist: no ${cmake_file}" >&2; exit 1; }

    names="$(sed -n 's/^[[:space:]]*add_application([[:space:]]*\([A-Za-z0-9_]*\).*/\1/p' \
             "${cmake_file}" | grep "^${prefix}_v[0-9]" || true)"
    count="$(printf '%s' "${names}" | grep -c . || true)"

    if [ "${count}" -ne 1 ]; then
        echo "publish-dist: ${cmake_file} has ${count} targets matching '${prefix}_v', need exactly 1" >&2
        [ "${count}" -gt 1 ] && printf '  found: %s\n' ${names} >&2
        exit 1
    fi
    printf '%s' "${names}"
}

# Older builds of the same artifact stay in the build directory after a version
# bump. Nothing can publish them now that the name is derived, but they are the
# ammunition this whole trap needs, so say they are there.
warn_stale() {
    build_dir="$1"; prefix="$2"; current="$3"
    for f in "${build_dir}/${prefix}"_v*.bin; do
        [ -f "${f}" ] || continue
        [ "$(basename "${f}")" = "${current}.bin" ] && continue
        echo "  NOTE stale $(basename "${f}") in ${build_dir} - not published, worth deleting"
    done
}

REDIRECTOR="$(artifact_name engine/CMakeLists.txt CDAudioRedirector)"
PROBE="$(artifact_name probes/cdplay/CMakeLists.txt CDPlayProbe)"

echo "publish-dist: publishing ${REDIRECTOR} and ${PROBE} (names taken from CMake)"

publish "engine/build/${REDIRECTOR}.bin"
publish "engine/build/${REDIRECTOR}.img"
publish "probes/cdplay/build/${PROBE}.bin"
publish "probes/cdplay/build/${PROBE}.img"

warn_stale engine/build CDAudioRedirector "${REDIRECTOR}"
warn_stale probes/cdplay/build CDPlayProbe "${PROBE}"

# dist/ keeps whatever was published last. A rename leaves the old pair behind,
# and two versions in a publication directory is how a downloader picks the wrong
# one, so name them.
for f in dist/*.bin; do
    [ -f "${f}" ] || continue
    b="$(basename "${f}" .bin)"
    [ "${b}" = "${REDIRECTOR}" ] && continue
    [ "${b}" = "${PROBE}" ] && continue
    echo "  NOTE dist/ still carries ${b} - superseded, delete before committing"
done

echo "publish-dist: dist/ is clean"

#!/bin/bash
# Verify the assembled 'SIZE' flag word of a built application.
#
# WHY THIS EXISTS: a Rez 'SIZE' resource is sixteen anonymous booleans in a fixed
# order. Name them in the wrong order and you set a different flag, with no warning
# from Rez and nothing visible until the app misbehaves on the OS 9 machine. The
# faceless build depends on exactly two of those bits, so the value is checked here
# rather than asserted in a comment.
#
# Usage: check-size-flags.sh <app.bin> <expected-hex>   e.g. ... CDAudio.bin 14C0
set -u

BIN="${1:?usage: check-size-flags.sh <app.bin> <expected-hex-flags>}"
WANT="${2:?usage: check-size-flags.sh <app.bin> <expected-hex-flags>}"

[ -f "$BIN" ] || { echo "check-size-flags: no such file: $BIN" >&2; exit 1; }

# The SIZE resource is 10 bytes: flags (2) + preferred (4) + minimum (4). Both memory
# sizes are known, so find that 8-byte tail in the resource fork and read the word in
# front of it. Cruder than parsing the resource map, and it cannot silently match the
# wrong thing: 00400000 00300000 is not a byte pattern that occurs by accident.
GOT=$(xxd -p "$BIN" | tr -d '\n' \
      | grep -oE '[0-9a-f]{4}0040000000300000' | head -1 | cut -c1-4)

if [ -z "${GOT}" ]; then
    echo "check-size-flags: could not locate a SIZE resource in ${BIN}" >&2
    echo "  (looked for the 4MB/3MB partition pair; adjust this script if those change)" >&2
    exit 1
fi

# Compare case-insensitively as hex numbers, not as strings.
if [ "$((0x${GOT}))" -ne "$((0x${WANT}))" ]; then
    echo "check-size-flags: FAIL - ${BIN} has SIZE flags 0x${GOT}, expected 0x${WANT}" >&2
    exit 1
fi
echo "check-size-flags: ${BIN} SIZE flags = 0x${GOT} as expected"

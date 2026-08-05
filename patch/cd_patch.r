/* cd_patch.r — packages the Phase-2a extension.
 *
 * Two flat 68K code resources in one file:
 *   'INIT' 128  the installer, run by the Start Manager during the extension parade
 *   'CDpt' 128  the resident patch blob, loaded by the INIT into the system heap
 *
 * Both are read by fixed filename, so the CMake targets' OUTPUT_NAME must match
 * exactly or Rez fails with "could not $$read file".
 */

#include "Retro68.r"
#include "Types.r"

type 'INIT' {
    RETRO68_CODE_TYPE
};

type 'CDpt' {
    RETRO68_CODE_TYPE
};

resource 'INIT' (128, locked) {
    dontBreakAtEntry, $$read("cd_patch_init.flt");
};

/* Locked and preloaded is deliberate: the INIT loads this into the system heap and
 * detaches it, and it must not move while the Control patch points into it. */
resource 'CDpt' (128, nonpurgeable, locked, preload) {
    dontBreakAtEntry, $$read("cd_patch_blob.flt");
};

resource 'vers' (1, "CDPatch2a") {
    0x00,
    0x10,                   /* 1.0 */
    development,
    0x01,
    verUS,
    "2a.1d1",
    "2a.1d1, CD Audio Redirector Control-entry patch (trace only, no audio)"
};

/* cd_patch_install.r — the on-demand installer app.
 *
 * Carries a copy of the SAME 'CDpt' 128 blob the extension does, so what gets
 * tested from the app is exactly what the INIT will install at boot.
 */

#include "Retro68APPL.r"
#include "Types.r"

type 'CDpt' {
    RETRO68_CODE_TYPE
};

resource 'CDpt' (128, nonpurgeable, locked, preload) {
    dontBreakAtEntry, $$read("cd_patch_blob.flt");
};

/* NoteAlert 128, used for the result. A plain alert: no DITL UserItems, which have
 * silently swallowed mouse clicks on this project before. */
resource 'ALRT' (128) {
    {60, 60, 190, 460}, 128, { OK, visible, silent, OK, visible, silent,
                               OK, visible, silent, OK, visible, silent },
    alertPositionMainScreen
};

resource 'DITL' (128) {
    {
        {96, 310, 116, 380}, Button { enabled, "OK" };
        {10, 70, 86, 380},   StaticText { disabled, "^0" };
        {10, 20, 42, 52},    Icon { disabled, 1 };
    }
};

resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground,
    multiFinderAware, backgroundAndForeground, dontGetFrontClicks,
    ignoreChildDiedEvents, is32BitCompatible, isHighLevelEventAware,
    onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    notDisplayManagerAware, reserved, reserved,
    512 * 1024, 384 * 1024
};

resource 'vers' (1, "CDPatchInstall") {
    0x00, 0x10, development, 0x01, verUS,
    "2a.1d1", "2a.1d1, installs the CD Control-entry patch post-boot"
};

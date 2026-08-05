/* cd_play_probe.r — resources for CDPlayProbe (Phase-0 probe P5a).
 *
 * The window and its two buttons are created programmatically in
 * cd_play_probe.c (NewWindow + NewControl), deliberately not from a DITL: this
 * project has already lost time to DITL UserItem placeholders that swallow mouse
 * clicks silently. NewControl push buttons have no such trap.
 */

#include "Processes.r"
#include "Types.r"

resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    multiFinderAware,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    isHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    notDisplayManagerAware,
    reserved,
    reserved,
    768 * 1024,    /* preferred */
    512 * 1024     /* minimum   */
};

resource 'vers' (1, "CDPlayProbe") {
    0x00,
    0x20,                   /* 2.0 */
    development,
    0x01,
    verUS,
    "2.0d1",
    "2.0d1, CD Audio Redirector legacy-audio API probe"
};

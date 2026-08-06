/* cd_recon.r — resources for CDPump, the Step-2 engine residency + validation app.
 *
 * The summary window is created programmatically in cd_recon.c (NewWindow), so
 * the only resources needed are the memory partition and a version stamp.
 */

#include "Processes.r"
#include "Types.r"

/* Small app: the Toolbox, one window, a 9 KB sector buffer and a 400-byte TOC
 * buffer. 768 KB preferred is generous headroom. */
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
    4096 * 1024,    /* preferred */
    3072 * 1024    /* minimum   */
};

resource 'vers' (1, "CDPump") {
    0x00,
    0x30,                   /* 3.0 */
    development,
    0x01,
    verUS,
    "4.0d1",
    "4.0d1, CD Audio Redirector Step-2 engine residency + validation"
};

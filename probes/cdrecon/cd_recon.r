/* cd_recon.r — resources for CDRecon, the Phase-0 recon app.
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
    768 * 1024,    /* preferred */
    512 * 1024     /* minimum   */
};

resource 'vers' (1, "CDRecon") {
    0x00,
    0x20,                   /* 2.0 */
    development,
    0x01,
    verUS,
    "2.0d1",
    "2.0d1, CD Audio Redirector Phase-0 recon"
};

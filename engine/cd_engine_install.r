/* cd_engine_install.r — resources for CDPump: driver patch, then the audio pump.
 *
 * The progress window is created programmatically (NewWindow), so the only
 * resources needed are the memory partition and a version stamp.
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
    0x90,                   /* 9.0 */
    development,
    0x01,
    verUS,
    "9.0d1",
    "9.0d1, CDPump - driver patch and audio pump, diagnostic build"
};

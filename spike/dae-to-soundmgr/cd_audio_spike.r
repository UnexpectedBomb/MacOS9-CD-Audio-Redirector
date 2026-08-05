/* cd_audio_spike.r — resources for the Phase-1 DAE → Sound Manager spike.
 *
 * Windows and buttons are built programmatically (NewWindow + NewControl), not
 * from a DITL: this project has already lost time to DITL UserItem placeholders
 * that swallow mouse clicks silently.
 */

#include "Processes.r"
#include "Types.r"

/* This one actually needs memory, unlike the Phase-0 probes:
 *   stage A audio buffer   5 s  = 882,000 bytes
 *   stage B ring           2 s  = 352,800 bytes  (not held at the same time)
 *   two double buffers     0.25 s each = 88,200 bytes
 * 4 MB preferred leaves generous room for the Toolbox and heap fragmentation. */
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
    4096 * 1024,   /* preferred */
    3072 * 1024    /* minimum   */
};

resource 'vers' (1, "CDAudioSpike") {
    0x00,
    0x10,                   /* 1.0 */
    development,
    0x01,
    verUS,
    "1.0d1",
    "1.0d1, CD Audio Redirector Phase-1 DAE to Sound Manager spike"
};

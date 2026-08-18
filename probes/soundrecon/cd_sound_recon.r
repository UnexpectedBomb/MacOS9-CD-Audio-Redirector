/* cd_sound_recon.r — resources for CDSoundRecon.
 *
 * The summary window is created programmatically, so all that is needed here is
 * the memory partition and a version stamp. Modelled on cd_recon.r.
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

resource 'vers' (1, "CDSoundRecon") {
    0x00,
    0x10,                   /* 1.0 */
    development,
    0x01,
    verUS,
    "1.0d1",
    "1.0d1, sound input recon for the CD Audio Redirector"
};

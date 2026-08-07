/* cd_engine_faceless.r — resources for the SHIPPING artifact: CD Audio Redirector.
 *
 * Same code as CDPump, built with CD_FACELESS=1. The only difference that lives
 * here is the SIZE resource: this build has no user interface at all, so it must
 * not appear in the Application menu and must never be switched to.
 *
 * ★ THE FLAG WORD MUST COME OUT AS 0x14C0. Each boolean below is one bit, high to
 * low, in the order 'SIZE' declares them in Processes.r — get the ORDER wrong and
 * you silently set a different flag, since every slot is just "boolean". Worked out
 * against that declaration:
 *
 *     bit 12  canBackground           0x1000   gets time while in the background
 *     bit 10  onlyBackground          0x0400   no user interface; stays off the menu
 *     bit  7  is32BitCompatible       0x0080
 *     bit  6  isHighLevelEventAware   0x0040   so the quit Apple event reaches us
 *                                     ------
 *                                     0x14C0
 *
 * Every other bit is deliberately the zero-valued name. The build verifies the
 * assembled bytes rather than trusting this comment — see scripts/check-size-flags.sh.
 *
 * ⚠ onlyBackground is what makes this invisible, and it is also what makes the quit
 * Apple event the ONLY way to stop it: there is no window to click and no menu to
 * quit from. Recovery from anything is a restart, as it has been all along.
 */

#include "Processes.r"
#include "Types.r"

/* The proven partition from the interactive build, unchanged on purpose. The pump
 * needs the 2-second ring (352800 bytes), two 44100-byte double buffers and the PEF;
 * the rest is headroom that has been exercised on hardware. Worth trimming only with
 * a measurement, not a guess. */
resource 'SIZE' (-1) {
    reserved,
    ignoreSuspendResumeEvents,      /* bit 14 = 0: nothing to suspend            */
    reserved,
    canBackground,                  /* bit 12 = 1                                */
    needsActivateOnFGSwitch,        /* bit 11 = 0: never comes to the foreground */
    onlyBackground,                 /* bit 10 = 1: faceless                      */
    dontGetFrontClicks,             /* bit  9 = 0                                */
    ignoreAppDiedEvents,            /* bit  8 = 0                                */
    is32BitCompatible,              /* bit  7 = 1                                */
    isHighLevelEventAware,          /* bit  6 = 1: receives the quit event       */
    onlyLocalHLEvents,              /* bit  5 = 0                                */
    notStationeryAware,             /* bit  4 = 0                                */
    dontUseTextEditServices,        /* bit  3 = 0                                */
    notDisplayManagerAware,         /* bit  2 = 0                                */
    reserved,
    reserved,
    4096 * 1024,    /* preferred */
    3072 * 1024     /* minimum   */
};

resource 'vers' (1, "CDAudioRedirector") {
    0x06,
    0x00,                   /* 6.0 */
    development,
    0x01,
    verUS,
    "6.0d1",
    "6.0d1, CD Audio Redirector - Red Book audio for legacy Mac CD games"
};

/*
 * CDPlayProbe — Phase-0 probe P5a: drive the legacy CD-audio API and see what
 * the driver actually does.
 *
 * WHY THIS AND NOT A TRACE OF A REAL GAME
 * ---------------------------------------
 * FEASIBILITY.md §7 asks for a call trace captured while Warcraft runs, to settle
 * H1 (the driver accepts AudioPlay but there is no analog route, so nothing is
 * audible) versus H2 (the ATAPI-era driver rejects the audio csCodes outright and
 * the game gives up). Tracing a running game needs resident boot code — an INIT
 * that patches the driver — and boot code costs a reboot per iteration on a
 * machine with no debugger.
 *
 * But H1 vs H2 is a question about the DRIVER, not about the game. This app asks
 * the driver directly: it issues the exact call sequence a mixed-mode game
 * issues, in the same order, and logs every csCode, parameter and result. No boot
 * code, no reboot, and it doubles as the test rig for the calls the extension
 * will eventually have to service.
 *
 * What it settles:
 *   - H1 vs H2, definitively: does AudioPlay succeed, and does anything come out?
 *   - The ADDRESS ENCODING the driver wants. The AppleCD audio calls take a
 *     "position type" byte plus a position, and the available source material does
 *     not pin down the values. This tries the plausible encodings and records
 *     which one the driver accepts, rather than guessing in the engine later.
 *   - The POSITION UNITS reported by AudioStatus and ReadQ while a track plays —
 *     absolute vs track-relative, MSF vs frames. FEASIBILITY.md §6 flags this as
 *     a real risk ("getting units wrong = music that never loops, or loops
 *     instantly"), and ten seconds of polling answers it exactly.
 *   - Whether the driver's own CD volume is the confound. If AudioPlay succeeds
 *     and nothing is audible, that only means H1 if the volume was not zero — so
 *     this reads the volume, sets it to full, and restores it afterwards.
 *
 * ⚠ THIS PROBE IS ACTIVE, not read-only. It starts and stops audio playback and
 * it changes the drive's audio volume, restoring it at the end. It always issues
 * AudioStop before it exits, including on the quit path. Run it with a mixed-mode
 * game disc or an ordinary audio CD in the drive.
 *
 * READ THE RESULT ON THE RIGHT MACHINE:
 *   - On a Mac that still has the analog CD-audio wire (e.g. the MDD), a
 *     successful AudioPlay should be AUDIBLE. That is the control case.
 *   - On the G4 mini, the expectation is: AudioPlay succeeds, nothing is audible
 *     ⇒ H1 ⇒ the interception design in FEASIBILITY.md §5 is the right fix.
 *   - If AudioPlay is REFUSED on both machines ⇒ H2 ⇒ the extension must also
 *     synthesise success-shaped replies and a believable TOC/status, not just
 *     produce sound.
 *
 * Output: "CD Play Probe Log" in the System Folder, appended — read from the LAST
 * banner. The window asks whether you heard anything and records the answer in
 * the log, so the log alone is the evidence.
 */

#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <Dialogs.h>
#include <Controls.h>
#include <ControlDefinitions.h>     /* pushButProc */
#include <TextEdit.h>
#include <Events.h>
#include <Devices.h>
#include <Files.h>
#include <Memory.h>
#include <OSUtils.h>
#include <ToolUtils.h>
#include <Timer.h>
#include <Gestalt.h>

#include <stdio.h>
#include <string.h>

#include "cd_probe_common.h"
#include "cd_cscodes.h"
#include "cd_engine.h"      /* the real CDEnginePublic, so the layout cannot drift */

#define kVersionString  "CDPlayProbe v7"

#define kPollSeconds    10      /* how long to watch a playing track    */
#define kPollTicks      15      /* poll interval, ~4 Hz                 */

/* ---- state --------------------------------------------------------------- */

typedef struct {
    CDDriverInfo cd;
    CDTOC        toc;
    short        audioTrackIdx;   /* index into toc.track, -1 if none      */
    unsigned char savedVolL, savedVolR;
    Boolean      volumeRead;
    Boolean      volumeSet;
    OSErr        trackSearchErr;
    OSErr        playErr;
    short        playPosType;     /* the encoding that worked, -1 if none  */
    const char  *playForm;        /* "MSF" or "track"                      */
    Boolean      statusMoved;     /* did the reported position advance?    */
    OSErr        pauseErr, resumeErr, stopErr;
} PlayProbe;

static PlayProbe gP;

/* ---- helpers ------------------------------------------------------------- */

static void SleepTicks(long ticks)
{
    long deadline = TickCount() + ticks;
    EventRecord evt;
    while (TickCount() < deadline)
        (void)WaitNextEvent(0, &evt, 1, NULL);   /* stay a good citizen */
}

/* ---- main probe sequence ------------------------------------------------- */

/* AudioPlay / AudioTrackSearch take a position-type byte at csParam+0 and a
 * position starting at csParam+2. The values of the position type are not pinned
 * down by the material available, so try the plausible ones and record which the
 * driver accepts. Two position FORMS are tried for each type:
 *
 *   "MSF"   — three BCD bytes M, S, F at +2, the track's start address from the
 *             TOC. This is how a game that read the TOC would address a track.
 *   "track" — the track number in BCD at +2.
 *
 * The first combination that returns noErr wins and is logged; that answer goes
 * into cd_cscodes.h afterwards. */
static OSErr TryAudioCall(short csCode, short posType, Boolean asMSF,
                          int m, int s, int f, int trackNum,
                          Boolean stopFlag, const char *label)
{
    short param[11];
    unsigned char *b = (unsigned char *)param;
    OSErr err;

    memset(param, 0, sizeof(param));
    b[0] = (unsigned char)posType;          /* +0: position type */
    if (asMSF) {
        b[2] = kBinToBCD(m);                /* +2..+4: M S F, BCD */
        b[3] = kBinToBCD(s);
        b[4] = kBinToBCD(f);
    } else {
        b[2] = kBinToBCD(trackNum);         /* +2: track number, BCD */
    }
    b[6] = stopFlag ? 1 : 0;                /* +6: stop/hold flag */
    b[9] = 0;                               /* +9: play mode, 0 = stereo */

    CDLogf("  attempt %s: csCode=%d posType=%d form=%s (%02d:%02d:%02d / trk %d)",
           label, csCode, posType, asMSF ? "MSF" : "track", m, s, f, trackNum);
    err = CDControlCall(gP.cd.refNum, csCode, param, sizeof(param), NULL, 0);
    CDLogf("    -> err=%d", err);
    return err;
}

/* Poll AudioStatus and ReadQ repeatedly and dump the raw bytes each time. The
 * point is not to interpret them now but to capture enough samples that the units
 * convention is unambiguous afterwards: whatever field counts up at 75 units per
 * second is the frame counter, and whether it starts at the track boundary or at
 * the start of the disc tells us absolute vs track-relative. */
/* ★ Report the PUMP's state into the PROBE's log, once per poll.
 *
 * WHY IT LIVES HERE. The v3 freeze run put a heartbeat in the pump's own log and it
 * measured nothing: that log had stopped receiving lines entirely, while the pump was
 * provably still playing — its synthesised position climbed smoothly to 3.00 s of
 * delivered audio. In the same window this probe wrote 359 lines to its own log, so
 * the File Manager and the volume were healthy; it was the pump's logging channel that
 * was broken. Instrumentation sent down a broken channel measures nothing, so the pump
 * publishes to memory and the probe reports it here.
 *
 * What each field answers when the machine next wedges:
 *   beat frozen while polls continue  -> the pump stopped being scheduled
 *   beat still climbing at the end    -> the pump was alive; the wedge is elsewhere
 *   quiet > 0                         -> the pump's log was suppressed, not failing
 *   logErr != 0                       -> its writes were failing, and now we know it
 *   reqW climbing with reqR stuck     -> the drain loop stopped draining */
static void LogPumpState(void)
{
    long gv = 0;

    if (Gestalt(kEnginePublicSelector, &gv) != noErr || gv == 0) return;
    {
        CDEnginePublic *pub = (CDEnginePublic *)gv;
        if (pub->magic != kEngineMagic || pub->version != kEngineVersion) return;
        CDLogf("    pump: beat=%ld reqSeen=%ld state=%d absF=%ld | "
               "reqR=%ld reqW=%ld drop=%ld | under=%ld | quiet=%d logErr=%d "
               "logWrites=%ld",
               pub->pumpBeat, pub->pumpReqSeen, pub->playState, pub->curAbsFrame,
               pub->reqRead, pub->reqWrite, pub->reqDropped, pub->pumpUnderruns,
               pub->logQuietDepth, pub->logLastErr, pub->logWrites);
    }
}

/* LBA -> absolute MSF, the inverse of CDMSFToLBA. Absolute frame numbering starts
 * 150 frames (2 s) before LBA 0, which is the lead-in. */
static void LBAToMSF(long lba, int *m, int *s, int *f)
{
    long absF = lba + kCDDALeadInSectors;
    if (absF < 0) absF = 0;
    *m = (int)(absF / (75L * 60L));
    *s = (int)((absF / 75L) % 60L);
    *f = (int)(absF % 75L);
}

/* Issue AudioPlay at an arbitrary LBA, using the position encoding phase A already
 * discovered rather than rediscovering it. Used by the track-switch and
 * end-of-track phases, which need to start playback somewhere specific. */
static OSErr PlayAtLBA(long lba, const char *label)
{
    short          param[11];
    unsigned char *b = (unsigned char *)param;
    int            m, s, f;
    OSErr          err;

    LBAToMSF(lba, &m, &s, &f);
    memset(param, 0, sizeof(param));
    b[0] = (unsigned char)(gP.playPosType >= 0 ? gP.playPosType : 0);
    b[2] = kBinToBCD(m);
    b[3] = kBinToBCD(s);
    b[4] = kBinToBCD(f);
    b[6] = 0;                        /* play, do not merely position */
    b[9] = 0;                        /* stereo */

    CDLogf("--- %s: AudioPlay at LBA %ld = %02d:%02d:%02d ---", label, lba, m, s, f);
    err = CDControlCall(gP.cd.refNum, kcsAudioPlay, param, sizeof(param), NULL, 0);
    CDLogf("  AudioPlay err=%d", err);
    return err;
}

/* Poll AudioStatus and ReadQ repeatedly and dump the raw bytes each time.
 *
 * `watchCompletion` is for the end-of-track phase: it additionally watches for the
 * synthesised status byte reaching 0x13 (completed) and for the position ceasing to
 * advance, which together are what a game polling for the end of a track has to see.
 *
 * Returns the number of polls in which the reported absolute position was unchanged
 * from the previous one — the "held" count. */
static int PollFor(int seconds, const char *label, Boolean watchCompletion)
{
    int   i;
    const int polls = (seconds * 60) / kPollTicks;
    short first[11], last[11];
    Boolean haveFirst = false;
    long  prevAbs = -1;
    int   heldCount = 0;
    Boolean sawCompleted = false;

    CDLogf("--- %s: polling AudioStatus + ReadQ for %d s ---", label, seconds);

    for (i = 0; i < polls; i++) {
        short buf[11];
        OSErr err;

        CDLogf("  poll %d (t=%ld ticks)", i, (long)TickCount());
        LogPumpState();

        err = CDStatusCall(gP.cd.refNum, kcsAudioStatus, buf, sizeof(buf));
        if (err != noErr)
            err = CDControlCall(gP.cd.refNum, kcsAudioStatus, NULL, 0,
                                buf, sizeof(buf));
        if (err == noErr) {
            unsigned char *sb = (unsigned char *)buf;
            long absF;

            if (!haveFirst) {
                BlockMoveData(buf, first, sizeof(first));
                haveFirst = true;
            }
            BlockMoveData(buf, last, sizeof(last));

            /* bytes 3..5 are the absolute MSF, BCD, cross-checked to the frame
             * against the TOC in Phase 0. */
            absF = ((long)kBCDToBin(sb[3]) * 60L + kBCDToBin(sb[4])) * 75L
                   + kBCDToBin(sb[5]);
            if (absF == prevAbs) heldCount++;
            prevAbs = absF;

            if (sb[0] == kSynthStatusCompleted) {
                if (!sawCompleted)
                    CDLogf("  ★ status byte reached 0x%02X (COMPLETED) at poll %d, "
                           "abs frame %ld", sb[0], i, absF);
                sawCompleted = true;
            }
        }

        err = CDStatusCall(gP.cd.refNum, kcsReadTheQSubcode, buf, sizeof(buf));
        if (err != noErr)
            (void)CDControlCall(gP.cd.refNum, kcsReadTheQSubcode, NULL, 0,
                                buf, sizeof(buf));

        SleepTicks(kPollTicks);
    }

    if (haveFirst) {
        CDLogf("  first AudioStatus csParam:");
        CDLogHex("first", first, 22);
        CDLogf("  last  AudioStatus csParam:");
        CDLogHex("last ", last, 22);
        gP.statusMoved = (memcmp(first, last, 22) != 0);
        CDLogf("  ⇒ reported status %s over %d s. %s",
               gP.statusMoved ? "CHANGED" : "did NOT change", seconds,
               gP.statusMoved
                   ? "A moving position means audio really is being delivered."
                   : "A frozen position means nothing is playing, whatever "
                     "AudioPlay returned.");
        if (watchCompletion) {
            CDLogf("  ⇒ end-of-track watch: completed status %s, position held in "
                   "%d of %d polls",
                   sawCompleted ? "SEEN (0x13)" : "NEVER SEEN", heldCount, polls);
            if (!sawCompleted)
                CDLogf("    the track boundary was not reached, or completion is not "
                       "being reported. Either way a game polling for track end "
                       "would still be waiting.");
        }
    } else {
        CDLogf("  ⇒ AudioStatus never answered.");
    }
    return heldCount;
}

static void PollPosition(void)
{
    (void)PollFor(kPollSeconds, "phase A (track start)", false);
}

static void RunProbe(void)
{
    int t;

    CDLogf("--- P5a: locate and classify the optical driver ---");

    /* ★ NEVER sweep the unit table by default. It hung the machine on 2026-08-07.
     *
     * The probe was launched with an empty tray, so the CD had no drive-queue entry,
     * stage 1 found nothing, and the probe fell through to the full unit-table sweep —
     * which sends Status calls to arbitrary unrelated drivers and hangs on any one that
     * never completes. It hung on refNum -10, and the run was written off.
     *
     * That hazard was known: v1 of this probe swept unconditionally and hung, the sweep
     * prints its own warning, and the pump has always passed allowFullSweep = false.
     * Only the probe still had it armed by default, and a documented trap that is still
     * the default is just a trap.
     *
     * So: the sweep is now OPT-IN (hold option), and with no CD in the drive queue the
     * probe stops and says what to do instead of going looking. There is nothing to
     * probe without a disc anyway. */
    {
        KeyMap  km2;
        Boolean allowSweep;
        GetKeys(km2);
        allowSweep = KeyIsDown(km2, kOptionKeyCode);

        CDFindDriver(&gP.cd, allowSweep);

        if (!gP.cd.found) {
            CDLogf("no CD driver in the drive queue.");
            CDLogf("  ⇒ almost certainly NO DISC IN THE DRIVE. Insert an audio CD, wait");
            CDLogf("    for it to mount, and run this again.");
            CDLogf("  ⇒ NOT sweeping the unit table: that sends Status calls to unrelated");
            CDLogf("    drivers and hung this machine on 2026-08-07. Hold OPTION at launch");
            CDLogf("    if you really want the sweep.");
            CDProgressSay("NO DISC? insert a CD and re-run");
            return;
        }
    }

    CDReadTOC(gP.cd.refNum, &gP.toc);
    gP.audioTrackIdx = -1;
    for (t = 0; t < gP.toc.trackCount; t++) {
        if (!gP.toc.track[t].isData) { gP.audioTrackIdx = t; break; }
    }
    if (gP.audioTrackIdx < 0) {
        CDLogf("no audio track on this disc. Put a mixed-mode game disc or an "
               "ordinary audio CD in the drive and re-run.");
        return;
    }
    CDLogf("target: track %d at %02d:%02d:%02d (lba %ld)",
           gP.toc.track[gP.audioTrackIdx].number,
           gP.toc.track[gP.audioTrackIdx].m,
           gP.toc.track[gP.audioTrackIdx].s,
           gP.toc.track[gP.audioTrackIdx].f,
           gP.toc.track[gP.audioTrackIdx].lba);

    /* --- volume, so that silence means something --- */
    {
        short          buf[11];
        unsigned char *b = (unsigned char *)buf;
        short          param[11];
        OSErr          err;

        CDLogf("--- audio volume (so that 'no sound' cannot just be volume 0) ---");
        err = CDStatusCall(gP.cd.refNum, kcsReadAudioVolume, buf, sizeof(buf));
        if (err == noErr) {
            gP.volumeRead = true;
            gP.savedVolL  = b[0];
            gP.savedVolR  = b[1];
            CDLogf("  ReadAudioVolume: left=%d right=%d", b[0], b[1]);
        } else {
            CDLogf("  ReadAudioVolume err=%d (cannot tell what the volume was)",
                   err);
        }

        memset(param, 0, sizeof(param));
        ((unsigned char *)param)[0] = 255;
        ((unsigned char *)param)[1] = 255;
        err = CDControlCall(gP.cd.refNum, kcsAudioControl, param, sizeof(param),
                            NULL, 0);
        gP.volumeSet = (err == noErr);
        CDLogf("  AudioControl(255,255) err=%d%s", err,
               (err == noErr) ? "" : "  <-- volume could not be set");
    }

    /* --- AudioTrackSearch, then AudioPlay, across candidate encodings --- */
    {
        int   posType;
        OSErr err = paramErr;
        int   m = gP.toc.track[gP.audioTrackIdx].m;
        int   s = gP.toc.track[gP.audioTrackIdx].s;
        int   f = gP.toc.track[gP.audioTrackIdx].f;
        int   trk = gP.toc.track[gP.audioTrackIdx].number;

        gP.playPosType = -1;
        gP.playForm    = "none";

        CDLogf("--- AudioTrackSearch (position to the track and hold) ---");
        for (posType = 0; posType <= 3 && err != noErr; posType++)
            err = TryAudioCall(kcsAudioTrackSearch, posType, true,
                               m, s, f, trk, true, "TrackSearch/MSF");
        if (err != noErr)
            for (posType = 0; posType <= 3 && err != noErr; posType++)
                err = TryAudioCall(kcsAudioTrackSearch, posType, false,
                                   m, s, f, trk, true, "TrackSearch/track");
        gP.trackSearchErr = err;
        CDLogf("  ⇒ AudioTrackSearch best err=%d", err);

        CDLogf("--- AudioPlay ---");
        err = paramErr;
        for (posType = 0; posType <= 3 && err != noErr; posType++) {
            err = TryAudioCall(kcsAudioPlay, posType, true,
                               m, s, f, trk, false, "AudioPlay/MSF");
            if (err == noErr) { gP.playPosType = posType; gP.playForm = "MSF"; }
        }
        if (err != noErr) {
            for (posType = 0; posType <= 3 && err != noErr; posType++) {
                err = TryAudioCall(kcsAudioPlay, posType, false,
                                   m, s, f, trk, false, "AudioPlay/track");
                if (err == noErr) {
                    gP.playPosType = posType;
                    gP.playForm    = "track";
                }
            }
        }
        gP.playErr = err;
        CDLogf("  ⇒ AudioPlay err=%d posType=%d form=%s",
               err, gP.playPosType, gP.playForm);

        if (err != noErr) {
            CDLogf("  ⇒ H2 INDICATED: the driver refused AudioPlay in every "
                   "encoding tried. The extension will have to emulate the audio "
                   "surface convincingly, not merely add sound. (Check the "
                   "per-attempt errors above: paramErr suggests a wrong "
                   "encoding, controlErr suggests the csCode is unimplemented.)");
            return;
        }
        CDLogf("  ⇒ the driver ACCEPTED AudioPlay. Whether anything is audible is "
               "now the H1 test — see the user's answer at the end of this run.");
        CDProgressSay("PLAYING track %d - LISTEN NOW for ~12 seconds",
                      gP.toc.track[gP.audioTrackIdx].number);
    }

    /* --- watch it --- */
    PollPosition();

    /* --- pause / resume --- */
    {
        short param[11];
        CDLogf("--- AudioPause (1 = pause) ---");
        memset(param, 0, sizeof(param));
        ((unsigned char *)param)[0] = 1;
        gP.pauseErr = CDControlCall(gP.cd.refNum, kcsAudioPause,
                                    param, sizeof(param), NULL, 0);
        CDLogf("  pause err=%d", gP.pauseErr);
        SleepTicks(60);

        CDLogf("--- AudioPause (0 = resume) ---");
        memset(param, 0, sizeof(param));
        ((unsigned char *)param)[0] = 0;
        gP.resumeErr = CDControlCall(gP.cd.refNum, kcsAudioPause,
                                     param, sizeof(param), NULL, 0);
        CDLogf("  resume err=%d", gP.resumeErr);
        SleepTicks(60);
    }

    /* ============================================================================
     * ★ PHASES B AND C — the two mechanism gaps that have been open since the
     * beginning, and that the ship gate cannot be closed without.
     *
     * Everything before this point plays the FIRST audio track from its START, for
     * ten seconds. That is all any run has ever done, so two things a real game does
     * routinely have never once executed on hardware:
     *
     *   B  a second AudioPlay for a DIFFERENT track. The pump has to recompute
     *      gTrackStartLBA and the play range; nothing has ever made it.
     *   C  a track reaching its NATURAL END. The pump is supposed to hold the final
     *      position and report status 0x13 (completed) instead of reverting to the
     *      driver's stale answer — the path a looping game polls for. Never run,
     *      because a probe that stops after twelve seconds never reaches the end of
     *      a three-minute track.
     *
     * C is made cheap by starting playback a few seconds BEFORE the boundary rather
     * than waiting out the track: the pump derives the end of the range from the
     * TOC, so it arrives on schedule either way.
     * ============================================================================ */
    {
        short idxB = -1;
        int   i;

        /* A different audio track that still has a successor in the TOC, since the
         * end of the range is the next track's start. */
        for (i = 0; i < gP.toc.trackCount - 1; i++) {
            if (i == gP.audioTrackIdx) continue;
            if (gP.toc.track[i].isData)  continue;
            idxB = (short)i;
            break;
        }

        if (gP.playErr != noErr) {
            CDLogf("--- phases B and C SKIPPED: phase A never played ---");
        } else if (idxB < 0) {
            CDLogf("--- phases B and C SKIPPED ---");
            CDLogf("  no second audio track with a successor in the TOC, so there is");
            CDLogf("  no track to switch to and no boundary to run into. A disc with");
            CDLogf("  three or more audio tracks exercises both.");
        } else {
            long startB = gP.toc.track[idxB].lba;
            long endB   = gP.toc.track[idxB + 1].lba;

            /* ---- PHASE B: switch to a different track ---- */
            CDLogf("=== PHASE B: switch to track %d (LBA %ld..%ld) ===",
                   gP.toc.track[idxB].number, startB, endB);
            CDLogf("  the pump should report 'inside track %d' and the cursor should",
                   gP.toc.track[idxB].number);
            CDLogf("  restart from zero rather than continuing phase A's count.");
            CDProgressSay("PHASE B: switching to track %d - LISTEN",
                          gP.toc.track[idxB].number);

            if (PlayAtLBA(startB, "phase B") == noErr)
                (void)PollFor(6, "phase B (track switch)", false);
            else
                CDLogf("  ⇒ the switch was REFUSED. A game changing tracks would get "
                       "silence here.");

            /* ---- PHASE C: run into the end of that track ---- */
            {
                long lead  = 6L * kCDDASectorsPerSec;   /* start 6 s before the end */
                long startC = endB - lead;
                if (startC < startB) startC = startB;

                CDLogf("=== PHASE C: end of track %d, starting %ld s before the "
                       "boundary ===", gP.toc.track[idxB].number,
                       (endB - startC) / kCDDASectorsPerSec);
                CDLogf("  expect the position to climb to LBA %ld and then STOP, with",
                       endB);
                CDLogf("  the status byte becoming 0x13 and the pump reporting state=3.");
                CDLogf("  a game waiting for the end of a track is waiting for exactly");
                CDLogf("  this, and it has never happened on hardware before now.");
                CDProgressSay("PHASE C: playing into the end of track %d",
                              gP.toc.track[idxB].number);

                if (PlayAtLBA(startC, "phase C") == noErr)
                    (void)PollFor(12, "phase C (end of track)", true);
                else
                    CDLogf("  ⇒ REFUSED, so end-of-track behaviour is still untested.");
            }
        }
    }
}

/* Always run, including on the early-exit paths: leave the drive quiet and the
 * volume as we found it. */
static void Cleanup(void)
{
    short param[11];

    if (!gP.cd.found) return;

    CDLogf("--- cleanup ---");
    memset(param, 0, sizeof(param));
    gP.stopErr = CDControlCall(gP.cd.refNum, kcsAudioStop,
                               param, sizeof(param), NULL, 0);
    CDLogf("  AudioStop err=%d", gP.stopErr);

    if (gP.volumeRead && gP.volumeSet) {
        memset(param, 0, sizeof(param));
        ((unsigned char *)param)[0] = gP.savedVolL;
        ((unsigned char *)param)[1] = gP.savedVolR;
        CDLogf("  restoring volume to left=%d right=%d",
               gP.savedVolL, gP.savedVolR);
        (void)CDControlCall(gP.cd.refNum, kcsAudioControl,
                            param, sizeof(param), NULL, 0);
    }
}

/* ---- the "did you hear it?" window --------------------------------------- */

static void DrawAt(short v, const char *label, const char *value)
{
    Str255 s;
    char   tmp[256];
    int    n;

    n = snprintf(tmp, sizeof(tmp), "%s%s", label, value ? value : "");
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    s[0] = (unsigned char)n;
    BlockMoveData(tmp, s + 1, n);
    MoveTo(12, v);
    DrawString(s);
}

/* Returns 1 = heard music, 0 = silence, -1 = no answer. */
static short AskDidYouHearIt(void)
{
    WindowPtr   win;
    Rect        bounds, r;
    ControlHandle heard, silent;
    EventRecord evt;
    char        tmp[256];
    short       v = 22;
    short       answer = -1;

    SetRect(&bounds, 40, 60, 40 + 580, 60 + 300);
    win = NewWindow(NULL, &bounds, "\p" kVersionString, true, documentProc,
                    (WindowPtr)-1L, false, 0);
    if (win == NULL) return -1;
    SetPort(win);
    TextFont(kFontIDGeneva);
    TextSize(9);

    DrawAt(v, "CD Audio Redirector - P5a: legacy audio API probe", ""); v += 18;

    if (!gP.cd.found) {
        DrawAt(v, "No CD driver found. See 'CD Play Probe Log'.", ""); v += 16;
    } else if (gP.audioTrackIdx < 0) {
        DrawAt(v, "No audio track on this disc. Insert a mixed-mode game", "");
        v += 14;
        DrawAt(v, "disc or an audio CD and run again.", ""); v += 16;
    } else {
        snprintf(tmp, sizeof(tmp), "%s, refNum %d, track %d",
                 gP.cd.isNative ? "native ndrv" : "classic DRVR",
                 gP.cd.refNum, gP.toc.track[gP.audioTrackIdx].number);
        DrawAt(v, "driver:      ", tmp); v += 14;

        snprintf(tmp, sizeof(tmp), "err=%d", gP.trackSearchErr);
        DrawAt(v, "TrackSearch: ", tmp); v += 14;

        snprintf(tmp, sizeof(tmp), "err=%d   posType=%d form=%s",
                 gP.playErr, gP.playPosType, gP.playForm);
        DrawAt(v, "AudioPlay:   ", tmp); v += 14;

        snprintf(tmp, sizeof(tmp), "%s",
                 gP.statusMoved ? "position advanced (drive is transporting)"
                                : "position never moved");
        DrawAt(v, "AudioStatus: ", tmp); v += 18;

        if (gP.playErr != noErr) {
            DrawAt(v, "The driver REFUSED AudioPlay -> H2.", ""); v += 14;
            DrawAt(v, "The extension must emulate the audio surface, not just", "");
            v += 14;
            DrawAt(v, "add sound. Detail in the log.", ""); v += 18;
        } else {
            DrawAt(v, "The driver ACCEPTED AudioPlay. The remaining question is", "");
            v += 14;
            DrawAt(v, "whether you HEARD anything during the last ~12 seconds.", "");
            v += 18;
            DrawAt(v, "  music audible -> the analog path works on this Mac", "");
            v += 14;
            DrawAt(v, "  silence       -> H1 confirmed: accepted, but no route", "");
            v += 18;
        }
    }

    DrawAt(v, "Answer below; it is recorded in the log.", ""); v += 6;

    SetRect(&r, 20, v + 8, 170, v + 28);
    heard = NewControl(win, &r, "\pI heard music", true, 0, 0, 1,
                       pushButProc, 0);
    SetRect(&r, 190, v + 8, 340, v + 28);
    silent = NewControl(win, &r, "\pIt was silent", true, 0, 0, 1,
                        pushButProc, 0);
    DrawControls(win);

    while (answer < 0) {
        if (WaitNextEvent(mDownMask | keyDownMask, &evt, 10, NULL)) {
            if (evt.what == mouseDown) {
                Point p = evt.where;
                ControlHandle which;
                GlobalToLocal(&p);
                if (FindControl(p, win, &which)) {
                    if (TrackControl(which, p, NULL)) {
                        if (which == heard)  answer = 1;
                        if (which == silent) answer = 0;
                    }
                }
            } else if (evt.what == keyDown) {
                char c = evt.message & charCodeMask;
                if (c == 'y' || c == 'Y') answer = 1;
                if (c == 'n' || c == 'N') answer = 0;
                if (c == 27)              answer = -1;   /* escape: no answer */
                if (c == 27) break;
            }
        }
    }

    DisposeWindow(win);
    return answer;
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    short   answer;
    KeyMap  km;
    Boolean safeMode, logOK;   /* safeMode: informational only, see below */

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    GetKeys(km);
    safeMode = KeyIsDown(km, kShiftKeyCode);

    memset(&gP, 0, sizeof(gP));
    gP.audioTrackIdx = -1;
    gP.playPosType   = -1;
    gP.playForm      = "none";

    /* Progress window before any driver call, for the same reason as CDRecon:
     * this probe spends ~12 seconds deliberately waiting on playback, and
     * "waiting on purpose" has to look different from "hung". */
    CDProgressOpen("\p" kVersionString " - progress");
    CDProgressSay("%s starting", kVersionString);
    /* The sweep is opt-in now (option), so shift no longer gates anything here.
     * Kept as a no-op rather than silently repurposing a key people may still
     * be holding out of habit. */
    if (safeMode) CDProgressSay("shift held (the sweep is opt-in now anyway)");

    logOK = CDLogOpen("\pCD Play Probe Log");
    if (!logOK)
        CDProgressSay("!! could not open 'CD Play Probe Log' - screen only");
    CDLogBanner(kVersionString " - legacy CD-audio API probe (P5a)",
                "ACTIVE probe: starts/stops playback and changes CD volume");

    RunProbe();
    Cleanup();
    CDLogFlush();

    CDProgressSay("done - answer the question in the next window");
    CDProgressClose();
    answer = AskDidYouHearIt();

    CDLogf("--- listener verdict ---");
    CDLogf("  user reported: %s",
           (answer == 1) ? "MUSIC WAS AUDIBLE"
                         : (answer == 0) ? "SILENCE" : "no answer given");
    if (gP.playErr == noErr && answer == 0)
        CDLogf("  ⇒ H1 CONFIRMED: the driver accepts the legacy audio calls but "
               "there is no route to the speakers. Interception + DAE is the "
               "right design.");
    /* ★ Ask the redirector directly. Judging by the transport position no longer
     * works: before synthesis existed, a frozen position meant the redirector was
     * doing the playing, but now the redirector deliberately makes the position ADVANCE
     * — so "moved" is consistent with both the analog path and with us. The only
     * definitive answer is to ask whether the redirector is installed and playing. */
    {
        long  gv = 0;
        if (Gestalt(kEnginePublicSelector, &gv) == noErr && gv != 0) {
            /* ★ The real CDEnginePublic from cd_engine.h, not a hand-copied struct.
             * This used to be a local re-declaration of the layout, which meant the
             * engine could not change a field without this probe silently reading the
             * wrong offsets — and the request ring did exactly that, moving everything
             * after it. Include the header and the compiler keeps them honest. */
            CDEnginePublic *pub = (CDEnginePublic *)gv;

            CDLogf("--- the CD Audio Redirector IS resident ---");
            if (pub->magic != kEngineMagic) {
                CDLogf("  !! magic is 0x%08lX, expected 0x%08lX - not our block, so",
                       (unsigned long)pub->magic, (unsigned long)kEngineMagic);
                CDLogf("     nothing below can be trusted.");
            } else if (pub->version != kEngineVersion) {
                CDLogf("  !! engine reports version %d, this probe was built for %d.",
                       pub->version, kEngineVersion);
                CDLogf("     The layout differs, so the numbers below would be");
                CDLogf("     nonsense. Rebuild both from the same tree.");
            } else {
                CDLogf("  patched=%d  pumpAlive=%d  pumpPlaying=%d  underruns=%ld",
                       pub->patched, pub->pumpAlive, pub->pumpPlaying,
                       pub->pumpUnderruns);
                CDLogf("  requests: %ld posted, %ld serviced, %ld DROPPED",
                       pub->reqWrite, pub->reqRead, pub->reqDropped);
                if (pub->reqDropped > 0)
                    CDLogf("  !! dropped requests mean the pump missed calls this probe "
                           "made - treat any silence below as explained by that.");
            }
            CDLogf("  ⇒ any music heard came from the redirector's digital path, and");
            CDLogf("    any moving position was synthesised by it. This is the fixed");
            CDLogf("    machine, not the analog control case.");
        } else {
            CDLogf("--- the CD Audio Redirector is NOT resident ---");
            CDLogf("  ⇒ this run is the unpatched baseline: music, if any, can only");
            CDLogf("    have come from a working analog CD-audio path.");
        }
    }

    if (gP.playErr == noErr && answer == 1) {
        CDLogf("  ⇒ MUSIC WAS AUDIBLE. Two very different causes, and the transport");
        CDLogf("    position above distinguishes them:");
        CDLogf("    position lines above are NOT the discriminator any more — the");
        CDLogf("    redirector synthesises a moving position on purpose. Use the");
        CDLogf("    'IS/is NOT resident' block above instead: resident means the");
        CDLogf("    music and the movement are both ours; not resident means a");
        CDLogf("    working analog path (the MDD control case).");
    }
    if (gP.playErr != noErr)
        CDLogf("  ⇒ H2: playback was never accepted, so audibility says nothing.");

    CDLogf("=== end of run: found=%d play=%d posType=%d moved=%d verdict=%d",
           gP.cd.found, gP.playErr, gP.playPosType, gP.statusMoved, answer);
    CDLogClose();

    return 0;
}

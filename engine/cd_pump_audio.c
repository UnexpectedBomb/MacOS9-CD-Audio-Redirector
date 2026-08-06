/*
 * cd_pump_audio.c — the audio engine, in the PUMP APPLICATION's task context.
 *
 * ★ WHY IT LIVES HERE AND NOT IN THE DRIVER PATCH
 * The first attempt put this inside the patched Control entry and deadlocked: a
 * synchronous PBRead to a driver cannot begin until the Control call it is nested inside
 * returns, and that Control call was waiting for the read. `AudioPlay` froze on the first
 * nested call. `accRun` was no escape either, since accRun is itself a Control call.
 *
 * So the driver patch now only records "the game asked for track N" in a mailbox and
 * chains, and everything below runs here — in an ordinary application's task context,
 * outside any Control call. That is precisely where Phase 1 measured 30 seconds of
 * streaming with zero underruns, so this is proven code doing a proven thing, merely
 * triggered from somewhere new.
 *
 * The engine parameters are Phase 1's, unchanged and hardware-verified on this machine:
 * DAE via `ChangeBlockSize(2352)` + driver-level `PBRead`; `SndPlayDoubleBuffer` with
 * `'sowt'` (so no byte swap) and `compressionID = notCompressed`; 0.25 s double buffers;
 * a 2-second ring; 32-sector reads.
 *
 * ★ INTERRUPT SAFETY is unchanged from Phase 1: the doubleback proc copies out of a
 * pre-allocated ring, emits silence on underrun, and never allocates, blocks, or touches
 * the File Manager. The ring is single-producer/single-consumer with no shared mutable
 * counter — `writeOff` task-level only, `readOff` interrupt-level only, availability
 * derived from the difference.
 */

#include <MacTypes.h>
#include <Devices.h>
#include <Disks.h>
#include <Files.h>
#include <MacMemory.h>
#include <Sound.h>
#include <Events.h>

#include "cd_engine.h"
#include "cd_cscodes.h"
#include "cd_probe_common.h"

#define kDBufFrames     11025               /* 0.25 s */
#define kDBufBytes      (kDBufFrames * 4)
#define kRingSeconds    2
#define kChunkSectors   32                  /* 75264 bytes per read */

/* ---- state ---------------------------------------------------------------- */

static short              gRefNum, gDriveNum;
static SndChannelPtr      gChan     = NULL;
static SndDoubleBufferPtr gDBuf[2]  = { NULL, NULL };
static SndDoubleBackUPP   gDBackUPP = NULL;

static unsigned char     *gPCM      = NULL;
static long               gPCMBytes = 0;
static volatile long      gReadOff  = 0;    /* interrupt writes this only  */
static volatile long      gWriteOff = 0;    /* task level writes this only */

static volatile Boolean   gPlaying  = false;
static Boolean            gPaused   = false;
static volatile long      gUnderruns = 0;
static long               gNextLBA  = 0, gEndLBA = 0;
static short              gSavedBlockSize = 512;
static Boolean            gBlockSizeTaken = false;

static CDTOC              gTOC;

/* Which track we are inside, so the cursor can report a track-relative position. */
static short              gCurTrack      = 1;
static long               gTrackStartLBA = 0;
static long               gPlayStartLBA  = 0;

/* The published block, so the cursor can be handed to the handler. */
static CDEnginePublic    *gPub = NULL;

void CDPumpSetPublic(CDEnginePublic *pub) { gPub = pub; }

/* ★ Publish where playback has ACTUALLY reached.
 *
 * Derived from gReadOff — bytes the Sound Manager has consumed — not from what was
 * requested and not from how much has been read off the disc. That distinction is the
 * whole point: FEASIBILITY §6 warned that a cursor taken from the wrong place gives
 * music that never loops or loops instantly, and the only honest source is what has
 * been handed to the speaker.
 *
 * 2352 bytes = one CD frame, so bytes/2352 is frames played. Absolute frame numbering
 * is LBA + 150 (the lead-in), matching what ReadQ reported on hardware. */
static void PublishCursor(void)
{
    long framesPlayed, absF, relF;

    if (gPub == NULL) return;

    /* ★ When a track finishes naturally, HOLD the final position and report
     * "completed" rather than falling back to playState 0.
     *
     * Reverting to 0 would stop synthesis, and the driver's own answer is a stale
     * position from whenever it was last touched — on this machine, a leftover from an
     * earlier iTunes session, in a different track entirely. A game polling for track
     * end would see the position jump backwards to nonsense instead of arriving at the
     * track boundary. A real drive holds the end position and reports completion, so
     * that is what we report. Only an explicit AudioStop returns to 0. */
    if (!gPlaying) {
        if (gPub->playState == 1 || gPub->playState == 2) gPub->playState = 3;
        gPub->posSeq++;
        return;
    }

    framesPlayed = gReadOff / kCDDASectorBytes;
    absF = gPlayStartLBA + framesPlayed + kCDDALeadInSectors;
    relF = (gPlayStartLBA - gTrackStartLBA) + framesPlayed;
    if (relF < 0) relF = 0;

    gPub->curTrack    = gCurTrack;
    gPub->curAbsFrame = absF;
    gPub->curRelFrame = relF;
    gPub->playState   = gPaused ? 2 : 1;
    gPub->posSeq++;
}

/* ---- the doubleback proc: INTERRUPT LEVEL --------------------------------- */

static pascal void DoubleBack(SndChannelPtr chan, SndDoubleBufferPtr buf)
{
    long want = kDBufBytes;
    long have;

    (void)chan;

    if (!gPlaying || gPCM == NULL) {
        buf->dbNumFrames = 0;
        buf->dbFlags     = dbBufferReady | dbLastBuffer;
        return;
    }

    have = gWriteOff - gReadOff;
    if (have >= want) {
        long idx  = gReadOff % gPCMBytes;
        long run1 = gPCMBytes - idx;
        if (run1 >= want) {
            BlockMoveData(gPCM + idx, buf->dbSoundData, want);
        } else {
            BlockMoveData(gPCM + idx, buf->dbSoundData, run1);
            BlockMoveData(gPCM, buf->dbSoundData + run1, want - run1);
        }
        gReadOff += want;
    } else {
        long i;
        for (i = 0; i < want; i++) buf->dbSoundData[i] = 0;
        gUnderruns++;                       /* never wait, never allocate */
    }

    buf->dbNumFrames = want / 4;
    buf->dbFlags     = dbBufferReady;
}

/* ---- setup ---------------------------------------------------------------- */

OSErr CDPumpInit(short refNum, short driveNum)
{
    THz  saveZone;
    int  i;

    gRefNum   = refNum;
    gDriveNum = driveNum;

    gPCMBytes = ((long)kRingSeconds * kCDDABytesPerSec / kCDDASectorBytes)
                * kCDDASectorBytes;
    gPCM = (unsigned char *)NewPtr(gPCMBytes);
    if (gPCM == NULL) return memFullErr;

    for (i = 0; i < 2; i++) {
        gDBuf[i] = (SndDoubleBufferPtr)
                   NewPtrClear((Size)(sizeof(SndDoubleBuffer) + kDBufBytes));
        if (gDBuf[i] == NULL) return memFullErr;
    }

    gDBackUPP = NewSndDoubleBackUPP(DoubleBack);
    if (gDBackUPP == NULL) return memFullErr;

    /* SetZone(SystemZone()) around SndNewChannel: a channel allocated in this
     * application's heap would die with it, and REVIEW.md §5 flagged exactly that. The
     * pump is meant to stay running, but making the channel system-owned costs nothing
     * and removes the failure mode. */
    saveZone = GetZone();
    SetZone(SystemZone());
    if (SndNewChannel(&gChan, sampledSynth, initStereo, NULL) != noErr) gChan = NULL;
    SetZone(saveZone);
    if (gChan == NULL) return memFullErr;

    CDReadTOC(refNum, &gTOC);
    if (!gTOC.valid) CDLogf("  pump: TOC unreadable; requests cannot be resolved");

    return noErr;
}

/* ---- block size, taken only while we are actually reading ------------------ */

static void TakeBlockSize(void)
{
    short p[11];
    int   i;
    if (gBlockSizeTaken) return;
    if (CDStatusCall(gRefNum, kcsGetBlockSize, p, sizeof(p)) == noErr && p[0] > 0)
        gSavedBlockSize = p[0];
    for (i = 0; i < 11; i++) p[i] = 0;
    p[0] = kCDDASectorBytes;
    (void)CDControlCall(gRefNum, kcsChangeBlockSize, p, sizeof(p), NULL, 0);
    gBlockSizeTaken = true;
}

static void GiveBackBlockSize(void)
{
    short p[11];
    int   i;
    if (!gBlockSizeTaken) return;
    for (i = 0; i < 11; i++) p[i] = 0;
    p[0] = gSavedBlockSize;
    (void)CDControlCall(gRefNum, kcsChangeBlockSize, p, sizeof(p), NULL, 0);
    gBlockSizeTaken = false;
}

/* Lean read: no logging, because this runs inside the refill loop. */
static long ReadSectors(long lba, void *dest, long sectors)
{
    ParamBlockRec pb;
    int           i;

    for (i = 0; i < (int)sizeof(pb); i++) ((char *)&pb)[i] = 0;
    pb.ioParam.ioRefNum    = gRefNum;
    pb.ioParam.ioVRefNum   = gDriveNum;
    pb.ioParam.ioBuffer    = (Ptr)dest;
    pb.ioParam.ioReqCount  = sectors * kCDDASectorBytes;
    pb.ioParam.ioPosMode   = fsFromStart;
    pb.ioParam.ioPosOffset = lba * kCDDASectorBytes;

    if (PBReadSync(&pb) != noErr) return 0;
    return pb.ioParam.ioActCount;
}

static long RefillOnce(void)
{
    long space  = gPCMBytes - (gWriteOff - gReadOff);
    long idx    = gWriteOff % gPCMBytes;
    long contig = gPCMBytes - idx;
    long sectors = kChunkSectors;
    long got;

    if (sectors * kCDDASectorBytes > space)  sectors = space / kCDDASectorBytes;
    if (sectors * kCDDASectorBytes > contig) sectors = contig / kCDDASectorBytes;
    if (sectors <= 0 || gNextLBA >= gEndLBA) return 0;
    if (gNextLBA + sectors > gEndLBA) sectors = gEndLBA - gNextLBA;
    if (sectors <= 0) return 0;

    got = ReadSectors(gNextLBA, gPCM + idx, sectors);
    if (got <= 0) return 0;
    gWriteOff += got;
    gNextLBA  += got / kCDDASectorBytes;
    return got;
}

/* ---- transport ------------------------------------------------------------ */

void CDPumpStop(void)
{
    if (gChan != NULL && gPlaying) {
        SndCommand c;
        c.cmd = quietCmd; c.param1 = 0; c.param2 = 0;
        (void)SndDoImmediate(gChan, &c);
        c.cmd = flushCmd; c.param1 = 0; c.param2 = 0;
        (void)SndDoImmediate(gChan, &c);
    }
    gPlaying = false;
    gPaused  = false;
    GiveBackBlockSize();
    /* An explicit stop really does mean stopped: back to the driver's own answers. */
    if (gPub != NULL) { gPub->playState = 0; gPub->posSeq++; }
}

/* Resolve a transport request's csParam into an LBA range using the TOC. Both the MSF
 * and track-number forms are accepted: Phase 0 measured this driver taking posType 0
 * with MSF, but a game may use either. */
static Boolean DecodePos(const unsigned char *cp, long *startLBA, long *endLBA)
{
    long  lba = -1;
    short i;

    if (!gTOC.valid) return false;

    {
        short m = kBCDToBin(cp[2]), s = kBCDToBin(cp[3]), f = kBCDToBin(cp[4]);
        if (m <= 99 && s < 60 && f < 75) lba = CDMSFToLBA(m, s, f);
    }
    {
        short trk = kBCDToBin(cp[2]);
        if (trk >= gTOC.firstTrack && trk <= gTOC.lastTrack &&
            (lba < 0 || (cp[3] == 0 && cp[4] == 0))) {
            for (i = 0; i < gTOC.trackCount; i++)
                if (gTOC.track[i].number == trk) { lba = gTOC.track[i].lba; break; }
        }
    }
    if (lba < 0) return false;

    *startLBA = lba;
    *endLBA   = lba + 75L * 60L * 80L;
    for (i = 0; i < gTOC.trackCount; i++)
        if (gTOC.track[i].lba > lba && gTOC.track[i].lba < *endLBA)
            *endLBA = gTOC.track[i].lba;
    return true;
}

OSErr CDPumpPlay(const unsigned char *csParam)
{
    SndDoubleBufferHeader2 h;
    long  a, b;
    OSErr err;
    int   i;

    if (gChan == NULL || gPCM == NULL) return notOpenErr;
    if (!DecodePos(csParam, &a, &b)) {
        CDLogf("  pump: could not resolve the requested position");
        return paramErr;
    }
    CDLogf("  pump: play LBA %ld .. %ld (%ld sectors, %ld s)",
           a, b, b - a, (b - a) / kCDDASectorsPerSec);

    if (gPlaying) CDPumpStop();

    gReadOff = 0; gWriteOff = 0;
    gNextLBA = a; gEndLBA = b;
    gUnderruns = 0; gPaused = false;

    /* Which track this position falls in, for the track-relative cursor. */
    gPlayStartLBA  = a;
    gCurTrack      = gTOC.firstTrack;
    gTrackStartLBA = a;
    {
        short i;
        for (i = 0; i < gTOC.trackCount; i++) {
            if (gTOC.track[i].lba <= a &&
                (i + 1 >= gTOC.trackCount || gTOC.track[i + 1].lba > a)) {
                gCurTrack      = gTOC.track[i].number;
                gTrackStartLBA = gTOC.track[i].lba;
                break;
            }
        }
    }
    CDLogf("  pump: inside track %d (starts at LBA %ld)", gCurTrack, gTrackStartLBA);

    TakeBlockSize();
    while ((gWriteOff - gReadOff) < gPCMBytes)
        if (RefillOnce() <= 0) break;
    CDLogf("  pump: pre-roll %ld bytes", gWriteOff);

    if (gWriteOff == 0) { GiveBackBlockSize(); return ioErr; }

    gPlaying = true;
    for (i = 0; i < 2; i++) {
        gDBuf[i]->dbFlags = 0;
        DoubleBack(gChan, gDBuf[i]);
    }

    for (i = 0; i < (int)sizeof(h); i++) ((char *)&h)[i] = 0;
    h.dbhNumChannels   = 2;
    h.dbhSampleSize    = 16;
    h.dbhCompressionID = notCompressed;
    h.dbhPacketSize    = 0;
    h.dbhSampleRate    = rate44khz;
    h.dbhBufferPtr[0]  = gDBuf[0];
    h.dbhBufferPtr[1]  = gDBuf[1];
    h.dbhDoubleBack    = gDBackUPP;
    h.dbhFormat        = k16BitLittleEndianFormat;      /* 'sowt' */

    err = SndPlayDoubleBuffer(gChan, (SndDoubleBufferHeaderPtr)&h);
    CDLogf("  pump: SndPlayDoubleBuffer err=%d", err);
    if (err != noErr) { gPlaying = false; GiveBackBlockSize(); }
    PublishCursor();
    return err;
}

void CDPumpPause(Boolean pause)
{
    SndCommand c;
    if (!gPlaying || gChan == NULL) return;
    gPaused = pause;
    c.cmd = pause ? pauseCmd : resumeCmd;
    c.param1 = 0; c.param2 = 0;
    (void)SndDoImmediate(gChan, &c);
    PublishCursor();
}

/* Called from the pump's event loop, as often as it gets time. This is Phase 1's
 * stage-B refill, which measured zero underruns over 30 seconds. */
void CDPumpIdle(void)
{
    long guard = 4;

    PublishCursor();

    if (!gPlaying || gPaused) return;

    if (gNextLBA >= gEndLBA && (gWriteOff - gReadOff) <= 0) {
        CDLogf("  pump: reached the end of the range; holding the final position "
               "and reporting completed");
        gPlaying = false;
        gPaused  = false;
        GiveBackBlockSize();
        PublishCursor();            /* -> playState 3, position held */
        return;
    }
    while (guard-- > 0)
        if (RefillOnce() <= 0) break;
}

void CDPumpStats(Boolean *playing, long *underruns, long *delivered)
{
    if (playing)   *playing   = gPlaying;
    if (underruns) *underruns = gUnderruns;
    if (delivered) *delivered = gReadOff;
}

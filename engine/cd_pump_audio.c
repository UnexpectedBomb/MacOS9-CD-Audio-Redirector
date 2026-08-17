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
static short              gNormalBlockSize = 0;   /* the drive's own, captured once */

static CDTOC              gTOC;

/* Scratch for the re-read, static rather than a local: a CDTOC carries a
 * 100-entry track table, and ~1.6 KB of stack inside the play path is not worth
 * the risk when the pump is single-threaded and task-level throughout. */
static CDTOC              gTOCScratch;
static long               gTOCGeneration = 0;   /* bumped whenever the disc changes */

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

    /* An opening read, so a pump started with a disc already in the drive has the
     * TOC on the record at install time. It is no longer load-bearing: every play
     * request re-reads (see EnsureTOC), which is what lets the faceless build start
     * at boot with an empty tray and still work when a disc turns up later. */
    CDReadTOC(refNum, &gTOC);
    if (!gTOC.valid)
        CDLogf("  pump: no TOC at startup (empty drive?) - will re-read on the "
               "first play request");

    return noErr;
}

/* ---- block size -----------------------------------------------------------
 *
 * ★★★ THE BLOCK SIZE BELONGS TO THE READ, NOT TO PLAYBACK. 2026-08-17.
 *
 * It used to be taken once when playback started and given back only on stop, so the
 * drive sat at 2352 for the whole of a piece of music, which may be minutes. The File
 * Manager expects 512. On a plain audio CD nothing else reads the disc, so that never
 * mattered and thirty hardware runs said nothing about it.
 *
 * Phase D on a real mixed-mode disc exercised it for the first time and **crashed the
 * machine**: a bus error inside _DeleteMenu following a garbage pointer, in an app
 * that never calls DeleteMenu, on roughly the second 32 KB File Manager read of a
 * 70 MB file taken while the pump was streaming. That is a memory-corruption
 * signature, not a menu bug, and a real game reading level data off track 1 does
 * exactly the same reads. See FINDINGS 2026-08-17.
 *
 * ★ WHY TAKE-AND-RESTORE PER READ ACTUALLY CLOSES THIS, rather than just narrowing it.
 * Mac OS 9 is cooperatively scheduled. A game's File Manager read runs at task level,
 * and so does ours. Another task cannot be scheduled in the middle of a sequence that
 * never yields. So as long as set-2352, read, restore contains no yield, no WaitNext-
 * Event, no logging and no allocation, a task-level reader cannot observe 2352 at all.
 * The old design held the wrong size precisely ACROSS the yields, which is the only
 * time the game gets to run: it was not so much a race as a guarantee.
 *
 * ⚠ What this does NOT cover, stated so nobody reads more into it than is there:
 * asynchronous or interrupt-time completions, and any File Manager work that runs at
 * deferred-task level, could still observe the window. Closing that needs the ATA
 * route via the 'dvrf' handle, which bypasses the shared block size entirely.
 *
 * The cost is two extra Control calls per refill at roughly 10 Hz. Measured against a
 * TOC read of three Control calls coming in around one tick, that is negligible, and
 * the underrun counter is the check.
 */

/* The drive's own block size, captured once and reused. Captured rather than assumed
 * because the value is the drive's, not ours; guarded against capturing 2352 itself,
 * which would make "restore" a no-op and hide the whole problem. */
static void CaptureNormalBlockSize(void)
{
    short p[11];
    int   i;

    if (gNormalBlockSize > 0) return;
    for (i = 0; i < 11; i++) p[i] = 0;
    if (CDStatusCall(gRefNum, kcsGetBlockSize, p, sizeof(p)) == noErr &&
        p[0] > 0 && p[0] != kCDDASectorBytes)
        gNormalBlockSize = p[0];
    else
        gNormalBlockSize = 512;
}

/* Deliberately silent and allocation-free: it runs between the two halves of the
 * no-yield sequence in RefillOnce, and anything that could yield here would reopen
 * exactly the window this exists to close. */
static void SetBlockSize(short size)
{
    short p[11];
    int   i;

    for (i = 0; i < 11; i++) p[i] = 0;
    p[0] = size;
    (void)CDControlCall(gRefNum, kcsChangeBlockSize, p, sizeof(p), NULL, 0);
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

    /* ★ THE NO-YIELD SEQUENCE. Nothing between these three statements may yield,
     * log, allocate or call the File Manager. On a cooperative system that is what
     * makes the wrong block size unobservable to another task rather than merely
     * unlikely. If you add anything here, you have reopened the crash. */
    CaptureNormalBlockSize();
    SetBlockSize(kCDDASectorBytes);
    got = ReadSectors(gNextLBA, gPCM + idx, sectors);
    SetBlockSize(gNormalBlockSize);

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
    /* Nothing to give back: the block size is restored inside every refill, so
     * outside a refill the drive is already at its own size. */
    /* An explicit stop really does mean stopped: back to the driver's own answers. */
    if (gPub != NULL) { gPub->playState = 0; gPub->posSeq++; }
}

/* Resolve a transport request's csParam into an LBA range using the TOC. Both the MSF
 * and track-number forms are accepted: Phase 0 measured this driver taking posType 0
 * with MSF, but a game may use either. */
/* ★★★ Decode strictly by the position type in the word at csParam+0.
 *
 * This used to sniff: read M,S,F from cp[2],cp[3],cp[4], read a track number from
 * cp[2], and take whichever looked plausible — never once consulting the type. It
 * agreed perfectly with CDPlayProbe, because the probe wrote the same wrong layout,
 * and thirty-odd hardware runs therefore proved only that our two halves were
 * consistent with each other.
 *
 * A REAL GAME encodes it properly, and the old code mis-read it. Warcraft asks for
 * track 2 as MSF 29:43:25, which puts 0x29,0x43,0x25 at cp[3],cp[4],cp[5]. The
 * sniffer read m=cp[2]=0, s=0x29→29, f=0x43→43, all of which pass a plausibility
 * check, and resolved LBA 2068 — inside the 261 MB DATA track. v10's DATA guard then
 * correctly refused to stream it, so the whole thing came out as silence with every
 * counter reading zero. That is exactly the report we got from a mixed-mode disc.
 *
 * The contract is in cd_cscodes.h, taken from the driver's own parser. An unknown
 * type is NOT guessed at: it is refused and counted, so if v1.4.8 differs from the
 * v1.4.0 that was disassembled, the log says so instead of playing the wrong thing.
 */
static Boolean DecodePos(const unsigned char *cp, long *startLBA, long *endLBA)
{
    long  lba = -1;
    short i;
    short posType;

    if (!gTOC.valid) return false;

    posType = (short)((cp[0] << 8) | cp[1]);     /* the WORD at csParam+0 */

    switch (posType) {
    case kCDPosTypeBlock: {
        lba = ((long)cp[2] << 24) | ((long)cp[3] << 16) |
              ((long)cp[4] << 8)  |  (long)cp[5];
        /* Bound it against what a CD can physically hold — 80 minutes is 360000
         * sectors. The TOC we keep has no lead-out field, and this check exists for
         * a specific reason rather than as decoration: the old encoding bug sent
         * type-0 addresses of ~691,000,000 and nothing rejected them. */
        if (lba < 0 || lba >= 360000L) {
            CDLogf("  pump: block address %ld is not on a CD", lba);
            return false;
        }
        break;
    }
    case kCDPosTypeMSF: {
        short m = kBCDToBin(cp[3]), s = kBCDToBin(cp[4]), f = kBCDToBin(cp[5]);
        if (m > 99 || s >= 60 || f >= 75) {
            CDLogf("  pump: MSF %02d:%02d:%02d is not a valid address", m, s, f);
            return false;
        }
        lba = CDMSFToLBA(m, s, f);
        break;
    }
    case kCDPosTypeTrack: {
        short trk = kBCDToBin(cp[5]);
        if (trk < gTOC.firstTrack || trk > gTOC.lastTrack) {
            CDLogf("  pump: track %d is outside the TOC's %d..%d",
                   trk, gTOC.firstTrack, gTOC.lastTrack);
            return false;
        }
        for (i = 0; i < gTOC.trackCount; i++)
            if (gTOC.track[i].number == trk) { lba = gTOC.track[i].lba; break; }
        break;
    }
    default:
        /* ★ Counted, never guessed. A non-zero count here means the encoding
         * contract taken from v1.4.0 does not hold for whatever is running, and
         * that has to be visible rather than inferred from silence. */
        if (gPub != NULL) gPub->posTypeUnknown++;
        CDLogf("  pump: position type %d is not one of 0 (block), 1 (MSF) or "
               "2 (track). Refusing rather than guessing — %ld seen so far.",
               posType, gPub != NULL ? gPub->posTypeUnknown : -1L);
        return false;
    }

    if (lba < 0) return false;

    *startLBA = lba;
    *endLBA   = lba + 75L * 60L * 80L;
    for (i = 0; i < gTOC.trackCount; i++)
        if (gTOC.track[i].lba > lba && gTOC.track[i].lba < *endLBA)
            *endLBA = gTOC.track[i].lba;
    return true;
}

/* Do the two TOCs describe the same disc? Field by field rather than a block
 * compare, because a struct comparison also compares padding, and the answer
 * decides whether the log gets forty lines or none. Track number, audio-vs-data
 * and start address are what DecodePos actually consumes. */
static Boolean SameDisc(const CDTOC *x, const CDTOC *y)
{
    short i;

    if (x->valid      != y->valid)      return false;
    if (x->firstTrack != y->firstTrack) return false;
    if (x->lastTrack  != y->lastTrack)  return false;
    if (x->trackCount != y->trackCount) return false;
    if (x->audioCount != y->audioCount) return false;

    for (i = 0; i < x->trackCount; i++) {
        if (x->track[i].number != y->track[i].number) return false;
        if (x->track[i].isData != y->track[i].isData) return false;
        if (x->track[i].lba    != y->track[i].lba)    return false;
    }
    return true;
}

/* ★ Re-read the TOC before resolving a request.
 *
 * WHY THIS EXISTS: the TOC used to be read exactly once, inside CDPumpInit. That
 * was survivable while the pump was a foreground app launched by hand with a disc
 * already in the drive, and fatal the moment it becomes the faceless Startup-Items
 * item it has to ship as — that launches at boot with an EMPTY drive, so the TOC
 * read fails, gTOC.valid stays false for the rest of the session, and every request
 * a game ever makes goes unresolved. The symptom would have been an extension that
 * installs perfectly and simply never plays anything, which is the worst possible
 * thing to hand to someone else to test.
 *
 * Re-reading on every play request rather than trying to detect a disc change: the
 * TOC read is three Control calls and measured one tick on this hardware, against a
 * play path whose measured cost is 1.5 seconds. A staleness heuristic would be more
 * code and more ways to be wrong, to save 0.7% of the budget.
 *
 * Quiet, because at full verbosity this is ~40 lines and as many flushed FSWrites
 * at the start of every piece of music. The summary below is what reaches the log,
 * and the full table is printed whenever the disc actually changes — which is the
 * event worth having on the record. */
/* ★ Re-find the drive number, for the same reason as the TOC.
 *
 * The empty-drive boot exposed this: with no disc in the tray the CD has no drive
 * queue entry, so the pump starts with driveNum = 0 and passes that as ioVRefNum on
 * every PBRead. It WORKED on this hardware, because the read is driver-level and
 * ioRefNum is what selects the driver — but that is now the DEFAULT configuration
 * rather than an edge case, and it rests on this particular driver ignoring
 * ioVRefNum. Another drive need not be so forgiving, and the machine that matters is
 * not this one. Refreshing it costs a walk of the drive queue. */
static void RefreshDriveNumber(void)
{
    CDDriverInfo info;
    int          i;

    if (gDriveNum != 0) return;      /* already known, and the queue cannot renumber it */

    for (i = 0; i < (int)sizeof(info); i++) ((char *)&info)[i] = 0;
    info.refNum   = gRefNum;
    info.driveNum = 0;

    CDLogSetQuiet(true);
    CDFindDriveNumber(&info);
    CDLogSetQuiet(false);

    if (info.driveNum != 0) {
        gDriveNum = info.driveNum;
        CDLogf("  pump: drive number resolved to %d (it was 0 at launch, when the "
               "tray was empty)", gDriveNum);
    }
}

static void EnsureTOC(void)
{
    Boolean hadValid = gTOC.valid;

    RefreshDriveNumber();

    /* The block size no longer needs restoring around this. It is now set and put back
     * inside RefillOnce's no-yield sequence, so outside that sequence the drive is
     * always at its normal size and the TOC read gets the 512 that every successful
     * read on this hardware has had. The old dance here existed only because playback
     * used to hold 2352 across everything. */
    CDLogSetQuiet(true);
    CDReadTOC(gRefNum, &gTOCScratch);
    CDLogSetQuiet(false);

    if (!gTOCScratch.valid) {
        /* Keep a TOC we already trust: a transient read failure must not throw away
         * a good one and turn a playing disc into an unresolvable request. */
        CDLogf(hadValid
               ? "  pump: TOC re-read failed; keeping the TOC already in hand"
               : "  pump: no readable TOC (empty drive, or a disc without one)");
        return;
    }

    if (hadValid && SameDisc(&gTOC, &gTOCScratch)) return;   /* same disc, say nothing */

    gTOC = gTOCScratch;
    gTOCGeneration++;
    CDLogf("  pump: TOC generation %ld - %d track(s), %d audio, first=%d last=%d",
           gTOCGeneration, gTOC.trackCount, gTOC.audioCount,
           gTOC.firstTrack, gTOC.lastTrack);
    {
        short i;
        for (i = 0; i < gTOC.trackCount; i++)
            CDLogf("    track %2d: %s  %02d:%02d:%02d  lba=%ld",
                   gTOC.track[i].number,
                   gTOC.track[i].isData ? "DATA " : "AUDIO",
                   gTOC.track[i].m, gTOC.track[i].s, gTOC.track[i].f,
                   gTOC.track[i].lba);
    }
    if (gTOC.audioCount == 0)
        CDLogf("    ⇒ no audio tracks on this disc; audio requests cannot be served");
}

OSErr CDPumpPlay(const unsigned char *csParam)
{
    SndDoubleBufferHeader2 h;
    long  a, b;
    OSErr err;
    int   i;

    if (gChan == NULL || gPCM == NULL) return notOpenErr;

    /* Before resolving anything: the disc may have been inserted or swapped since
     * the pump started. */
    EnsureTOC();

    if (!DecodePos(csParam, &a, &b)) {
        /* ★ Count it, loudly. The handler may have told the caller noErr on the
         * strength of us accepting this request, so a failure here is the one case
         * where the override turned a visible refusal into silence. If this counter
         * is ever non-zero the override is lying and every log says so. */
        if (gPub != NULL) gPub->playResolveFails++;
        CDLogf("  pump: could not resolve the requested position");
        CDLogf("  !! the caller may have been told noErr on the strength of this "
               "request. %ld unresolvable play(s) so far — that is silence with no "
               "error, and it must not stay above zero.",
               gPub != NULL ? gPub->playResolveFails : -1L);
        return paramErr;
    }
    CDLogf("  pump: play LBA %ld .. %ld (%ld sectors, %ld s)",
           a, b, b - a, (b - a) / kCDDASectorsPerSec);

    /* ★ A repeat request for the range already playing keeps playing.
     *
     * Restarting unconditionally is what made a mixed-mode disc silent even once the
     * TOC was right. The probe hunts for an accepted encoding, so when the driver
     * refuses them all it issues eight AudioPlays inside a tenth of a second; the
     * pump serviced every one by stopping, resetting and re-reading a two-second
     * pre-roll. At the measured 330 KB/s that pre-roll alone is about a second, so
     * playback never survived long enough to hear — sixteen plays across 21.8 s of
     * log, and the listener reported "silent, or maybe a very short buzz".
     *
     * This is not merely a probe artefact: a game that gets an error back and retries
     * generates the same pattern, and so does one that re-issues Play on a loop
     * boundary. Coalescing costs nothing when the range differs, which is the case
     * that must still restart (a real track change — phase B covers it). */
    if (gPlaying && !gPaused && a == gPlayStartLBA && b == gEndLBA) {
        CDLogf("  pump: already playing this exact range; continuing rather than "
               "restarting (a restart here would re-read the pre-roll and the music "
               "would never be heard)");
        if (gPub != NULL) gPub->playsCoalesced++;
        PublishCursor();
        return noErr;
    }

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
        Boolean startsInData = false;

        for (i = 0; i < gTOC.trackCount; i++) {
            if (gTOC.track[i].lba <= a &&
                (i + 1 >= gTOC.trackCount || gTOC.track[i + 1].lba > a)) {
                gCurTrack      = gTOC.track[i].number;
                gTrackStartLBA = gTOC.track[i].lba;
                startsInData   = gTOC.track[i].isData;
                break;
            }
        }

        /* ★ NEVER STREAM A DATA TRACK TO THE SPEAKERS.
         *
         * Until 2026-08-07 nothing stopped this, because the TOC parser could not
         * tell a data track from an audio one (it read the wrong nibble of the
         * control field). On Warcraft's disc the pump therefore played 261 MB of
         * program code as 16-bit PCM, and the tester heard buzzing.
         *
         * Now that data tracks are identified correctly this should be unreachable,
         * which is exactly why it is worth having: it is cheap, it protects someone's
         * ears and speakers from full-scale noise, and if a future disc or drive
         * confuses the parse again the log says so instead of the speakers. */
        if (startsInData) {
            CDLogf("  !! REFUSING TO PLAY: LBA %ld is inside track %d, which is a DATA "
                   "track. Streaming it would be full-scale noise, not music.",
                   a, gCurTrack);
            CDLogf("     A game asking for audio here means either the disc's TOC is "
                   "being misread, or the game asked for something impossible.");
            if (gPub != NULL) gPub->playResolveFails++;
            return paramErr;
        }
    }
    CDLogf("  pump: inside track %d (starts at LBA %ld)", gCurTrack, gTrackStartLBA);

    while ((gWriteOff - gReadOff) < gPCMBytes)
        if (RefillOnce() <= 0) break;
    CDLogf("  pump: pre-roll %ld bytes", gWriteOff);

    if (gWriteOff == 0) return ioErr;

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
    if (err != noErr) gPlaying = false;
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

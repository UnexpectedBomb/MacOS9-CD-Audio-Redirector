/*
 * CDAudioSpike — Phase 1: DAE → Sound Manager, end to end.
 *
 * WHAT PHASE 1 IS FOR
 * -------------------
 * Phase 0 proved the two halves separately: CD-DA sectors are readable straight
 * through .AppleCD (2352-byte block size + a driver-level PBRead), and the digital
 * output path works because iTunes uses it. This joins them. Deliver an audible
 * track through the Sound Manager, with no interception anywhere, and measure the
 * engine that Phase 2 will later drive from the patched driver Control entry.
 *
 * THREE QUESTIONS, ANSWERED IN ONE HARDWARE RUN
 * ---------------------------------------------
 * Hardware trips are expensive, so this deliberately answers everything a single
 * run can:
 *
 *   Q1  Does the Sound Manager accept CD-DA's little-endian PCM as-is, given
 *       'sowt' in SndDoubleBufferHeader2.dbhFormat? REVIEW.md §5 argued the byte
 *       swap FEASIBILITY §3 calls mandatory is avoidable. Rather than guess, this
 *       plays the SAME five seconds twice: stage A1 as 'sowt' with the bytes
 *       untouched, then stage A2 as 'twos' with the bytes swapped. Whichever one
 *       sounds like music is the answer, and the listener cannot get it wrong
 *       because the wrong one is unmistakable — wrong-endian 16-bit PCM is loud
 *       white noise, not subtly-off music.
 *
 *   Q2  Does the double-buffer engine work at all — channel setup, sample rate,
 *       stereo, the interrupt-time doubleback proc? Stage A answers this without
 *       any streaming: the whole five seconds is read into RAM first, so nothing
 *       depends on refilling in time. If stage A is clean, the engine is sound.
 *
 *   Q3  Does the ring buffer sustain playback from the drive in real time? Stage B
 *       streams 30 seconds through a 2-second ring refilled at task level, and
 *       counts underruns. Stutter means the ring or the read pattern needs work;
 *       silence-free playback means the Phase 2 engine can be built on this.
 *
 * Separating Q2 from Q3 matters: if streaming stutters, stage A still proves the
 * format and the engine, so the run is not wasted and the follow-up is narrow.
 *
 * ★ INTERRUPT-SAFETY, THE RECURRING LANDMINE ON THIS PROJECT
 * The doubleback proc runs BELOW TASK LEVEL. It does not allocate, does not touch
 * the File Manager, does not log, and never waits. On an underrun it emits silence
 * and bumps a counter. See reference_os9_no_filemgr_at_interrupt: this exact
 * mistake cost the USB2 work three hardware cycles.
 *
 * The ring is a single-producer/single-consumer design with NO shared mutable
 * counter: writeOff is written only at task level, readOff only at interrupt
 * level, each read by the other side. Available bytes are derived
 * (writeOff - readOff), so there is no read-modify-write race to lose. The
 * cursors are monotonic longs — good for 2 GB, and this plays at most a few MB.
 *
 * Also deliberate: the streaming refill uses a LEAN PBRead with no logging. The
 * shared CDStatusCall/CDControlCall wrappers flush the log on every call, which is
 * exactly right for a recon probe and would starve the ring here. Timings are
 * logged after the fact instead.
 *
 * Output: "CD Audio Spike Log" in the System Folder, appended — read from the LAST
 * banner. The window asks which stage sounded correct and records the answer.
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
#include <Sound.h>
#include <ToolUtils.h>

#include <stdio.h>
#include <string.h>

#include "cd_probe_common.h"
#include "cd_cscodes.h"

#define kVersionString  "CDAudioSpike v1"

/* ---- tunables ------------------------------------------------------------ */

#define kBytesPerFrame     4               /* 16-bit stereo                   */
#define kStageASeconds     5               /* preloaded, played twice         */
#define kStageBSeconds     30              /* streamed                        */
#define kRingSeconds       2               /* stage B read-ahead              */
#define kDBufFrames        11025           /* 0.25 s per double buffer        */
#define kReadChunkSectors  32              /* 75264 bytes per PBRead          */
#define kSkipSeconds       2               /* skip any silent pregap          */

#define kDBufBytes         (kDBufFrames * kBytesPerFrame)

/* ---- the stream: one engine, used by both stages -------------------------- */

typedef struct {
    unsigned char  *ring;
    long            ringBytes;

    /* SPSC cursors. writeOff: task level only. readOff: interrupt only. */
    volatile long   writeOff;
    volatile long   readOff;

    long            totalBytes;        /* how much to play in this stage      */
    volatile long   deliveredBytes;    /* handed to the Sound Manager         */
    volatile long   underruns;
    volatile long   callbacks;
    volatile Boolean done;
} Stream;

static Stream           gS;
static SndChannelPtr    gChan     = NULL;
static SndDoubleBufferPtr gDBuf[2] = { NULL, NULL };
static SndDoubleBackUPP gDBackUPP = NULL;

/* CD geometry for this run */
static CDDriverInfo     gCD;
static CDTOC            gTOC;
static long             gTrackLBA   = 0;
static short            gSavedBlockSize = 512;

/* results */
static OSErr            gStageAErr[2] = { noErr, noErr };
static long             gStageAUnderruns[2] = { 0, 0 };
static OSErr            gStageBErr    = noErr;
static long             gStageBUnderruns = 0;
static long             gReadKBPerSec = 0;
static Boolean          gStageARan[2] = { false, false };
static Boolean          gStageBRan    = false;

/* ---- lean sector reader (no logging: called inside the refill loop) ------- */

/* Driver-level read of CD-DA sectors at a 2352-byte block size. Returns the byte
 * count actually read, 0 on error, and reports the OSErr through *errOut. */
static long ReadSectorsRaw(long lba, void *dest, long sectors, OSErr *errOut)
{
    ParamBlockRec pb;
    OSErr         err;

    memset(&pb, 0, sizeof(pb));
    pb.ioParam.ioRefNum    = gCD.refNum;
    pb.ioParam.ioVRefNum   = gCD.driveNum;
    pb.ioParam.ioBuffer    = (Ptr)dest;
    pb.ioParam.ioReqCount  = sectors * kCDDASectorBytes;
    pb.ioParam.ioPosMode   = fsFromStart;
    pb.ioParam.ioPosOffset = lba * kCDDASectorBytes;

    err = PBReadSync(&pb);
    if (errOut) *errOut = err;
    if (err != noErr) return 0;
    return pb.ioParam.ioActCount;
}

/* ---- the doubleback proc: INTERRUPT LEVEL -------------------------------- *
 * No allocation, no File Manager, no logging, no waiting. Context arrives through
 * dbUserInfo[0] rather than fragment globals, which is the same discipline the VBL
 * probe used for its interrupt tasks. */

static pascal void DoubleBackProc(SndChannelPtr chan, SndDoubleBufferPtr buf)
{
    Stream *s = (Stream *)buf->dbUserInfo[0];
    long    want, remaining, have;

    (void)chan;

    remaining = s->totalBytes - s->deliveredBytes;
    if (remaining <= 0) {
        buf->dbNumFrames = 0;
        buf->dbFlags     = dbBufferReady | dbLastBuffer;
        s->done          = true;
        return;
    }

    want = kDBufBytes;
    if (want > remaining) want = remaining;

    have = s->writeOff - s->readOff;
    if (have >= want) {
        long idx  = s->readOff % s->ringBytes;
        long run1 = s->ringBytes - idx;
        if (run1 >= want) {
            BlockMoveData(s->ring + idx, buf->dbSoundData, want);
        } else {
            BlockMoveData(s->ring + idx, buf->dbSoundData, run1);
            BlockMoveData(s->ring, buf->dbSoundData + run1, want - run1);
        }
        s->readOff += want;
    } else {
        /* Underrun. Emit silence, count it, and carry on — never wait, never
         * block, never allocate. readOff is deliberately NOT advanced, so the
         * audio resumes where it left off and the symptom is an audible gap
         * rather than a skip. */
        memset(buf->dbSoundData, 0, want);
        s->underruns++;
    }

    s->deliveredBytes += want;
    s->callbacks++;

    buf->dbNumFrames = want / kBytesPerFrame;
    buf->dbFlags     = dbBufferReady;
    if (s->deliveredBytes >= s->totalBytes) {
        buf->dbFlags |= dbLastBuffer;
        s->done       = true;
    }
}

/* ---- channel plumbing ---------------------------------------------------- */

static OSErr OpenChannel(void)
{
    OSErr err;

    gChan = NULL;
    err = SndNewChannel(&gChan, sampledSynth, initStereo, NULL);
    CDLogf("  SndNewChannel(sampledSynth, initStereo) err=%d chan=0x%08lX",
           err, (unsigned long)gChan);
    return err;
}

static void CloseChannel(void)
{
    if (gChan != NULL) {
        OSErr err = SndDisposeChannel(gChan, true);   /* true = quiet now */
        CDLogf("  SndDisposeChannel err=%d", err);
        gChan = NULL;
    }
}

static OSErr AllocDoubleBuffers(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (gDBuf[i] != NULL) continue;
        gDBuf[i] = (SndDoubleBufferPtr)
            NewPtrClear((Size)(sizeof(SndDoubleBuffer) + kDBufBytes));
        if (gDBuf[i] == NULL) {
            CDLogf("  !! could not allocate double buffer %d (%ld bytes)",
                   i, (long)(sizeof(SndDoubleBuffer) + kDBufBytes));
            return memFullErr;
        }
    }
    return noErr;
}

/* Fill one double buffer at TASK level, before playback starts. Same logic as the
 * interrupt-time proc, deliberately reused so the pre-roll cannot diverge from the
 * steady state. */
static void PrimeBuffer(SndDoubleBufferPtr buf)
{
    buf->dbUserInfo[0] = (long)&gS;
    buf->dbUserInfo[1] = 0;
    buf->dbFlags       = 0;
    DoubleBackProc(gChan, buf);
}

/* Play whatever is in the stream, with the given Sound Manager format.
 * compressionID is tried as notCompressed first and fixedCompression second,
 * because which one a header2 with dbhFormat wants is not something to guess on a
 * machine that costs a trip to reach. */
static OSErr PlayStream(OSType format, const char *label)
{
    SndDoubleBufferHeader2 h;
    OSErr  err;
    short  attempt;
    long   startTicks, guardTicks;

    if (AllocDoubleBuffers() != noErr) return memFullErr;

    for (attempt = 0; attempt < 2; attempt++) {
        short compID = (attempt == 0) ? notCompressed : fixedCompression;

        /* reset the consumer side for this attempt */
        gS.readOff        = 0;
        gS.deliveredBytes = 0;
        gS.underruns      = 0;
        gS.callbacks      = 0;
        gS.done           = false;

        PrimeBuffer(gDBuf[0]);
        PrimeBuffer(gDBuf[1]);

        memset(&h, 0, sizeof(h));
        h.dbhNumChannels   = 2;
        h.dbhSampleSize    = 16;
        h.dbhCompressionID = compID;
        h.dbhPacketSize    = 0;
        h.dbhSampleRate    = rate44khz;
        h.dbhBufferPtr[0]  = gDBuf[0];
        h.dbhBufferPtr[1]  = gDBuf[1];
        h.dbhDoubleBack    = gDBackUPP;
        h.dbhFormat        = format;

        CDLogf("  %s: SndPlayDoubleBuffer format='%.4s' compressionID=%d "
               "rate=0x%08lX bufFrames=%d",
               label, (char *)&format, compID,
               (unsigned long)rate44khz, kDBufFrames);
        CDProgressSay("%s: playing ('%.4s', compID %d)",
                      label, (char *)&format, compID);

        err = SndPlayDoubleBuffer(gChan, (SndDoubleBufferHeaderPtr)&h);
        CDLogf("  %s: SndPlayDoubleBuffer err=%d", label, err);
        if (err == noErr) break;

        CDLogf("  %s: retrying with the other compressionID", label);
    }

    if (err != noErr) return err;

    /* Wait for the doubleback proc to report the last buffer. If
     * SndPlayDoubleBuffer turned out to be synchronous this loop simply finds
     * done already set, which is why the ring is pre-filled: stage A can never
     * depend on getting task time back. */
    startTicks = TickCount();
    guardTicks = (long)(kStageBSeconds + 20) * 60;
    while (!gS.done) {
        EventRecord evt;
        if (TickCount() - startTicks > guardTicks) {
            CDLogf("  %s: TIMED OUT waiting for completion "
                   "(delivered=%ld/%ld callbacks=%ld)",
                   label, gS.deliveredBytes, gS.totalBytes, gS.callbacks);
            break;
        }
        (void)WaitNextEvent(0, &evt, 1, NULL);
    }

    CDLogf("  %s: finished. delivered=%ld/%ld bytes callbacks=%ld underruns=%ld",
           label, gS.deliveredBytes, gS.totalBytes, gS.callbacks, gS.underruns);
    return noErr;
}

/* ---- stage A: preloaded, played twice with opposite byte order ------------ */

/* Byte-swap 16-bit samples in place. Used between A1 and A2 so only one copy of
 * the audio has to be held in memory. */
static void SwapInPlace(unsigned char *p, long bytes)
{
    long i;
    for (i = 0; i + 1 < bytes; i += 2) {
        unsigned char t = p[i];
        p[i]     = p[i + 1];
        p[i + 1] = t;
    }
}

static void StageA(void)
{
    long  bytes   = (long)kStageASeconds * kCDDABytesPerSec;
    long  sectors = bytes / kCDDASectorBytes;
    long  got, t0, t1, elapsed;
    OSErr err = noErr;

    CDLogf("=== STAGE A: %d s preloaded into RAM, played twice ===",
           kStageASeconds);
    CDLogf("  Q1: which byte order does the Sound Manager want?");
    CDLogf("  Q2: does the double-buffer engine work at all?");

    bytes = sectors * kCDDASectorBytes;    /* whole sectors only */

    gS.ring      = (unsigned char *)NewPtr(bytes);
    if (gS.ring == NULL) {
        CDLogf("  !! could not allocate %ld bytes for the stage A buffer", bytes);
        gStageAErr[0] = memFullErr;
        return;
    }
    gS.ringBytes = bytes;

    /* Read it all up front, in chunks, and time it. The throughput figure is what
     * Phase 2 needs in order to size its read-ahead. */
    CDProgressSay("stage A: reading %ld KB from the CD", bytes / 1024);
    t0  = TickCount();
    got = 0;
    while (got < bytes) {
        long chunk = kReadChunkSectors;
        long want  = bytes - got;
        if (chunk * kCDDASectorBytes > want)
            chunk = want / kCDDASectorBytes;
        if (chunk <= 0) break;

        {
            long n = ReadSectorsRaw(gTrackLBA + got / kCDDASectorBytes,
                                    gS.ring + got, chunk, &err);
            if (n <= 0) {
                CDLogf("  !! read failed at offset %ld err=%d", got, err);
                break;
            }
            got += n;
        }
    }
    t1      = TickCount();
    elapsed = t1 - t0;
    if (elapsed <= 0) elapsed = 1;
    gReadKBPerSec = (got / 1024) * 60 / elapsed;
    CDLogf("  read %ld of %ld bytes in %ld ticks = %ld KB/s "
           "(CD-DA needs 172 KB/s to keep up)",
           got, bytes, elapsed, gReadKBPerSec);
    CDProgressSay("read %ld KB at %ld KB/s (need 172)", got / 1024,
                  gReadKBPerSec);

    if (got < bytes) {
        CDLogf("  !! short read; playing only what arrived");
        gS.ringBytes = got > 0 ? got : 1;
    }

    /* The whole buffer is already present, so the producer cursor is final. */
    gS.writeOff   = got;
    gS.totalBytes = got;

    /* --- A1: 'sowt', bytes exactly as they came off the disc --- */
    CDLogf("--- A1: 'sowt' (k16BitLittleEndianFormat), NO byte swap ---");
    gStageAErr[0]       = PlayStream(k16BitLittleEndianFormat, "A1 sowt");
    gStageAUnderruns[0] = gS.underruns;
    gStageARan[0]       = (gStageAErr[0] == noErr);

    /* a beat of quiet so the two stages are distinguishable by ear */
    {
        long until = TickCount() + 90;
        EventRecord evt;
        while (TickCount() < until) (void)WaitNextEvent(0, &evt, 1, NULL);
    }

    /* --- A2: 'twos', same audio byte-swapped --- */
    CDLogf("--- A2: 'twos' (k16BitBigEndianFormat), bytes SWAPPED ---");
    CDProgressSay("stage A2: swapping bytes for the 'twos' comparison");
    SwapInPlace(gS.ring, gS.ringBytes);
    gS.writeOff         = got;      /* producer cursor unchanged; data rewritten */
    gStageAErr[1]       = PlayStream(k16BitBigEndianFormat, "A2 twos");
    gStageAUnderruns[1] = gS.underruns;
    gStageARan[1]       = (gStageAErr[1] == noErr);

    DisposePtr((Ptr)gS.ring);
    gS.ring = NULL;
}

/* ---- stage B: streamed through a ring, refilled at task level ------------- */

static void StageB(void)
{
    long  ringBytes  = (long)kRingSeconds * kCDDABytesPerSec;
    long  totalBytes = (long)kStageBSeconds * kCDDABytesPerSec;
    long  nextLBA, t0;
    OSErr err = noErr;

    CDLogf("=== STAGE B: %d s streamed through a %d s ring ===",
           kStageBSeconds, kRingSeconds);
    CDLogf("  Q3: can task-level refill keep the interrupt-level consumer fed?");

    ringBytes  = (ringBytes / kCDDASectorBytes) * kCDDASectorBytes;
    totalBytes = (totalBytes / kCDDASectorBytes) * kCDDASectorBytes;

    gS.ring = (unsigned char *)NewPtr(ringBytes);
    if (gS.ring == NULL) {
        CDLogf("  !! could not allocate a %ld byte ring", ringBytes);
        gStageBErr = memFullErr;
        return;
    }
    gS.ringBytes  = ringBytes;
    gS.writeOff   = 0;
    gS.readOff    = 0;
    gS.totalBytes = totalBytes;
    nextLBA       = gTrackLBA;

    /* Pre-roll: fill the ring completely before starting, so playback begins with
     * a full read-ahead rather than racing from empty. */
    CDProgressSay("stage B: pre-filling the %ld KB ring", ringBytes / 1024);
    while (gS.writeOff < ringBytes) {
        /* During pre-roll writeOff only ever runs 0..ringBytes, so the free space
         * and the contiguous run to the end of the ring are the same number and
         * one clamp is enough. The steady-state loop below needs both. */
        long space = ringBytes - gS.writeOff;
        long chunk = kReadChunkSectors;
        long n;
        if (chunk * kCDDASectorBytes > space) chunk = space / kCDDASectorBytes;
        if (chunk <= 0) break;
        n = ReadSectorsRaw(nextLBA, gS.ring + (gS.writeOff % ringBytes),
                           chunk, &err);
        if (n <= 0) {
            CDLogf("  !! pre-roll read failed err=%d", err);
            gStageBErr = err;
            DisposePtr((Ptr)gS.ring);
            gS.ring = NULL;
            return;
        }
        gS.writeOff += n;
        nextLBA     += n / kCDDASectorBytes;
    }
    CDLogf("  pre-roll complete: %ld bytes, next LBA %ld", gS.writeOff, nextLBA);

    /* Start playback, then keep the ring topped up from task level. Note the lean
     * reader: no logging inside this loop, because a FlushVol per call would
     * starve the very thing being measured. */
    {
        SndDoubleBufferHeader2 h;

        gS.deliveredBytes = 0;
        gS.underruns      = 0;
        gS.callbacks      = 0;
        gS.done           = false;

        /* The consumer starts at 0 and the producer is already ahead of it. */
        gS.readOff = 0;

        PrimeBuffer(gDBuf[0]);
        PrimeBuffer(gDBuf[1]);

        memset(&h, 0, sizeof(h));
        h.dbhNumChannels   = 2;
        h.dbhSampleSize    = 16;
        h.dbhCompressionID = notCompressed;
        h.dbhPacketSize    = 0;
        h.dbhSampleRate    = rate44khz;
        h.dbhBufferPtr[0]  = gDBuf[0];
        h.dbhBufferPtr[1]  = gDBuf[1];
        h.dbhDoubleBack    = gDBackUPP;
        h.dbhFormat        = k16BitLittleEndianFormat;

        CDLogf("  starting streamed playback ('sowt')");
        CDProgressSay("stage B: streaming %d s - listen for gaps",
                      kStageBSeconds);
        gStageBErr = SndPlayDoubleBuffer(gChan, (SndDoubleBufferHeaderPtr)&h);
        CDLogf("  SndPlayDoubleBuffer err=%d", gStageBErr);
        if (gStageBErr != noErr) {
            DisposePtr((Ptr)gS.ring);
            gS.ring = NULL;
            return;
        }

        t0 = TickCount();
        while (!gS.done) {
            long consumed = gS.readOff;              /* interrupt side */
            long space    = ringBytes - (gS.writeOff - consumed);
            long idx      = gS.writeOff % ringBytes;
            long contig   = ringBytes - idx;          /* room before the wrap */
            long chunk    = kReadChunkSectors;

            if (TickCount() - t0 > (long)(kStageBSeconds + 25) * 60) {
                CDLogf("  TIMED OUT (delivered=%ld/%ld)",
                       gS.deliveredBytes, gS.totalBytes);
                break;
            }

            /* Clamp the read to the free space AND to the contiguous run before
             * the end of the ring. Missing that second clamp writes past the end
             * of the allocation — a PBRead straight into the heap. Both ringBytes
             * and every write are whole multiples of 2352, so idx stays
             * sector-aligned and contig is never a partial sector. */
            if (chunk * kCDDASectorBytes > space)
                chunk = space / kCDDASectorBytes;
            if (chunk * kCDDASectorBytes > contig)
                chunk = contig / kCDDASectorBytes;

            if (chunk > 0 && gS.writeOff < gS.totalBytes) {
                long n = ReadSectorsRaw(nextLBA, gS.ring + idx, chunk, &err);
                if (n <= 0) {
                    CDLogf("  refill read failed at LBA %ld err=%d",
                           nextLBA, err);
                    break;
                }
                gS.writeOff += n;
                nextLBA     += n / kCDDASectorBytes;
            } else {
                EventRecord evt;
                (void)WaitNextEvent(0, &evt, 1, NULL);
            }
        }

        gStageBUnderruns = gS.underruns;
        gStageBRan       = true;
        CDLogf("  stage B done: delivered=%ld/%ld callbacks=%ld UNDERRUNS=%ld",
               gS.deliveredBytes, gS.totalBytes, gS.callbacks, gS.underruns);
        CDProgressSay("stage B done: %ld underruns", gS.underruns);
    }

    DisposePtr((Ptr)gS.ring);
    gS.ring = NULL;
}

/* ---- the listener question ----------------------------------------------- */

static void DrawAt(short v, const char *s)
{
    Str255 p;
    int    n = (int)strlen(s);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    BlockMoveData(s, p + 1, n);
    MoveTo(12, v);
    DrawString(p);
}

/* Returns the index of the button pressed, or -1 on escape. */
static short AskWhichSounded(const char *lines[], int nLines,
                             const char *labels[], int nButtons)
{
    WindowPtr     win;
    Rect          bounds, r;
    ControlHandle ctl[4];
    EventRecord   evt;
    short         v = 22;
    short         answer = -1;
    int           i;

    SetRect(&bounds, 30, 50, 30 + 600, 50 + 26 + nLines * 14 + 60);
    win = NewWindow(NULL, &bounds, "\p" kVersionString, true, documentProc,
                    (WindowPtr)-1L, false, 0);
    if (win == NULL) return -1;
    SetPort(win);
    TextFont(kFontIDGeneva);
    TextSize(9);

    for (i = 0; i < nLines; i++) { DrawAt(v, lines[i]); v += 14; }
    v += 8;

    for (i = 0; i < nButtons && i < 4; i++) {
        Str255 lbl;
        int    n = (int)strlen(labels[i]);
        if (n > 255) n = 255;
        lbl[0] = (unsigned char)n;
        BlockMoveData(labels[i], lbl + 1, n);
        SetRect(&r, 20 + i * 145, v, 20 + i * 145 + 135, v + 20);
        ctl[i] = NewControl(win, &r, lbl, true, 0, 0, 1, pushButProc, 0);
    }
    DrawControls(win);

    while (answer < 0) {
        if (WaitNextEvent(mDownMask | keyDownMask, &evt, 10, NULL)) {
            if (evt.what == mouseDown) {
                Point         p = evt.where;
                ControlHandle which;
                GlobalToLocal(&p);
                if (FindControl(p, win, &which) && TrackControl(which, p, NULL)) {
                    for (i = 0; i < nButtons && i < 4; i++)
                        if (which == ctl[i]) answer = (short)i;
                }
            } else if (evt.what == keyDown) {
                char c = evt.message & charCodeMask;
                if (c >= '1' && c < '1' + nButtons) answer = (short)(c - '1');
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
    Boolean logOK;
    short   a1, a2;
    int     t;
    OSErr   err;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    memset(&gS, 0, sizeof(gS));
    memset(&gCD, 0, sizeof(gCD));

    CDProgressOpen("\p" kVersionString " - progress");
    CDProgressSay("%s starting", kVersionString);

    logOK = CDLogOpen("\pCD Audio Spike Log");
    if (!logOK) CDProgressSay("!! could not open the log - screen only");
    CDLogBanner(kVersionString " - Phase 1: DAE to Sound Manager",
                "reads CD-DA and plays it. No interception anywhere.");

    /* --- locate the drive and the audio --- */
    CDFindDriver(&gCD, false);        /* drive queue only; the sweep can hang */
    if (!gCD.found) {
        CDLogf("no CD driver; nothing to do");
        CDProgressSay("NO CD DRIVER FOUND");
        goto done;
    }
    if (gCD.driveNum == 0) {
        CDLogf("no drive number for refNum=%d; cannot issue driver reads",
               gCD.refNum);
        goto done;
    }

    CDReadTOC(gCD.refNum, &gTOC);
    if (!gTOC.valid || gTOC.audioCount == 0) {
        CDLogf("no audio track found. Put an audio CD in the drive.");
        CDProgressSay("NO AUDIO TRACK - insert an audio CD");
        goto done;
    }
    for (t = 0; t < gTOC.trackCount; t++) {
        if (!gTOC.track[t].isData) {
            gTrackLBA = gTOC.track[t].lba + kSkipSeconds * kCDDASectorsPerSec;
            CDLogf("playing from track %d, LBA %ld (track start %ld + %d s)",
                   gTOC.track[t].number, gTrackLBA, gTOC.track[t].lba,
                   kSkipSeconds);
            break;
        }
    }

    /* --- 2352-byte blocks, the route Phase 0 proved --- */
    {
        short param[11];
        if (CDStatusCall(gCD.refNum, kcsGetBlockSize, param,
                         sizeof(param)) == noErr)
            gSavedBlockSize = param[0];
        CDLogf("saved block size = %d", gSavedBlockSize);

        memset(param, 0, sizeof(param));
        param[0] = kCDDASectorBytes;
        err = CDControlCall(gCD.refNum, kcsChangeBlockSize, param,
                            sizeof(param), NULL, 0);
        CDLogf("ChangeBlockSize(2352) err=%d", err);
        if (err != noErr) {
            CDLogf("!! cannot switch to 2352-byte blocks; Phase 0 said this "
                   "works, so something differs about this disc or drive state");
            CDProgressSay("ChangeBlockSize(2352) FAILED err=%d", err);
            goto done;
        }
    }

    /* --- the engine --- */
    gDBackUPP = NewSndDoubleBackUPP(DoubleBackProc);
    if (gDBackUPP == NULL) {
        CDLogf("!! NewSndDoubleBackUPP failed");
        goto restore;
    }
    if (OpenChannel() != noErr) goto restore;

    StageA();
    StageB();

    CloseChannel();

restore:
    {
        short param[11];
        memset(param, 0, sizeof(param));
        param[0] = gSavedBlockSize;
        err = CDControlCall(gCD.refNum, kcsChangeBlockSize, param,
                            sizeof(param), NULL, 0);
        CDLogf("ChangeBlockSize(%d) restore err=%d", gSavedBlockSize, err);
        if (err != noErr)
            CDLogf("!! block size NOT restored; eject and re-insert the disc");
    }

done:
    {
        int i;
        for (i = 0; i < 2; i++)
            if (gDBuf[i] != NULL) { DisposePtr((Ptr)gDBuf[i]); gDBuf[i] = NULL; }
    }
    if (gDBackUPP != NULL) { DisposeSndDoubleBackUPP(gDBackUPP); gDBackUPP = NULL; }

    CDLogFlush();
    CDProgressSay("done - answer the questions");
    CDProgressClose();

    /* --- Q1: which byte order --- */
    if (gStageARan[0] || gStageARan[1]) {
        static const char *lines[] = {
            "Stage A played the SAME five seconds twice:",
            "",
            "   FIRST  = 'sowt', bytes straight off the disc (no swap)",
            "   SECOND = 'twos', bytes swapped",
            "",
            "One of them should be music. The other should be obvious",
            "loud noise or static - wrong-endian PCM is unmistakable.",
            "",
            "Which one sounded like music?"
        };
        static const char *labels[] = { "First (sowt)", "Second (twos)",
                                        "Both", "Neither" };
        a1 = AskWhichSounded(lines, 9, labels, 4);
    } else {
        a1 = -1;
    }

    /* --- Q3: did the streamed stage hold up --- */
    if (gStageBRan) {
        static const char *lines[] = {
            "Stage B streamed 30 seconds from the drive through a",
            "2-second ring buffer, refilled at task level.",
            "",
            "Gaps or stutters mean the ring ran dry (underruns are",
            "counted in the log). Continuous music means the engine",
            "can carry Phase 2.",
            "",
            "How did the long stage sound?"
        };
        static const char *labels[] = { "Continuous", "Occasional gaps",
                                        "Badly broken", "Silent" };
        a2 = AskWhichSounded(lines, 8, labels, 4);
    } else {
        a2 = -1;
    }

    /* --- record the verdicts --- *
     * The log is still open here: only CDLogFlush was called above, not
     * CDLogClose. v1 re-opened it "to be safe", which was silently fatal — the
     * second FSpOpenDF on a file already open for writing fails with opWrErr, so
     * gLogRef went to 0 and every line below was dropped. The whole point of the
     * run, the listener's answers, went missing. CDLogOpen is idempotent now, and
     * this call is gone. */
    CDLogf("--- listener verdicts ---");
    {
        static const char *byteOrder[] = { "FIRST: 'sowt', no swap",
                                           "SECOND: 'twos', swapped",
                                           "BOTH sounded like music",
                                           "NEITHER sounded like music" };
        static const char *quality[]   = { "continuous", "occasional gaps",
                                           "badly broken", "silent" };
        CDLogf("  Q1 byte order: %s",
               (a1 >= 0 && a1 < 4) ? byteOrder[a1] : "no answer");
        CDLogf("  Q3 stream quality: %s",
               (a2 >= 0 && a2 < 4) ? quality[a2] : "no answer");

        if (a1 == 0)
            CDLogf("  ⇒ 'sowt' CONFIRMED: the Sound Manager takes CD-DA as-is. "
                   "No byte swap in the engine, so the doubleback proc stays a "
                   "plain copy. REVIEW.md §5 holds.");
        if (a1 == 1)
            CDLogf("  ⇒ the swap IS required: the Sound Manager ignored "
                   "dbhFormat='sowt'. Phase 2 must swap LE->BE in the "
                   "doubleback proc, as FEASIBILITY §3 assumed.");
        if (a1 == 2)
            CDLogf("  ⇒ both audible: suspicious. Either dbhFormat is being "
                   "ignored in a way that happens to be inaudible, or the swap "
                   "did not happen. Check the A2 log lines before trusting this.");
        if (a1 == 3)
            CDLogf("  ⇒ neither audible: the engine, the channel or the output "
                   "routing is wrong, not the byte order. Check "
                   "SndPlayDoubleBuffer errs and callback counts above.");

        if (a2 == 0 && gStageBUnderruns == 0)
            CDLogf("  ⇒ STREAMING PROVEN: %d s with zero underruns at %ld KB/s "
                   "read throughput. A %d s ring is enough; Phase 2's engine can "
                   "be built on this.", kStageBSeconds, gReadKBPerSec,
                   kRingSeconds);
        if (a2 > 0 || gStageBUnderruns > 0)
            CDLogf("  ⇒ streaming is marginal: %ld underruns. Options in order of "
                   "cheapness: a bigger ring, larger reads per call, or the "
                   "rip-ahead-to-disk design in REVIEW.md §4.",
                   gStageBUnderruns);
    }
    CDLogf("=== end of run: readKBs=%ld A1err=%d A2err=%d Berr=%d "
           "Bunderruns=%ld q1=%d q3=%d",
           gReadKBPerSec, gStageAErr[0], gStageAErr[1], gStageBErr,
           gStageBUnderruns, a1, a2);
    CDLogClose();

    return 0;
}

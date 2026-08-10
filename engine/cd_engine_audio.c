/*
 * cd_engine_audio.c — Step 5a: the audio engine, inside the resident PowerPC engine.
 *
 * ⛔ RETIRED. THIS FILE IS NOT BUILT. Nothing references it, including
 * engine/CMakeLists.txt. The design it describes — doing the audio inside the
 * resident engine, from within the patched Control entry — was killed on hardware
 * by the deadlock: you cannot do synchronous driver I/O from inside that driver's
 * own Control entry, because the read cannot start until the Control call returns
 * and the call is waiting on the read. `accRun` is no escape, being a Control call
 * itself. That is why the pump application exists, and the shipping path is
 * engine/cd_pump_audio.c.
 *
 * Kept for the same reason the 68K generation in patch/ is kept: it records what
 * was tried and why it could not work. Read it as history, not as a component.
 * ⚠ If any of it is ever revived, note that it carries its OWN copy of the TOC
 * parse, separate from the one in probes/common/cd_probe_common.c that everything
 * shipping uses. That copy had the control-nibble bug for the same reason the real
 * one did; it has been corrected in place so this file cannot reintroduce it, but a
 * second parser is a second thing to get wrong.
 *
 * This is the payload the whole project has been building towards: when a game issues
 * the legacy `AudioPlay`, read the CD-DA sectors ourselves and play them through the
 * Sound Manager, so the music is audible on a Mac with no analog CD-audio wire.
 *
 * The playback engine itself is not new. Phase 1 proved it on this exact hardware:
 * `ChangeBlockSize(2352)` + driver-level `PBRead` for DAE, `SndPlayDoubleBuffer` with
 * `'sowt'` and `compressionID = notCompressed`, 0.25 s double buffers, a 2-second ring,
 * 30 seconds streamed with zero underruns. What is new is being driven from inside a
 * patched driver Control entry instead of from an application's main loop.
 *
 * ★ SIDE EFFECTS ONLY — WE ALWAYS CHAIN
 * For every csCode, including the ones we act on, the original driver is still called
 * and its result is still returned. We add audio; we never take responsibility for
 * completing a request. That matters because Phase 2a measured `AudioStatus` and `ReadQ`
 * arriving QUEUED, and a queued request must end at `jIODone` rather than simply
 * returning — a completion protocol we would otherwise have to reproduce from PowerPC.
 * By always chaining, the original keeps doing whatever it already does correctly, and
 * the transport csCodes are ones it "accepts and ignores" anyway, so nothing is lost.
 *
 * ⇒ Consequence: Step 5a does NOT synthesise AudioStatus/ReadQ, so a game that polls for
 * track end still sees a frozen position. Music will play; looping will not yet work.
 * That is Step 5b, and it needs one fact this run will supply: whether chaining a QUEUED
 * call returns to us at all. If it does, 5b is chain-then-rewrite-csParam. If it does
 * not, 5b needs a real completion path.
 *
 * ★ RE-ENTRANCY
 * The engine has to talk to the very driver whose Control entry it has patched — TOC
 * reads, block-size changes. Those re-enter our own handler. `gInSelfCall` makes the
 * handler pass its own traffic straight through without interpreting it.
 *
 * ★ BLOCK SIZE IS RESTORED AROUND EVERY READ
 * A 2352-byte block size is wrong for data reads, so leaving it set for the duration of
 * playback would break a game reading level data from track 1. Each refill sets 2352,
 * reads, and restores. A data read landing inside that window would still see the wrong
 * size; that race is documented rather than solved, and would need the ATA route
 * ('dvrf') to remove entirely.
 *
 * ★ INTERRUPT SAFETY
 * The doubleback proc runs below task level: plain copies out of a pre-allocated ring,
 * no allocation, no File Manager, no waiting, silence on underrun. Everything that
 * allocates or issues I/O happens at task level — install time, `AudioPlay`, or `accRun`.
 */

#include <MacTypes.h>
#include <Devices.h>
#include <Files.h>
#include <MacMemory.h>
#include <Sound.h>
#include <MixedMode.h>

#include "cd_engine.h"
#include "cd_cscodes.h"

#define LM_Ticks    (*(volatile unsigned long *)0x016A)

/* Phase-1 proven numbers. */
#define kDBufFrames     11025                       /* 0.25 s per double buffer */
#define kDBufBytes      (kDBufFrames * 4)
#define kRingSeconds    2
#define kChunkSectors   32                          /* 75264 bytes per read     */
#define kRefillDelay    6                           /* ticks: ~10 accRun/s      */

/* ---- state ---------------------------------------------------------------- */

static short              gRefNum;          /* the CD driver                   */
static short              gDriveNum;        /* for driver-level reads          */

static SndChannelPtr      gChan     = NULL;
static SndDoubleBufferPtr gDBuf[2]  = { NULL, NULL };
static SndDoubleBackUPP   gDBackUPP = NULL;

static unsigned char     *gPCM      = NULL; /* the audio ring                  */
static long               gPCMBytes = 0;
static volatile long      gReadOff  = 0;    /* consumer: interrupt only        */
static volatile long      gWriteOff = 0;    /* producer: task level only       */

static volatile Boolean   gPlaying  = false;
static volatile Boolean   gPaused   = false;
static volatile long      gUnderruns = 0;
static long               gNextLBA  = 0;
static long               gEndLBA   = 0;

static short              gSavedBlockSize = 512;
static short              gSavedDelay     = 120;
static Boolean            gDelayShortened = false;

static unsigned char      gVolL = 255, gVolR = 255;

static Boolean            gInSelfCall = false;

/* TOC, read once at install so AudioPlay does not have to. */
#define kMaxTracks 100
static struct {
    Boolean valid;
    short   firstTrack, lastTrack, count;
    struct { short number; Boolean isData; long lba; } t[kMaxTracks];
} gTOC;

Boolean CDAudioInSelfCall(void) { return gInSelfCall; }

/* ---- self-directed I/O to the patched driver ------------------------------- */

static OSErr SelfControl(short csCode, const void *param, long len)
{
    CntrlParam pb;
    OSErr      err;
    long       i;

    for (i = 0; i < (long)sizeof(pb); i++) ((char *)&pb)[i] = 0;
    pb.ioCRefNum = gRefNum;
    pb.csCode    = csCode;
    if (param != NULL) {
        if (len > 22) len = 22;
        BlockMoveData(param, pb.csParam, len);
    }
    gInSelfCall = true;
    err = PBControlSync((ParmBlkPtr)&pb);
    gInSelfCall = false;
    return err;
}

static long SelfReadSectors(long lba, void *dest, long sectors)
{
    ParamBlockRec pb;
    OSErr         err;
    long          i;

    for (i = 0; i < (long)sizeof(pb); i++) ((char *)&pb)[i] = 0;
    pb.ioParam.ioRefNum    = gRefNum;
    pb.ioParam.ioVRefNum   = gDriveNum;
    pb.ioParam.ioBuffer    = (Ptr)dest;
    pb.ioParam.ioReqCount  = sectors * kCDDASectorBytes;
    pb.ioParam.ioPosMode   = fsFromStart;
    pb.ioParam.ioPosOffset = lba * kCDDASectorBytes;

    gInSelfCall = true;
    err = PBReadSync(&pb);
    gInSelfCall = false;
    if (err != noErr) return 0;
    return pb.ioParam.ioActCount;
}

static void SetBlockSize(short size)
{
    short p[11];
    long  i;
    for (i = 0; i < 11; i++) p[i] = 0;
    p[0] = size;
    (void)SelfControl(kcsChangeBlockSize, p, sizeof(p));
}

/* ---- TOC, read once at install -------------------------------------------- */

static void ReadTOC(void)
{
    short          buf[11];
    unsigned char *p = (unsigned char *)buf;
    short          param[11];
    long           i;

    gTOC.valid = false;
    gTOC.count = 0;

    /* SelfControl does not hand csParam back, so the calls that need a RESULT use a
     * local parameter block directly rather than issuing the request twice. */
    (void)param;
    {
        CntrlParam pb;
        for (i = 0; i < (long)sizeof(pb); i++) ((char *)&pb)[i] = 0;
        pb.ioCRefNum = gRefNum;
        pb.csCode    = kcsReadTOC;
        pb.csParam[0] = kTOCActionFirstLast;
        gInSelfCall = true;
        if (PBControlSync((ParmBlkPtr)&pb) != noErr) { gInSelfCall = false; return; }
        gInSelfCall = false;
        p = (unsigned char *)pb.csParam;
        gTOC.firstTrack = kBCDToBin(p[0]);
        gTOC.lastTrack  = kBCDToBin(p[1]);
    }
    if (gTOC.firstTrack < 1 || gTOC.lastTrack < gTOC.firstTrack) return;

    /* Per-track addresses. csParam+2 = buffer, +6 = size (word), +8 = start track. */
    {
        short          n = gTOC.lastTrack - gTOC.firstTrack + 1;
        unsigned char *tb;
        CntrlParam     pb;

        if (n > kMaxTracks) n = kMaxTracks;
        tb = (unsigned char *)NewPtrSysClear((Size)(4 * n));
        if (tb == NULL) return;

        for (i = 0; i < (long)sizeof(pb); i++) ((char *)&pb)[i] = 0;
        pb.ioCRefNum = gRefNum;
        pb.csCode    = kcsReadTOC;
        pb.csParam[0] = kTOCActionTrackAddrs;
        *(Ptr *)&pb.csParam[1] = (Ptr)tb;
        pb.csParam[3] = (short)(4 * n);
        ((unsigned char *)pb.csParam)[8] = kBinToBCD(gTOC.firstTrack);

        gInSelfCall = true;
        if (PBControlSync((ParmBlkPtr)&pb) == noErr) {
            short k;
            gInSelfCall = false;
            for (k = 0; k < n; k++) {
                unsigned char *e = tb + 4 * k;
                /* Control is the LOW nibble; ADR is the high one. Reading the high
                   nibble here called a data track AUDIO, which is the bug Jubadub's
                   Warcraft disc found. Corrected to match CDReadTOC in
                   cd_probe_common.c, which is the parser that actually ships. */
                short ctrl = e[0] & 0x0F;
                gTOC.t[k].number = gTOC.firstTrack + k;
                gTOC.t[k].isData = (ctrl & 0x04) != 0;
                gTOC.t[k].lba    = ((long)kBCDToBin(e[1]) * 60 + kBCDToBin(e[2]))
                                   * kCDDASectorsPerSec + kBCDToBin(e[3])
                                   - kCDDALeadInSectors;
                gTOC.count++;
            }
            gTOC.valid = (gTOC.count > 0);
        } else {
            gInSelfCall = false;
        }
        DisposePtr((Ptr)tb);
    }
}

/* ---- the doubleback proc: INTERRUPT LEVEL --------------------------------- */

static pascal void DoubleBack(SndChannelPtr chan, SndDoubleBufferPtr buf)
{
    long want = kDBufBytes;
    long have;

    (void)chan;

    if (!gPlaying) {
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
        /* Underrun: emit silence, count it, never wait. readOff is not advanced, so
         * the audio resumes where it left off and the symptom is a gap. */
        long i;
        for (i = 0; i < want; i++) buf->dbSoundData[i] = 0;
        gUnderruns++;
    }

    buf->dbNumFrames = want / 4;
    buf->dbFlags     = dbBufferReady;
}

/* ---- install-time setup --------------------------------------------------- */

OSErr CDAudioInit(short refNum, short driveNum)
{
    THz  saveZone;
    long i;

    gRefNum   = refNum;
    gDriveNum = driveNum;

    gPCMBytes = ((long)kRingSeconds * kCDDABytesPerSec / kCDDASectorBytes)
                * kCDDASectorBytes;
    gPCM = (unsigned char *)NewPtrSys(gPCMBytes);
    if (gPCM == NULL) return memFullErr;

    for (i = 0; i < 2; i++) {
        gDBuf[i] = (SndDoubleBufferPtr)
                   NewPtrSysClear((Size)(sizeof(SndDoubleBuffer) + kDBufBytes));
        if (gDBuf[i] == NULL) return memFullErr;
    }

    gDBackUPP = NewSndDoubleBackUPP(DoubleBack);
    if (gDBackUPP == NULL) return memFullErr;

    /* ★ The channel is allocated with the SYSTEM zone current. A sound channel
     * allocated from driver context would otherwise land in whatever application
     * happened to be frontmost, and vanish when that application quit — the hazard
     * REVIEW.md §5 flagged. Allocated once here, kept for the session. */
    saveZone = GetZone();
    SetZone(SystemZone());
    if (SndNewChannel(&gChan, sampledSynth, initStereo, NULL) != noErr) gChan = NULL;
    SetZone(saveZone);
    if (gChan == NULL) return memFullErr;

    /* Read the TOC now, at task level, so AudioPlay does not have to. */
    ReadTOC();

    return noErr;
}

/* ---- transport ------------------------------------------------------------ */

static void PrimeBuffer(SndDoubleBufferPtr b)
{
    b->dbUserInfo[0] = 0;
    b->dbUserInfo[1] = 0;
    b->dbFlags       = 0;
    DoubleBack(gChan, b);
}

static long RefillOnce(void)
{
    long space, idx, contig, sectors, got;

    space  = gPCMBytes - (gWriteOff - gReadOff);
    idx    = gWriteOff % gPCMBytes;
    contig = gPCMBytes - idx;

    sectors = kChunkSectors;
    if (sectors * kCDDASectorBytes > space)  sectors = space / kCDDASectorBytes;
    if (sectors * kCDDASectorBytes > contig) sectors = contig / kCDDASectorBytes;
    if (sectors <= 0) return 0;
    if (gNextLBA >= gEndLBA) return 0;
    if (gNextLBA + sectors > gEndLBA) sectors = gEndLBA - gNextLBA;
    if (sectors <= 0) return 0;

    got = SelfReadSectors(gNextLBA, gPCM + idx, sectors);
    if (got <= 0) return 0;

    gWriteOff += got;
    gNextLBA  += got / kCDDASectorBytes;
    return got;
}

void CDAudioStop(void)
{
    if (!gPlaying) return;
    gPlaying = false;
    gPaused  = false;

    if (gChan != NULL) {
        SndCommand c;
        c.cmd = quietCmd; c.param1 = 0; c.param2 = 0;
        (void)SndDoImmediate(gChan, &c);
        c.cmd = flushCmd; c.param1 = 0; c.param2 = 0;
        (void)SndDoImmediate(gChan, &c);
    }

    if (gDelayShortened) {
        DCtlHandle dceH = GetDCtlEntry(gRefNum);
        if (dceH != NULL && *dceH != NULL) (*dceH)->dCtlDelay = gSavedDelay;
        gDelayShortened = false;
    }
    SetBlockSize(gSavedBlockSize);
}

OSErr CDAudioPlayLBA(long startLBA, long endLBA)
{
    SndDoubleBufferHeader2 h;
    OSErr                  err;
    long                   i;

    if (gChan == NULL || gPCM == NULL) return notOpenErr;
    if (gPlaying) CDAudioStop();

    gReadOff   = 0;
    gWriteOff  = 0;
    gNextLBA   = startLBA;
    gEndLBA    = endLBA;
    gUnderruns = 0;
    gPaused    = false;

    /* Remember what to put back, then take the drive for CD-DA reads. */
    {
        CntrlParam pb;
        for (i = 0; i < (long)sizeof(pb); i++) ((char *)&pb)[i] = 0;
        pb.ioCRefNum = gRefNum;
        pb.csCode    = kcsGetBlockSize;
        gInSelfCall = true;
        if (PBStatusSync((ParmBlkPtr)&pb) == noErr && pb.csParam[0] > 0)
            gSavedBlockSize = pb.csParam[0];
        gInSelfCall = false;
    }

    /* Pre-fill the ring before starting, so playback begins with a full read-ahead. */
    SetBlockSize(kCDDASectorBytes);
    while ((gWriteOff - gReadOff) < gPCMBytes) {
        if (RefillOnce() <= 0) break;
    }
    SetBlockSize(gSavedBlockSize);

    if (gWriteOff == 0) return ioErr;      /* nothing readable; leave audio alone */

    /* ★ Shorten accRun so the refill pump runs at ~10 Hz. Measured on hardware:
     * dCtlDelay is 120 ticks, i.e. accRun arrives every 2.0 s, which would starve a
     * 2-second ring continuously. Restored by CDAudioStop. */
    {
        DCtlHandle dceH = GetDCtlEntry(gRefNum);
        if (dceH != NULL && *dceH != NULL) {
            gSavedDelay = (*dceH)->dCtlDelay;
            (*dceH)->dCtlDelay = kRefillDelay;
            gDelayShortened = true;
        }
    }

    gPlaying = true;
    PrimeBuffer(gDBuf[0]);
    PrimeBuffer(gDBuf[1]);

    for (i = 0; i < (long)sizeof(h); i++) ((char *)&h)[i] = 0;
    h.dbhNumChannels   = 2;
    h.dbhSampleSize    = 16;
    h.dbhCompressionID = notCompressed;     /* Phase 1: accepted first try */
    h.dbhPacketSize    = 0;
    h.dbhSampleRate    = rate44khz;
    h.dbhBufferPtr[0]  = gDBuf[0];
    h.dbhBufferPtr[1]  = gDBuf[1];
    h.dbhDoubleBack    = gDBackUPP;
    h.dbhFormat        = k16BitLittleEndianFormat;   /* 'sowt': no byte swap */

    err = SndPlayDoubleBuffer(gChan, (SndDoubleBufferHeaderPtr)&h);
    if (err != noErr) {
        gPlaying = false;
        if (gDelayShortened) {
            DCtlHandle dceH = GetDCtlEntry(gRefNum);
            if (dceH != NULL && *dceH != NULL) (*dceH)->dCtlDelay = gSavedDelay;
            gDelayShortened = false;
        }
    }
    return err;
}

void CDAudioPause(Boolean pause)
{
    SndCommand c;
    if (!gPlaying || gChan == NULL) return;
    gPaused = pause;
    c.cmd = pause ? pauseCmd : resumeCmd;
    c.param1 = 0; c.param2 = 0;
    (void)SndDoImmediate(gChan, &c);
}

void CDAudioSetVolume(unsigned char l, unsigned char r)
{
    gVolL = l; gVolR = r;
    /* Software volume is a Step-5b refinement: scaling samples belongs in the
     * doubleback proc, and the value is recorded here so the behaviour is visible in
     * the trace before it is acted on. */
}

/* Called from accRun, at task level. */
void CDAudioRefill(void)
{
    long guard = 8;      /* bound the work done inside one Control call */

    if (!gPlaying || gPaused) return;

    /* Finished? */
    if (gNextLBA >= gEndLBA && (gWriteOff - gReadOff) <= 0) {
        CDAudioStop();
        return;
    }

    SetBlockSize(kCDDASectorBytes);
    while (guard-- > 0) {
        if (RefillOnce() <= 0) break;
    }
    SetBlockSize(gSavedBlockSize);
}

/* ---- decode a transport request ------------------------------------------- *
 * AudioPlay / AudioTrackSearch carry a position-type byte at csParam+0 and a position
 * from csParam+2. Phase 0 measured this driver accepting posType 0 with an MSF form,
 * but a game may use either, so both are decoded and the track table resolves either
 * into an LBA. Returns false if nothing sensible can be made of it. */
Boolean CDAudioDecodePos(const unsigned char *csParam, long *startLBA, long *endLBA)
{
    short i;
    long  lba = -1;

    if (!gTOC.valid) return false;

    /* MSF form: three BCD bytes at +2. */
    {
        short m = kBCDToBin(csParam[2]);
        short s = kBCDToBin(csParam[3]);
        short f = kBCDToBin(csParam[4]);
        if (m <= 99 && s < 60 && f < 75)
            lba = ((long)m * 60 + s) * kCDDASectorsPerSec + f - kCDDALeadInSectors;
    }

    /* Track form: a BCD track number at +2. Prefer it when it names a real track and
     * the MSF reading would be nonsense. */
    {
        short trk = kBCDToBin(csParam[2]);
        if (trk >= gTOC.firstTrack && trk <= gTOC.lastTrack &&
            (lba < 0 || csParam[3] == 0)) {
            for (i = 0; i < gTOC.count; i++)
                if (gTOC.t[i].number == trk) { lba = gTOC.t[i].lba; break; }
        }
    }

    if (lba < 0) return false;

    /* Play to the start of the next track, or the end of the disc. */
    *startLBA = lba;
    *endLBA   = lba + 75L * 60L * 80L;          /* generous default */
    for (i = 0; i < gTOC.count; i++)
        if (gTOC.t[i].lba > lba && gTOC.t[i].lba < *endLBA) *endLBA = gTOC.t[i].lba;

    return true;
}

/* For the reader's benefit. */
void CDAudioStats(long *underruns, long *delivered, Boolean *playing)
{
    if (underruns) *underruns = gUnderruns;
    if (delivered) *delivered = gReadOff;
    if (playing)   *playing   = gPlaying;
}

/*
 * cd_engine_driver.c — the resident PowerPC engine for the CD Audio Redirector.
 *
 * Built as a PEF shaped like a native driver, purely so that
 * `GetDriverMemoryFragment` will accept it and `SetDriverClosureMemory(connID, true)`
 * will hold its memory. It is deliberately **never installed into the unit table**:
 * we want resident PowerPC code, not a device driver the OS might try to open, close
 * or bind to a node. That also sidesteps `InstallDriverFromMemory` needing a
 * `RegEntryID`, which this CD driver does not have — `'nmrg'` returns −18 and
 * `dCtlNodeID` is 0.
 *
 * See cd_engine.h for why the engine is PowerPC and why the patch is one field.
 *
 * ★ STEP 2 DOES NOT PATCH. kInitialize finds the driver, validates the descriptor,
 * saves the original TVector and reports. `CDEngineControl` exists so its TVector is
 * real and so Step 3 is a one-line change, but nothing points at it yet.
 */

#include <MacTypes.h>
#include <Devices.h>
#include <Disks.h>
#include <Files.h>
#include <MacMemory.h>
#include <MixedMode.h>
#include <Gestalt.h>
#include <Timer.h>
#include <Events.h>

#include "cd_engine.h"
#include "cd_cscodes.h"



/* Low-memory globals by absolute address, as everywhere else in this project. */
#define LM_DrvQHdr          ((QHdrPtr)0x0308)
#define LM_UTableBase       (*(Ptr *)0x011C)
#define LM_UnitEntryCount   (*(short *)0x01D2)
/* ApplicZone() is not in the PowerPC import libs for this toolchain, so the
 * application zone comes from its low-memory global like everything else here. */
#define LM_ApplZone         (*(THz *)0x02AA)
/* Ticks straight from low memory. The handler may run at interrupt time, and a plain
 * aligned read is strictly safer there than a Toolbox trap. */
#define LM_Ticks            (*(volatile unsigned long *)0x016A)

#define kNoQueueBit         0x0200

/* ---- the driver description ------------------------------------------------ *
 * A fragment must declare at least one service or VerifyFragmentAsDriver rejects it
 * (DriverFamilyMatching.h: "The List of Services (at least one)"). The values are
 * copied from the shape this project already got accepted on hardware for the eSATA
 * work. driverRuntime is 0: we are not discovered, not opened upon load and not under
 * expert control, because nothing should manage this fragment but us. */

#define FOURCC(a,b,c,d) ((OSType)(((UInt32)(a)<<24)|((UInt32)(b)<<16)|((UInt32)(c)<<8)|(UInt32)(d)))

typedef struct {
    OSType     serviceCategory;
    OSType     serviceType;
    NumVersion serviceVersion;
} EngineServiceInfo;

typedef struct {
    OSType            sig;
    UInt32            descVersion;
    Str31             nameInfoStr;
    NumVersion        typeVersion;
    UInt32            driverRuntime;
    Str31             driverName;
    UInt32            reserved[8];
    UInt32            nServices;
    EngineServiceInfo service0;
} EngineDriverDescription;

EngineDriverDescription TheDriverDescription = {
    FOURCC('m','t','e','j'),                  /* kTheDescriptionSignature       */
    0,                                        /* kInitialDriverDescriptor       */
    /* Str31 as an explicit byte list: a brace-wrapped string literal is not a
     * load-time-computable initializer for unsigned char[32] here. 13 = strlen. */
    { 13,'C','D','A','u','d','i','o','E','n','g','i','n','e' },
    { 1, 0, 0x80 /*final*/, 0 },
    0x00000000UL,                             /* managed by nobody but us       */
    { 13,'C','D','A','u','d','i','o','E','n','g','i','n','e' },
    { 0,0,0,0,0,0,0,0 },
    1,
    { FOURCC('n','d','r','v'), FOURCC('b','l','o','k'), { 1, 0, 0x80, 0 } }
};

/* ---- resident state ------------------------------------------------------- *
 * Reached through the TOC, which is correct on entry because we are always called
 * through a TVector. This is the thing the 68K flat code resource had to work around
 * with dbUserInfo and relocation. */

typedef OSErr (*CDCtlProc)(ParmBlkPtr pb, DCtlPtr dce);

static CDCtlProc       gOrigCtl    = NULL;
static CDEngineTrace  *gRing       = NULL;
static CDEngineInfo   *gInfo       = NULL;
static CDEnginePublic *gPub        = NULL;  /* system heap, published via Gestalt */

/* Everything patch/unpatch needs, held HERE rather than in the caller's info block —
 * that block is a global in the installer application and dies when it quits. */
static RoutineDescriptorPtr gCtlRD      = NULL;
static ProcPtr              gSavedTV    = NULL;
static Boolean              gPatched    = false;

/* ---- the Control handler --------------------------------------------------- *
 * Not yet reachable in Step 2. When the descriptor's TVector is repointed here, Mixed
 * Mode invokes this with the SAME procInfo the original had — kRegisterBased with
 * A0 = ParmBlkPtr, A1 = DCtlPtr, word result in D0 — which for a PowerPC routine
 * arrives as ordinary arguments and an ordinary return value.
 *
 * INTERRUPT SAFETY: Control may be issued asynchronously, so this may run below task
 * level. It does nothing but plain stores into a pre-allocated ring, then chains. No
 * allocation, no File Manager, no logging, no waiting.
 *
 * Chaining is a plain indirect call through the saved TVector and it RETURNS, because
 * that is exactly what Mixed Mode does when the Device Manager calls the original. So
 * whatever the original does about jIODone for a queued request, it keeps doing. */
OSErr CDEngineControl(ParmBlkPtr pb, DCtlPtr dce)
{
    if (gRing != NULL && gPub != NULL && pb != NULL) {
        CDEngineTrace *e = &gRing[gPub->writeCount & (kEngineRingEntries - 1)];
        short          cs = ((CntrlParam *)pb)->csCode;
        int            i;

        e->ticks  = LM_Ticks;
        e->csCode = cs;
        e->ioTrap = (unsigned short)pb->ioParam.ioTrap;
        for (i = 0; i < 8; i++)
            e->csParam[i] = ((unsigned char *)((CntrlParam *)pb)->csParam)[i];

        gPub->callCount++;
        if (cs == kcsAudioPlay || cs == kcsAudioTrackSearch || cs == kcsAudioPause ||
            cs == kcsAudioStop || cs == kcsAudioStatus      || cs == kcsAudioScan  ||
            cs == kcsAudioControl || cs == kcsReadTheQSubcode ||
            cs == kcsReadAudioVolume)
            gPub->audioCallCount++;

        /* Incremented LAST, so a reader never sees a slot count that outruns its
         * contents. Plain stores throughout: this may be interrupt time. */
        gPub->writeCount++;
    }

    /* ★ SYNTHESIS: chain first, then rewrite the answer.
     *
     * The last hardware run settled the question this was waiting on. CDPlayProbe polled
     * AudioStatus and ReadQ forty times through this handler, and every one chained and
     * came back — so a QUEUED call does return to us, and we can let the original answer
     * and then correct it. No need to reproduce the jIODone completion protocol.
     *
     * We rewrite only what we can decode. ReadQ's nine bytes were cross-checked against
     * the TOC to the exact frame in Phase 0, so all of them are rewritten. For
     * AudioStatus only bytes 3..5 (the absolute MSF, which match ReadQ's) plus the
     * play-state byte are touched; the rest of the original's answer is left alone.
     *
     * Interrupt-safe: reads of the published cursor and plain stores into csParam. */
    /* ★ CAPTURE THE DRIVER'S OWN ANSWER TO AN IDLE AudioStatus, once.
     *
     * callCount, audioCallCount and the trace ring already see every call, so a game's
     * polling was never invisible - it was merely never read back after a game session.
     * What none of them hold is the REPLY: the ring copies csParam on the way IN.
     *
     * A game at launch finds the pump idle and asks how the CD is doing. Whatever the
     * driver says is what it bases its decision on, and on the mini that decision comes
     * out as "no music" while the MDD plays. So chain, keep a copy of the answer, and
     * return it untouched. Plain stores, no logging: this may be interrupt time. */
    if (pb != NULL && gPub != NULL && gPub->patched && gPub->playState == 0 &&
        !gPub->idleStatusCaptured && gOrigCtl != NULL &&
        ((CntrlParam *)pb)->csCode == kcsAudioStatus) {
        unsigned char *cp0 = (unsigned char *)((CntrlParam *)pb)->csParam;
        OSErr e0 = gOrigCtl(pb, dce);
        int   k;
        for (k = 0; k < 16; k++) gPub->idleStatusParam[k] = cp0[k];
        gPub->idleStatusCaptured = 1;
        return e0;
    }

    if (pb != NULL && gPub != NULL && gPub->patched && gPub->playState != 0) {
        short cs = ((CntrlParam *)pb)->csCode;

        if (cs == kcsAudioStatus || cs == kcsReadTheQSubcode) {
            unsigned char *cp = (unsigned char *)((CntrlParam *)pb)->csParam;
            OSErr          err;
            long           absF, relF;
            short          trk, state;
            int            i;

            if (gOrigCtl == NULL) return controlErr;
            err = gOrigCtl(pb, dce);          /* let the original answer first */

            /* Snapshot the cursor once, so every field of one answer is consistent
             * even if playback advances while we format it. */
            absF  = gPub->curAbsFrame;
            relF  = gPub->curRelFrame;
            trk   = gPub->curTrack;
            state = gPub->playState;

            if (cs == kcsAudioStatus) {
                if (!gPub->origStatusCaptured) {
                    for (i = 0; i < 16; i++) gPub->origStatusParam[i] = cp[i];
                    gPub->origStatusCaptured = 1;
                }
                cp[0] = (state == 2) ? kSynthStatusPaused
                      : (state == 3) ? kSynthStatusCompleted
                                     : kSynthStatusPlaying;
                cp[3] = kBinToBCD((absF / 75) / 60);
                cp[4] = kBinToBCD((absF / 75) % 60);
                cp[5] = kBinToBCD(absF % 75);
                gPub->synthStatusCount++;
            } else {
                if (!gPub->origReadQCaptured) {
                    for (i = 0; i < 16; i++) gPub->origReadQParam[i] = cp[i];
                    gPub->origReadQCaptured = 1;
                }
                cp[0] = 0x00;                        /* control/adr: audio, no pre-emph */
                cp[1] = kBinToBCD(trk);
                cp[2] = kBinToBCD(1);                /* index 1 */
                cp[3] = kBinToBCD((relF / 75) / 60);
                cp[4] = kBinToBCD((relF / 75) % 60);
                cp[5] = kBinToBCD(relF % 75);
                cp[6] = kBinToBCD((absF / 75) / 60);
                cp[7] = kBinToBCD((absF / 75) % 60);
                cp[8] = kBinToBCD(absF % 75);
                gPub->synthReadQCount++;
            }
            return err;
        }
    }

    /* ★ POST THE REQUEST AND CHAIN. NO I/O HERE, EVER.
     *
     * The previous version read the disc and started playback from inside this handler
     * and deadlocked: a synchronous PBRead to a driver cannot begin until the Control
     * call it is nested inside returns. So all this does now is record what was asked
     * for and let the original driver answer as it always has.
     *
     * That leaves the handler safe at any interrupt level — a few plain stores and a
     * chain — and puts every read in the pump application's task context, where
     * Phase 1 proved the streaming engine works. If no pump is running the request is
     * simply unserviced: nothing crashes. */
    /* When the ring is inline this is an array and the test is trivially true, which
     * -Wall rightly complains about; only the separate-allocation build needs it. */
#if CD_RING_SEPARATE
#define kRingUsable (gPub->reqRing != NULL)
#else
#define kRingUsable 1
#endif
    /* Set when we post a request the PUMP will actually act on, so the chain-return
     * below knows whether we are entitled to overrule a refusal. */
    Boolean weWillService = false;

    if (pb != NULL && gPub != NULL && gPub->patched && kRingUsable) {
        short cs = ((CntrlParam *)pb)->csCode;

        /* ★★ THE PUMP'S OWN AudioStop PASSES STRAIGHT THROUGH.
         *
         * Once the encoding was corrected, .AppleCD started ACCEPTING AudioPlay and
         * really driving the transport - which on a machine with no analog wire makes
         * no sound and simply occupies the drive. Runs A and B both stalled every
         * task-level app on the machine for about 31 seconds per play, draining the
         * ring and emitting 29 s of silence. So the pump now stops the driver's
         * transport right after it takes a play over, and that stop must not be
         * mistaken for the game asking us to stop.
         *
         * ⚠ The window between the pump setting the flag and this call arriving is not
         * a race on a cooperative system: the pump does not yield between the two, so
         * no other task can slip an AudioStop in between. An interrupt-level caller
         * still could in principle, which is why every suppression is COUNTED rather
         * than assumed - a game's lost Stop would show up as stopsSuppressed climbing
         * without the pump having asked for it. */
        if (cs == kcsAudioStop && gPub->suppressStops > 0) {
            gPub->suppressStops--;
            gPub->stopsSuppressed++;
            /* Chain it so the driver really does stop, but post nothing: this stop is
             * ours, and the pump must keep playing. */
            return (gOrigCtl != NULL) ? gOrigCtl(pb, dce) : controlErr;
        }

        /* Only the four the pump's switch handles. AudioScan and AudioControl are
         * posted for the trace but the pump does nothing with them, so a refusal of
         * those is not ours to overrule. */
        if (cs == kcsAudioPlay || cs == kcsAudioTrackSearch ||
            cs == kcsAudioPause || cs == kcsAudioStop)
            weWillService = true;

        if (cs == kcsAudioPlay || cs == kcsAudioTrackSearch ||
            cs == kcsAudioPause || cs == kcsAudioStop ||
            cs == kcsAudioScan  || cs == kcsAudioControl) {
            const unsigned char *cp =
                (const unsigned char *)((CntrlParam *)pb)->csParam;
            long             w = gPub->reqWrite;
#if CD_RING_MODE
            volatile CDEngineRequest *e =
                &gPub->reqRing[w & (kEngineReqRingEntries - 1)];
#else
            /* Bisect: v1's behaviour, v4's memory. Always slot 0, so the producer's
             * store lands at a FIXED offset exactly as it did in v1. */
            volatile CDEngineRequest *e = &gPub->reqRing[0];
#endif
            int              i;

            e->csCode = cs;
            for (i = 0; i < 16; i++) e->param[i] = cp[i];

            /* Published LAST, and this single aligned store is what makes the entry
             * visible. Until it lands the pump does not know the slot exists, so it
             * can never read one half-written. Still just plain stores: nothing here
             * allocates, blocks or calls out, so it remains safe at any interrupt
             * level — which is the whole reason the pump exists. */
            gPub->reqWrite = w + 1;
        }
    }

    if (gOrigCtl == NULL) return controlErr;

    /* ★ CHAIN, THEN COUNT — we no longer try to correct the answer.
     *
     * v8 replaced the return value with noErr here. It did not work, and the run that
     * proved it is worth remembering: the counter incremented three times while the
     * caller received -50 all three times. These calls are QUEUED, so the original
     * completes the request through jIODone before returning to us and the Device
     * Manager hands the caller ioResult, not our return value. Details and the
     * decision not to touch ioResult are in kEngineNotableRefusal.
     *
     * The counter stays, because "the driver refused something we serviced" is exactly
     * what a real game's complaint would look like from this side, and we would rather
     * have the number than guess.
     *
     * Interrupt-safe: a comparison and one increment. */
    {
        OSErr err = gOrigCtl(pb, dce);

        if (weWillService && gPub != NULL && gPub->pumpAlive &&
            err != noErr && kEngineNotableRefusal(err))
            gPub->refusalsServiced++;

        return err;
    }
}

/* ---- passive driver discovery --------------------------------------------- *
 * Read each DCE's DRVR header name straight out of memory and look for ".AppleCD".
 * No driver is called, so this cannot hang the way the Phase-0 unit-table sweep did —
 * that hang came from issuing Status calls to arbitrary drivers, one of which never
 * completed. Every pointer is checked and every name byte must be printable. */

static Boolean GetUnitName(short refNum, unsigned char *out, short outMax)
{
    DCtlHandle     dceH;
    DCtlPtr        dce;
    unsigned char *base;
    DRVRHeaderPtr  hdr;
    short          len, i;

    dceH = GetDCtlEntry(refNum);
    if (dceH == NULL || *dceH == NULL) return false;
    dce = *dceH;

    if (dce->dCtlFlags & dRAMBasedMask) {
        Handle hh = (Handle)dce->dCtlDriver;
        if (hh == NULL || *hh == NULL) return false;
        base = (unsigned char *)(*hh);
    } else {
        base = (unsigned char *)dce->dCtlDriver;
    }
    if (base == NULL || ((unsigned long)base & 1) ||
        (unsigned long)base < 0x1000)
        return false;

    hdr = (DRVRHeaderPtr)base;
    len = hdr->drvrName[0];
    if (len < 1 || len > 31 || len >= outMax) return false;
    for (i = 1; i <= len; i++) {
        unsigned char c = hdr->drvrName[i];
        if (c < 0x20 || c > 0x7E) return false;
    }
    for (i = 0; i <= len; i++) out[i] = hdr->drvrName[i];
    return true;
}

static Boolean NameIsAppleCD(const unsigned char *p)
{
    static const char want[8] = { '.','A','P','P','L','E','C','D' };
    short i;

    if (p[0] != 8) return false;
    for (i = 0; i < 8; i++) {
        unsigned char c = p[1 + i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        if (c != (unsigned char)want[i]) return false;
    }
    return true;
}

static short FindCDRefNum(void)
{
    Ptr   utable = LM_UTableBase;
    short count  = LM_UnitEntryCount;
    short i;

    if (utable == NULL || count <= 0 || count > 256) return 0;
    for (i = 0; i < count; i++) {
        unsigned char name[36];
        if (((DCtlHandle *)utable)[i] == NULL) continue;
        if (!GetUnitName((short)~i, name, sizeof(name))) continue;
        if (NameIsAppleCD(name)) return (short)~i;
    }
    return 0;
}

/* ---- kInitialize: validate and report, change nothing --------------------- */

static OSErr EngineInit(CDEngineInfo *info)
{
    short           refNum;
    DCtlHandle      dceH;
    DCtlPtr         dce;
    unsigned char  *base;
    DRVRHeaderPtr   hdr;
    RoutineDescriptorPtr rd;
    unsigned long       *tv;

    if (info == NULL) return paramErr;

    /* The caller zeroes this, but do not depend on it. */
    info->magic   = kEngineMagic;
    info->version = kEngineVersion;
    info->patched = 0;
    info->status  = kEngineOK;

    refNum = FindCDRefNum();
    if (refNum == 0) { info->status = kEngineNoDriver; return noErr; }
    info->cdRefNum = refNum;

    dceH = GetDCtlEntry(refNum);
    if (dceH == NULL || *dceH == NULL) { info->status = kEngineNoDCE; return noErr; }
    dce = *dceH;

    if (dce->dCtlFlags & dRAMBasedMask) { info->status = kEngineRAMBased; return noErr; }

    base = (unsigned char *)dce->dCtlDriver;
    if (base == NULL || ((unsigned long)base & 1) ||
        (unsigned long)base < 0x1000) {
        info->status = kEngineBadDriverPtr;
        return noErr;
    }
    info->dCtlDriver = (Ptr)base;
    hdr = (DRVRHeaderPtr)base;

    if (hdr->drvrCtl == 0 || (hdr->drvrCtl & 1) ||
        hdr->drvrCtl < 0x12 || (unsigned short)hdr->drvrCtl > 0x4000) {
        info->status = kEngineNotDRVRShape;
        return noErr;
    }

    /* Typed access through MixedMode.h's real structs, so an offset cannot be
     * mis-transcribed again. Step 2's first run failed exactly that way. */
    rd = (RoutineDescriptorPtr)(base + (unsigned short)hdr->drvrCtl);
    info->ctlDescriptor = (Ptr)rd;

    /* Is it the Mixed Mode descriptor we characterised? */
    if (rd->goMixedModeTrap != kRDMagic) {
        info->status = kEngineNotDescriptor;
        return noErr;
    }
    info->rdVersion = (UInt8)rd->version;
    info->procInfo  = (UInt32)rd->routineRecords[0].procInfo;
    info->isa       = (UInt8)rd->routineRecords[0].ISA;

    if (info->isa != kRDISAPowerPC) { info->status = kEngineNotPowerPCISA; return noErr; }

    info->origTVector = (Ptr)rd->routineRecords[0].procDescriptor;

    if (info->origTVector == NULL || ((unsigned long)info->origTVector & 3)) {
        info->status = kEngineBadTVector;
        return noErr;
    }

    /* A TVector is two words: the code address and the TOC. Reading both is the
     * cheapest sanity check that we are looking at one, and it gives Step 3 a value
     * to compare against after the write. */
    tv = (unsigned long *)info->origTVector;
    info->origCode = tv[0];
    info->origTOC  = tv[1];

    /* Our own handler. In CFM a function pointer IS the TVector address. */
    info->ourTVector = (Ptr)CDEngineControl;
    tv = (unsigned long *)info->ourTVector;
    info->ourCode = tv[0];
    info->ourTOC  = tv[1];

    /* ★ Would our own code survive the installer quitting? CFM uses a PEF's code
     * section in place, so if the installer handed us the resource handle rather than
     * a system-heap copy, our code is in the application heap and would be freed on
     * quit. Refuse rather than install something with a fuse in it. */
    {
        THz appZone = LM_ApplZone;
        if (appZone != NULL &&
            (Ptr)info->ourCode >= (Ptr)appZone &&
            (Ptr)info->ourCode <  appZone->bkLim) {
            info->status = kEngineCodeInAppHeap;
            return noErr;
        }
    }

    /* Allocate the ring now, so Step 3 has nothing left to fail at. */
    gRing = (CDEngineTrace *)NewPtrSysClear(
                (Size)(kEngineRingEntries * sizeof(CDEngineTrace)));
    if (gRing == NULL) { info->status = kEngineNoMemory; return noErr; }
    info->ring        = (Ptr)gRing;
    info->ringEntries = kEngineRingEntries;

    /* Publish, so a separate reader app can find the ring and the counters. */
    if (gPub == NULL) {
        gPub = (CDEnginePublic *)NewPtrSysClear((Size)sizeof(CDEnginePublic));
        if (gPub == NULL) { info->status = kEngineNoMemory; return noErr; }
    }
#if CD_RING_SEPARATE
    /* The request ring in its own block. Allocated BEFORE the descriptor is patched, so
     * the handler can never see a non-NULL gPub with a NULL ring — and it NULL-checks
     * anyway, because "cannot happen" is how the interrupt-level code gets to crash. */
    if (gPub->reqRing == NULL) {
        gPub->reqRing = (volatile CDEngineRequest *)NewPtrSysClear(
                            (Size)(kEngineReqRingEntries * sizeof(CDEngineRequest)));
        if (gPub->reqRing == NULL) { info->status = kEngineNoMemory; return noErr; }
    }
#endif
    /* ★ Record what the system heap looks like AFTER we have taken our share. This is
     * the number the freeze investigation now turns on: the fault tracks the size of
     * this block and nothing else, so how much room was left matters more than any
     * property of our own code. Cheap, and it is on the record before anything can go
     * wrong. */
    {
        Size grow = 0;
        gPub->sysFreeAtInit    = (long)FreeMemSys();
        gPub->sysLargestAtInit = (long)MaxMemSys(&grow);
        gPub->pubBlockBytes    = (long)sizeof(CDEnginePublic);
    }

    gPub->magic          = kEngineMagic;
    gPub->version        = kEngineVersion;
    gPub->patched        = 0;
    gPub->cdRefNum       = refNum;
    gPub->origTVector    = info->origTVector;
    gPub->ourTVector     = info->ourTVector;
    gPub->ring           = (Ptr)gRing;
    gPub->ringEntries    = kEngineRingEntries;
    gPub->writeCount     = 0;
    gPub->callCount      = 0;
    gPub->audioCallCount = 0;

    /* ★ Try all three registration entry points and RECORD EVERY RESULT.
     * The first version called only SetGestaltValue and discarded the error with a
     * (void) cast; the selector never registered and the reader reported
     * gestaltUndefSelectorErr (-5551) with nothing to explain it. Whichever of these
     * OS 9 accepts for a brand-new value selector, we will now know. */
    info->pubBlock          = (Ptr)gPub;
    info->gestaltNewErr     = NewGestaltValue(kEnginePublicSelector, (long)gPub);
    info->gestaltReplaceErr = 1;     /* 1 = not attempted */
    info->gestaltSetErr     = 1;
    if (info->gestaltNewErr != noErr) {
        info->gestaltReplaceErr = ReplaceGestaltValue(kEnginePublicSelector,
                                                     (long)gPub);
        if (info->gestaltReplaceErr != noErr)
            info->gestaltSetErr = SetGestaltValue(kEnginePublicSelector, (long)gPub);
    }
    info->gestaltPublished = (info->gestaltNewErr     == noErr ||
                              info->gestaltReplaceErr == noErr ||
                              info->gestaltSetErr     == noErr) ? 1 : 0;

    /* Kept resident here rather than in the info block, so neither the handler nor
     * a later patch/unpatch ever dereferences caller-owned memory. */
    gOrigCtl = (CDCtlProc)info->origTVector;
    gCtlRD   = rd;
    gSavedTV = rd->routineRecords[0].procDescriptor;
    gInfo    = info;

    /* No audio setup here any more. The engine does no I/O at all — see the mailbox
     * note in cd_engine.h. The pump application owns the ring, the sound channel and
     * the TOC, because all three need a task-level context this code does not have. */
    info->driveNum     = 0;
    info->audioInitErr = noErr;

    /* ★ Deliberately NOT writing the descriptor here. The patch command does that. */
    return noErr;
}

/* ---- Step 3: the patch, and its exact inverse ------------------------------ *
 * One aligned 4-byte store into `procDescriptor`. procInfo and ISA are left alone, so
 * Mixed Mode performs exactly the transition it performs today and simply lands in our
 * routine. The DRVR header, the driver's name, its address and dCtlDriver are all
 * untouched — which is what makes this different from the 68K shell that broke iTunes.
 *
 * Everything is re-validated at the moment of writing rather than trusted from the
 * earlier init call: the driver could have been reloaded in between. */
static OSErr EnginePatch(CDEngineInfo *info)
{
    if (info != NULL) info->patched = 0;

    if (gCtlRD == NULL || gOrigCtl == NULL || gRing == NULL) return paramErr;
    if (gPatched) {
        if (info != NULL) { info->status = kEngineAlreadyPatched; }
        return noErr;
    }

    /* Re-check the descriptor, now, immediately before the store. */
    if (gCtlRD->goMixedModeTrap != kRDMagic)              return paramErr;
    if (gCtlRD->routineRecords[0].ISA != kRDISAPowerPC)   return paramErr;
    if (gCtlRD->routineRecords[0].procDescriptor != gSavedTV) return paramErr;

    /* ★ THE PATCH. */
    gCtlRD->routineRecords[0].procDescriptor = (ProcPtr)CDEngineControl;
    gPatched = true;

    if (gPub != NULL) gPub->patched = 1;
    if (info != NULL) {
        info->patched     = 1;
        info->origTVector = (Ptr)gSavedTV;
        info->ourTVector  = (Ptr)CDEngineControl;
        /* Read it back, so the caller reports what is actually in the driver now
         * rather than what we intended to put there. */
        info->ring        = (Ptr)gRing;
        info->status      = (gCtlRD->routineRecords[0].procDescriptor ==
                             (ProcPtr)CDEngineControl) ? kEngineOK : kEngineBadTVector;
    }
    return noErr;
}

static OSErr EngineUnpatch(CDEngineInfo *info)
{
    if (gCtlRD == NULL || gSavedTV == NULL) return paramErr;
    if (!gPatched) { if (info != NULL) info->patched = 0; return noErr; }

    gCtlRD->routineRecords[0].procDescriptor = gSavedTV;
    gPatched = false;

    if (gPub != NULL) gPub->patched = 0;
    if (info != NULL) {
        info->patched = 0;
        info->status  = (gCtlRD->routineRecords[0].procDescriptor == gSavedTV)
                        ? kEngineOK : kEngineBadTVector;
    }
    /* The ring is deliberately not freed: a Control call could still be inside the
     * handler, and leaking one small system-heap block until restart is far cheaper
     * than freeing memory something might still be using. */
    return noErr;
}

static OSErr EngineFinalize(void)
{
    /* Nothing to undo in Step 2: nothing was patched. The ring is deliberately not
     * freed — once Step 3 is live a Control call could be inside the handler, and
     * leaking one small system-heap block until restart is far cheaper than freeing
     * memory something might still be using. */
    gOrigCtl = NULL;
    gInfo    = NULL;
    return noErr;
}

/* ---- the native-driver entry point ---------------------------------------- *
 * Called by US, never by the Device Manager, because this fragment is never installed
 * into the unit table. So `contents` carries a CDEngineInfo* rather than the
 * DriverInitInfo* the Device Manager would pass. */

/* The real signature from Devices.h:291. We are the only caller, so `contents`
 * carries a CDEngineInfo* in its `pb` member rather than the DriverInitInfo* the
 * Device Manager would pass. It is a union of pointers, so this is well defined. */
OSErr DoDriverIO(AddressSpaceID spaceID, IOCommandID cmdID,
                 IOCommandContents contents, IOCommandCode code, IOCommandKind kind)
{
    (void)spaceID; (void)cmdID; (void)kind;

    switch (code) {
        case kEngineInitCommand:
            return EngineInit((CDEngineInfo *)contents.pb);
        case kEnginePatchCommand:
            return EnginePatch((CDEngineInfo *)contents.pb);
        case kEngineUnpatchCommand:
            return EngineUnpatch((CDEngineInfo *)contents.pb);
        case kEngineFinalizeCommand:
            return EngineFinalize();
        default:
            return paramErr;
    }
}

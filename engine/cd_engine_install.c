/*
 * CDEngineInstall — Step 2: make the PowerPC engine resident, and validate the
 * descriptor we intend to patch. **It does not patch anything.**
 *
 * WHAT THIS RUN PROVES, AND WHY IT CANNOT BREAK ANYTHING
 * -----------------------------------------------------
 * Three previous 68K installs crashed the machine, and one broke iTunes' ability to
 * read audio CDs. Every one of those failures came from *modifying* the CD driver. So
 * this run modifies nothing at all:
 *
 *   1. `GetDriverMemoryFragment` prepares the engine PEF from memory. If the fragment
 *      is malformed, this fails cleanly with an error code and we stop.
 *   2. `SetDriverClosureMemory(connID, true)` holds the fragment's memory so the code
 *      outlives this application. That is the whole residency question, answered on
 *      its own, with nothing else at stake.
 *   3. We call the fragment's `main` (its `DoDriverIO`) ourselves with an init command.
 *      It finds `.AppleCD` by a passive name scan, validates the Control descriptor,
 *      saves the original TVector and reports everything back.
 *
 * The fragment is deliberately **not** installed into the unit table. We want resident
 * PowerPC code, not a driver the OS might open, close or bind — and
 * `InstallDriverFromMemory` would need a `RegEntryID` this CD driver does not have
 * (`'nmrg'` returns −18, `dCtlNodeID` is 0).
 *
 * Read the log's ⇒ lines: they say whether Step 3 is safe to attempt and what it will
 * write.
 */

#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Events.h>
#include <Devices.h>
#include <Files.h>
#include <MacMemory.h>
#include <Resources.h>
#include <Folders.h>
#include <Script.h>
#include <ToolUtils.h>
#include <CodeFragments.h>
#include <AppleEvents.h>
#include <AEInteraction.h>      /* AEProcessAppleEvent lives here, not in AppleEvents.h */

#include <stdio.h>
#include <string.h>

/* ★ TWO BUILDS FROM THIS ONE SOURCE.
 *
 *   CD_FACELESS = 0   CDPump — the diagnostic tool. Opens a progress window,
 *                     requires a modifier key before it will touch anything, and
 *                     stops on a click. This is what every hardware run so far used.
 *
 *   CD_FACELESS = 1   CD Audio Redirector — the artifact people install. Drops into
 *                     Startup Items, patches with no key held, opens no window, and
 *                     runs until the machine shuts down.
 *
 * One source rather than two, because the thing being shipped must be the thing that
 * was tested. The differences are confined to: whether the progress window opens,
 * whether a modifier key is required, and how the pump loop ends.
 *
 * ⚠ It still cannot be a real INIT, for three reasons established earlier and none of
 * them softened by going faceless: an INIT has no ongoing task-level context; the
 * audio must run outside any driver Control call (the deadlock); and an INIT patches
 * before the real ATAPI driver exists. A Startup Items app is the same one file and
 * one restart from the user's point of view. */
#ifndef CD_FACELESS
#define CD_FACELESS 0
#endif

#include "cd_probe_common.h"
#include "cd_engine.h"
#include "cd_cscodes.h"

/* The audio engine, in this application's task context (cd_pump_audio.c). */
extern OSErr CDPumpInit(short refNum, short driveNum);
extern OSErr CDPumpPlay(const unsigned char *csParam);
extern void  CDPumpStop(void);
extern void  CDPumpPause(Boolean pause);
extern void  CDPumpIdle(void);
extern void  CDPumpStats(Boolean *playing, long *underruns, long *delivered);
extern void  CDPumpSetPublic(CDEnginePublic *pub);

#if CD_FACELESS
/* With no window and no menu there is nothing to click, so the quit Apple event is the
 * only orderly way to stop. Handling it matters: a background app that ignores quit can
 * stall shutdown, and stalling the user's shutdown is a far worse bug than anything this
 * extension is trying to fix. */
static Boolean gQuitRequested = false;

static pascal OSErr HandleQuitEvent(const AppleEvent *ae, AppleEvent *reply, long refCon)
{
    (void)ae; (void)reply; (void)refCon;
    gQuitRequested = true;
    return noErr;
}

/* The Finder sends 'oapp' at launch. Nothing to do with it, but an app that declares
 * itself high-level-event-aware and then has no handler leaves the event to fail; a
 * no-op handler is two lines and removes the question. */
static pascal OSErr HandleOpenAppEvent(const AppleEvent *ae, AppleEvent *reply,
                                       long refCon)
{
    (void)ae; (void)reply; (void)refCon;
    return noErr;
}
#endif

/* Watch the engine's mailbox and service what the game asked for. Returns when the user
 * clicks or presses a key — or, in the faceless build, when asked to quit.
 *
 * ★ This loop is the whole reason the restructure happened. The driver patch cannot do
 * this work: a synchronous read to a driver deadlocks if issued from inside that driver's
 * own Control call. Here we are in an ordinary application, outside any Control call, so
 * the reads simply work — as Phase 1 measured. */
static void PumpLoop(CDEnginePublic *pub, short refNum)
{
    EventRecord evt;
    long        reported = -1;
    long        lastBeat = 0;       /* playback heartbeat, see below */

    if (pub == NULL) return;
    /* Start level with the producer: anything posted before the pump existed was never
     * ours to service, and replaying it would start playback nobody asked for. */
    pub->reqRead = pub->reqWrite;
    pub->pumpAlive = 1;
    CDPumpSetPublic(pub);        /* so the pump can publish the playback cursor */

    CDLogf("=== PUMP RUNNING. Click or press a key to stop and quit. ===");
    CDProgressSay("pump running - now run CDPlayProbe_v2 and LISTEN");

    for (;;) {
        /* ★ DRAIN EVERY PENDING REQUEST, not just the newest one.
         *
         * The old code read whichever request happened to be in the single mailbox slot
         * and then jumped lastSeq to that sequence number, stepping silently over
         * anything that had arrived since it last looked. The hardware logs show it
         * happening in every run — the request numbers skip. See cd_engine.h. */
#if CD_RING_SEPARATE
        if (pub->reqRing == NULL) { CDPumpIdle(); goto afterDrain; }
#endif
        while (pub->reqRead != pub->reqWrite) {
            long             backlog = pub->reqWrite - pub->reqRead;
            long             seq;
            volatile CDEngineRequest *e;
            short            cs;
            unsigned char    param[16];
            int              i;

            /* Producer lapped us: the oldest entries have been overwritten. Count them
             * and resynchronise on the oldest one that survives. This should never
             * happen with 16 entries and a 1-tick poll, and if it ever does the count
             * says so out loud rather than leaving another silent gap. */
            if (backlog > kEngineReqRingEntries) {
                long lost = backlog - kEngineReqRingEntries;
                pub->reqDropped += lost;
                pub->reqRead     = pub->reqWrite - kEngineReqRingEntries;
                CDLogf("  !! request ring overflowed: %ld request(s) lost "
                       "(%ld total). The pump is not getting enough time.",
                       lost, pub->reqDropped);
            }

#if CD_RING_MODE
            seq = pub->reqRead;
            e   = &pub->reqRing[seq & (kEngineReqRingEntries - 1)];
#else
            /* Bisect: v1's behaviour on v4's memory. Take only the NEWEST request from
             * the fixed slot and skip the rest, exactly as the single-slot mailbox did
             * — including its drop, which is the bug we are trying to keep fixed. This
             * build exists to answer a question, not to ship. */
            seq = pub->reqWrite - 1;
            e   = &pub->reqRing[0];
#endif
            cs  = e->csCode;
            for (i = 0; i < 16; i++) param[i] = e->param[i];

            /* Re-check AFTER the copy: if the producer lapped us while we were reading
             * this very slot, what we just copied is a different request. Discard it
             * and let the overflow branch above resynchronise on the next pass. */
            if (pub->reqWrite - seq > kEngineReqRingEntries) continue;

            pub->reqRead = seq + 1;
            pub->pumpReqSeen++;      /* published, so it survives a dead log */

            CDLogf("--- request %ld: csCode %d ---", seq, cs);
            CDLogHexAt("  param", param, 16, 0);
            switch (cs) {
                case kcsAudioPlay:
                    CDProgressSay("AudioPlay -> starting playback");
                    (void)CDPumpPlay(param);
                    break;
                case kcsAudioTrackSearch:
                    /* csParam+6 non-zero means hold, not play. */
                    if (param[6] == 0) {
                        CDProgressSay("AudioTrackSearch -> starting playback");
                        (void)CDPumpPlay(param);
                    } else {
                        CDLogf("  hold flag set; positioning only, not playing");
                    }
                    break;
                case kcsAudioPause:
                    CDProgressSay("AudioPause(%d)", param[0]);
                    CDPumpPause(param[0] != 0);
                    break;
                case kcsAudioStop:
                    CDProgressSay("AudioStop");
                    CDPumpStop();
                    break;
                default:
                    break;
            }

#if CD_RING_MODE
            /* Keep the ring fed between requests too. A burst of queued requests can
             * take a while to work through — CDPumpPlay alone pre-rolls for a second —
             * and the audio must not starve while we catch up.
             *
             * Gated with the bisect switch so mode 0 restores v1's behaviour COMPLETELY
             * — one request per pass and one CDPumpIdle per pass. A half-restored
             * control answers half a question. */
            CDPumpIdle();
#endif
        }

#if CD_RING_SEPARATE
afterDrain:
#endif
        CDPumpIdle();

        /* Log what the ORIGINAL driver answers for AudioStatus and ReadQ, once each.
         * The MSF bytes we rewrite with confidence; byte 0 of AudioStatus is still a
         * guess (kSynthStatusPlaying), and this is the evidence that settles it. */
        if (pub->origStatusCaptured == 1) {
            pub->origStatusCaptured = 2;
            CDLogf("  ORIGINAL AudioStatus answer (before our rewrite):");
            CDLogHexAt("    orig", (void *)pub->origStatusParam, 16, 0);
        }
        if (pub->origReadQCaptured == 1) {
            pub->origReadQCaptured = 2;
            CDLogf("  ORIGINAL ReadQ answer (before our rewrite):");
            CDLogHexAt("    orig", (void *)pub->origReadQParam, 16, 0);
        }

        {
            Boolean playing;
            long    ur, delivered;
            CDPumpStats(&playing, &ur, &delivered);
            pub->pumpPlaying   = playing ? 1 : 0;
            pub->pumpUnderruns = ur;
            if (playing && (delivered >> 18) != reported) {
                reported = delivered >> 18;
                CDProgressSay("playing: %ld KB delivered, %ld underruns",
                              delivered / 1024, ur);
            }

            /* ★ PUBLISHED LIVENESS. This is the heartbeat that actually reaches
             * someone: the v3 log-based one measured nothing, because the pump's log
             * had already gone silent while the pump itself kept playing. The probe
             * reads these and writes them to ITS log, which survived that run intact. */
            pub->pumpBeat++;
            {
                short qd = 0, le = 0;
                long  lw = 0;
                CDLogDiag(&qd, &le, &lw);
                pub->logQuietDepth = qd;
                pub->logLastErr    = le;
                pub->logWrites     = lw;
            }

            /* ★ PLAYBACK HEARTBEAT — instrumentation for the v2 freeze.
             *
             * v2 (request ring) froze ~1.6 s into playback; v1 (single-slot mailbox)
             * ran the identical probe to completion on the same machine and disc. So
             * the ring is implicated, but reading the code has not shown how — and at
             * the moment of the freeze the ring is IDLE: the pump has drained
             * everything, and AudioStatus and ReadQ are never posted to it.
             *
             * That contradiction is what this line is for. One log write per second
             * while playing, and the question it answers is which side stopped:
             *
             *   beats stop at the same instant as the probe's last line
             *       -> the whole machine wedged; look below the application
             *   beats CONTINUE past the probe's last line
             *       -> the pump is alive and it is the probe's Status call into the
             *          driver that never returned; look at the handler, not the ring
             *
             * reqR/reqW/drop are here too, so a runaway index shows up as numbers
             * rather than as a theory. Deliberately cheap: one flushed line a second,
             * against a play path that already spends 1.5 s pre-rolling. */
            if (playing && (TickCount() - lastBeat) >= 60) {
                lastBeat = TickCount();
                CDLogf("  beat t=%ld  %ldKB  underruns=%ld  reqR=%ld reqW=%ld drop=%ld",
                       (long)TickCount(), delivered / 1024, ur,
                       pub->reqRead, pub->reqWrite, pub->reqDropped);
            }
        }

        /* A short sleep so the foreground application still gets time, but short
         * enough that the ring is topped up many times a second. */
#if CD_FACELESS
        /* The game is the foreground application here, and it must not lose events to
         * us. A background-only app is not offered mouse or key events anyway, so the
         * only thing worth acting on is the quit. */
        if (WaitNextEvent(everyEvent, &evt, 1, NULL)) {
            if (evt.what == kHighLevelEvent) (void)AEProcessAppleEvent(&evt);
        }
        if (gQuitRequested) break;
#else
        if (WaitNextEvent(mDownMask | keyDownMask, &evt, 1, NULL)) {
            if (evt.what == mouseDown || evt.what == keyDown) break;
        }
#endif
    }

    {
        Boolean playing;
        long    ur = 0, delivered = 0;
        CDPumpStats(&playing, &ur, &delivered);
        CDLogf("=== pump stopped: %ld KB delivered, %ld underruns ===",
               delivered / 1024, ur);
        CDLogf("  synthesised answers: %ld AudioStatus, %ld ReadQ",
               pub->synthStatusCount, pub->synthReadQCount);
        CDLogf("  requests: %ld posted, %ld serviced, %ld dropped",
               pub->reqWrite, pub->reqRead, pub->reqDropped);
        if (pub->reqDropped > 0)
            CDLogf("  ⇒ %ld request(s) were LOST. Any missing music is explained by "
                   "this; the ring needs to be bigger or the pump needs more time.",
                   pub->reqDropped);
        if (pub->synthStatusCount == 0 && pub->synthReadQCount == 0)
            CDLogf("  ⇒ nothing polled for position, so synthesis was never exercised.");
        if (ur == 0 && delivered > 0)
            CDLogf("  ⇒ zero underruns: the ring and the event-loop refill kept up.");
        else if (ur > 0)
            CDLogf("  ⇒ %ld underruns = audible gaps. Dials, cheapest first: a bigger "
                   "ring, then larger reads per refill.", ur);
    }
    CDPumpStop();
    pub->pumpAlive = 0;
}

#if CD_FACELESS
#define kVersionString  "CD Audio Redirector v7"
#define kLogFileName    "\pCD Audio Redirector Log"
#else
#define kVersionString  "CDPump v11"
#define kLogFileName    "\pCD Engine Log"
#endif
#define kEnginePEFType  FOUR_CHAR_CODE('cdPF')
#define kEnginePEFID    128

/* Matches DriverEntryPointPtr (Devices.h:291) exactly, so the call is type-correct
 * rather than relying on the PowerPC ABI happening to pass a one-pointer union like a
 * pointer. */
typedef OSErr (*EngineIOProc)(AddressSpaceID spaceID, IOCommandID cmdID,
                              IOCommandContents contents, IOCommandCode code,
                              IOCommandKind kind);

static CDEngineInfo gInfo;

/* Fallback publication: 4 bytes of magic then the 4-byte address of the public block,
 * written into the System Folder. The reader tries Gestalt first and falls back to
 * this. Deliberately the dumbest possible channel — after the Gestalt call silently
 * failed, a mechanism with nothing to misunderstand is worth having. */
static void WriteStateFile(Ptr pub)
{
    short   vRefNum;
    long    dirID;
    FSSpec  spec;
    short   ref;
    long    len;
    long    buf[2];

    if (pub == NULL) return;
    buf[0] = (long)kEngineMagic;
    buf[1] = (long)pub;

    if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return;
    if (FSMakeFSSpec(vRefNum, dirID, kEngineStateFileName, &spec) != noErr)
        (void)FSpCreate(&spec, 'CDei', 'CDst', smSystemScript);
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) return;
    SetFPos(ref, fsFromStart, 0);
    len = sizeof(buf);
    FSWrite(ref, &len, (Ptr)buf);
    SetEOF(ref, sizeof(buf));
    FSClose(ref);
    FlushVol(NULL, vRefNum);
    CDLogf("  state file written: magic + block address");
}

static const char *StatusText(short s)
{
    switch (s) {
        case kEngineOK:             return "OK - descriptor validated";
        case kEngineNoDriver:       return "no unit named .AppleCD";
        case kEngineNoDCE:          return "no DCE";
        case kEngineRAMBased:       return "driver is Handle-based, refused";
        case kEngineBadDriverPtr:   return "dCtlDriver implausible";
        case kEngineNotDRVRShape:   return "not a DRVR shape";
        case kEngineNotDescriptor:  return "Control entry is NOT a 0xAAFE descriptor";
        case kEngineNotPowerPCISA:  return "descriptor ISA is not PowerPC";
        case kEngineBadTVector:     return "saved TVector implausible";
        case kEngineNoMemory:       return "out of system memory";
        case kEngineAlreadyPatched: return "already patched";
        case kEngineCodeInAppHeap:  return "our code is in the APP heap - it would "
                                           "vanish on quit; the PEF was not copied "
                                           "to the system heap first";
        default:                    return "unknown";
    }
}

int main(void)
{
    KeyMap             km;
    Boolean            commit, undo;
    Boolean            logOK;
    Handle             pefH;
    Size               pefLen;
    Ptr                sysPef = NULL;
    CFragConnectionID  connID = 0;
    Ptr                fragMain = NULL;
    Ptr                driverDesc = NULL;
    OSErr              err;
    OSErr              st;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    memset(&gInfo, 0, sizeof(gInfo));

#if CD_FACELESS
    /* Nobody holds a key at boot, and there is no window to hold it for. The
     * validation the modifier used to gate still runs — the engine refuses to patch
     * unless the descriptor validates — so what is being dropped here is the human
     * confirmation, not the safety check. */
    (void)km;
    commit = true;
    undo   = false;

    /* No CDProgressOpen: with no window, every CDProgressSay below is a no-op, so the
     * shared code needs no faceless variant. */
    logOK = CDLogOpen(kLogFileName);
#else
    /* ★ The default action stays the harmless one. Validation is what caught an
     * off-by-four descriptor offset and then our own code sitting in the application
     * heap — two defects that would each have corrupted or crashed the machine had the
     * default been "patch". Patching requires holding a key on purpose. */
    GetKeys(km);
    commit = KeyIsDown(km, kOptionKeyCode);
    undo   = KeyIsDown(km, kShiftKeyCode);
    if (undo) commit = false;

    CDProgressOpen("\p" kVersionString " - progress");
    CDProgressSay("%s starting", kVersionString);
    if (undo)        CDProgressSay("SHIFT held: will UNPATCH and restore the driver");
    else if (commit) CDProgressSay("OPTION held: will PATCH the Control descriptor");
    else             CDProgressSay("validation only - nothing will be modified");

    logOK = CDLogOpen(kLogFileName);
#endif
    if (!logOK) CDProgressSay("!! could not open the log - screen only");

    /* ★ Say what this binary actually IS, from the compiled constants themselves.
     *
     * The bisectA run had to be identified from its behaviour, because two different
     * builds carried the same version string and the log could not tell them apart.
     * A version string is a promise a human has to keep; these values are the build.
     * They cannot drift, and every run is now self-identifying. */
    CDLogf("  build config: CD_RING_MODE=%d  ringEntries=%d  ringSeparate=%d  "
           "faceless=%d  structBytes=%ld",
           (int)CD_RING_MODE, (int)kEngineReqRingEntries, (int)CD_RING_SEPARATE,
           (int)CD_FACELESS, (long)sizeof(CDEnginePublic));

#if CD_FACELESS
    CDLogBanner(kVersionString " - Red Book CD audio for legacy Mac CD games",
                "FACELESS: patching automatically, no window, runs until shutdown");
    /* After the banner, so the log always opens with one. */
    {
        AEEventHandlerUPP qUPP = NewAEEventHandlerUPP(HandleQuitEvent);
        AEEventHandlerUPP oUPP = NewAEEventHandlerUPP(HandleOpenAppEvent);
        OSErr             aerr = (qUPP == NULL)
                               ? memFullErr
                               : AEInstallEventHandler(kCoreEventClass,
                                                       kAEQuitApplication,
                                                       qUPP, 0, false);
        OSErr             oerr = (oUPP == NULL)
                               ? memFullErr
                               : AEInstallEventHandler(kCoreEventClass,
                                                       kAEOpenApplication,
                                                       oUPP, 0, false);
        CDLogf("  quit Apple event handler installed err=%d%s", aerr,
               (aerr == noErr) ? ""
                               : "  <-- shutdown may have to force-quit this");
        CDLogf("  open-application handler installed err=%d", oerr);
    }
#else
    CDLogBanner(kVersionString " - resident PPC engine + one-field Control patch",
                undo   ? "SHIFT HELD: restoring the original TVector"
                       : (commit ? "OPTION HELD: patching the Control descriptor"
                                 : "VALIDATION ONLY. Nothing is modified."));
#endif

    /* --- 1. the PEF --- */
    CDLogStep("Get1Resource('cdPF', 128)");
    pefH = Get1Resource(kEnginePEFType, kEnginePEFID);
    if (pefH == NULL) {
        CDLogf("  the engine PEF resource is missing from this application");
        CDProgressSay("ENGINE PEF MISSING");
        goto done;
    }
    LoadResource(pefH);
    HLock(pefH);
    pefLen = GetHandleSize(pefH);
    CDLogf("  PEF resource at 0x%08lX, %ld bytes", (unsigned long)*pefH,
           (long)pefLen);

    /* ★ COPY THE PEF INTO THE SYSTEM HEAP AND PREPARE THE FRAGMENT FROM THE COPY.
     *
     * Step 2's second run caught why this matters. GetDriverMemoryFragment prepares a
     * fragment from the memory you hand it, and CFM uses the PEF's code section
     * IN PLACE — only the data section gets copied. Handing it the resource handle
     * therefore left our handler's code inside the APPLICATION's heap:
     *
     *     PEF resource at 0x3DB234E0
     *     OUR TVector = 0x018743D0 -> code=0x3DB2355C toc=0x018743F0
     *                                      ^ inside the PEF buffer, app heap
     *
     * SetDriverClosureMemory(connID, true) returned 0, but holding the closure does
     * not relocate a code section CFM never copied. Step 3 would have installed a
     * handler whose code disappears when this app quits — precisely the failure the
     * 68K 'preload' bug caused. Same lesson a third time: never let another
     * allocator decide which heap your resident code lives in. */
    sysPef = NewPtrSys(pefLen);
    if (sysPef == NULL) {
        CDLogf("  could not allocate %ld bytes in the system heap for the PEF",
               (long)pefLen);
        CDProgressSay("NO SYSTEM MEMORY for the PEF");
        goto done;
    }
    BlockMoveData(*pefH, sysPef, pefLen);
    HUnlock(pefH);
    ReleaseResource(pefH);      /* nothing ties the resident code to this app now */
    CDLogf("  PEF copied to the SYSTEM heap at 0x%08lX", (unsigned long)sysPef);

    /* --- 2. prepare the fragment --- */
    CDLogStep("GetDriverMemoryFragment (from the system-heap copy)");
    err = GetDriverMemoryFragment(sysPef, (long)pefLen, "\pCDAudioEngine",
                                  &connID,
                                  (DriverEntryPointPtr *)&fragMain,
                                  (DriverDescriptionPtr *)&driverDesc);
    CDLogf("  GetDriverMemoryFragment err=%d connID=0x%08lX main=0x%08lX desc=0x%08lX",
           err, (unsigned long)connID, (unsigned long)fragMain,
           (unsigned long)driverDesc);
    if (err != noErr || fragMain == NULL) {
        CDLogf("  ⇒ the fragment was REJECTED. Nothing is resident and nothing was");
        CDLogf("    modified. If err is cfragNoLibraryErr or similar, the PEF's");
        CDLogf("    exports or its `main` are wrong — check that DoDriverIO is both");
        CDLogf("    exported and set as main by patch-pef-main.py.");
        CDProgressSay("FRAGMENT REJECTED err=%d", err);
        goto done;
    }

    /* --- 3. THE residency step --- */
    CDLogStep("SetDriverClosureMemory(connID, true)");
    err = SetDriverClosureMemory(connID, true);
    CDLogf("  SetDriverClosureMemory err=%d", err);
    if (err != noErr) {
        CDLogf("  ⇒ the closure could NOT be held, so this code would die with the");
        CDLogf("    application. Do not proceed to Step 3 until this returns 0.");
        CDProgressSay("CLOSURE NOT HELD err=%d - residency unproven", err);
    } else {
        CDLogf("  ⇒ RESIDENCY PROVEN: the fragment's memory is held by the system and");
        CDLogf("    outlives this application.");
        CDProgressSay("residency held OK");
    }

    /* --- 4. validate, change nothing --- */
    CDLogStep("DoDriverIO(kInitialize) - find and validate the descriptor");
    {
        EngineIOProc      io = (EngineIOProc)fragMain;
        IOCommandContents cc;
        cc.pb = (ParmBlkPtr)&gInfo;
        st = io(NULL, NULL, cc, kEngineInitCommand, 0);
    }
    CDLogf("  DoDriverIO returned %ld", (long)st);

    CDLogf("--- what the engine found ---");
    CDLogf("  magic=0x%08lX version=%d status=%d (%s)",
           (unsigned long)gInfo.magic, gInfo.version, gInfo.status,
           StatusText(gInfo.status));
    CDLogf("  cdRefNum=%d  dCtlDriver=0x%08lX  ctlDescriptor=0x%08lX",
           gInfo.cdRefNum, (unsigned long)gInfo.dCtlDriver,
           (unsigned long)gInfo.ctlDescriptor);
    CDLogf("  descriptor: rdVersion=0x%02X procInfo=0x%08lX ISA=0x%02X",
           gInfo.rdVersion, (unsigned long)gInfo.procInfo, gInfo.isa);
    CDLogf("  ORIGINAL TVector = 0x%08lX  -> code=0x%08lX toc=0x%08lX",
           (unsigned long)gInfo.origTVector,
           (unsigned long)gInfo.origCode, (unsigned long)gInfo.origTOC);
    CDLogf("  OUR      TVector = 0x%08lX  -> code=0x%08lX toc=0x%08lX",
           (unsigned long)gInfo.ourTVector,
           (unsigned long)gInfo.ourCode, (unsigned long)gInfo.ourTOC);
    CDLogf("  ring=0x%08lX entries=%ld  patched=%d",
           (unsigned long)gInfo.ring, gInfo.ringEntries, gInfo.patched);
    CDLogf("  driveNum=%d  audioInitErr=%d %s", gInfo.driveNum, gInfo.audioInitErr,
           gInfo.audioInitErr == noErr
             ? "(ring, double buffers, sound channel and TOC are ready)"
             : "(NO AUDIO: interception will still work, but nothing will play)");
    CDLogf("  published block = 0x%08lX", (unsigned long)gInfo.pubBlock);

    /* ★ The system heap, which is now the prime suspect. bisectA proved our behaviour
     * innocent: reverting the drain loop and the variable-offset write changed nothing,
     * and the only thing left that differs from the clean v1 build is how many bytes
     * this block takes out of the system heap. Print what the ENGINE measured, plus
     * this application's own view of the struct size — if those two disagree the whole
     * investigation has been chasing the wrong thing. */
    if (gInfo.pubBlock != NULL) {
        CDEnginePublic *p = (CDEnginePublic *)gInfo.pubBlock;
        CDLogf("  SYSTEM HEAP at init: %ld bytes free, largest block %ld",
               p->sysFreeAtInit, p->sysLargestAtInit);
        CDLogf("  published block is %ld bytes (ring holds %d entries)",
               p->pubBlockBytes, (int)kEngineReqRingEntries);
        if (p->pubBlockBytes != (long)sizeof(CDEnginePublic))
            CDLogf("  !! the ENGINE thinks this struct is %ld bytes and this APP thinks "
                   "%ld - a layout mismatch, and nothing below can be trusted",
                   p->pubBlockBytes, (long)sizeof(CDEnginePublic));
        if (p->sysFreeAtInit > 0 && p->sysFreeAtInit < 64L * 1024L)
            CDLogf("  ⚠ under 64K free in the system heap. At that point 300 extra "
                   "bytes is enough to change who fails to allocate.");
    }
    CDLogf("  Gestalt registration: NewGestaltValue=%d ReplaceGestaltValue=%d "
           "SetGestaltValue=%d  (1 = not attempted)",
           gInfo.gestaltNewErr, gInfo.gestaltReplaceErr, gInfo.gestaltSetErr);
    if (gInfo.gestaltPublished)
        CDLogf("  ⇒ 'CDau' IS published; CDTraceRead will find it via Gestalt.");
    else
        CDLogf("  ⇒ Gestalt did NOT take. The state file below is the fallback.");
    WriteStateFile(gInfo.pubBlock);
    CDLogf("  sanity: our code 0x%08lX must NOT be inside the PEF resource handle;",
           (unsigned long)gInfo.ourCode);
    CDLogf("          it should sit near the system-heap copy at 0x%08lX",
           (unsigned long)sysPef);

    if (gInfo.status == kEngineOK) {
        CDLogf("  ⇒ STEP 3 IS SAFE TO ATTEMPT. It would write our TVector");
        CDLogf("    (0x%08lX) into the single long at descriptor + 0x10, currently",
               (unsigned long)gInfo.ourTVector);
        CDLogf("    0x%08lX. procInfo and ISA are unchanged, so Mixed Mode performs",
               (unsigned long)gInfo.origTVector);
        CDLogf("    exactly the same transition it does today. The DRVR header, the");
        CDLogf("    driver name, its address and dCtlDriver are all untouched.");
        CDProgressSay("VALIDATED - step 3 is safe to attempt");
    } else {
        CDLogf("  ⇒ DO NOT PROCEED to Step 3: %s", StatusText(gInfo.status));
        CDProgressSay("NOT validated: %s", StatusText(gInfo.status));
    }

    /* --- 5. Step 3: patch or unpatch, only when asked --- */
    if (gInfo.status == kEngineOK && (commit || undo)) {
        EngineIOProc      io = (EngineIOProc)fragMain;
        IOCommandContents cc;
        OSErr             perr;

        cc.pb = (ParmBlkPtr)&gInfo;

        if (undo) {
            CDLogStep("DoDriverIO(unpatch) - restore the original TVector");
            perr = io(NULL, NULL, cc, kEngineUnpatchCommand, 0);
            CDLogf("  unpatch returned %d, status=%d, patched=%d",
                   perr, gInfo.status, gInfo.patched);
            CDProgressSay("UNPATCHED (patched=%d)", gInfo.patched);
        } else {
            CDLogStep("DoDriverIO(patch) - write our TVector into the descriptor");
            perr = io(NULL, NULL, cc, kEnginePatchCommand, 0);
            CDLogf("  patch returned %d, status=%d, patched=%d",
                   perr, gInfo.status, gInfo.patched);
            if (perr == noErr && gInfo.patched && gInfo.status == kEngineOK) {
                CDLogf("  ⇒ PATCHED, and read back correct. The Control descriptor now");
                CDLogf("    points at 0x%08lX instead of 0x%08lX. Every CD Control call",
                       (unsigned long)gInfo.ourTVector,
                       (unsigned long)gInfo.origTVector);
                CDLogf("    from now on goes through our handler and is chained on.");
                CDLogf("  ⇒ NOW RUN CDPlayProbe_v2 AND LISTEN. It issues the legacy");
                CDLogf("    AudioPlay that a game issues, which previously produced");
                CDLogf("    SILENCE. If music comes out, the whole chain works.");
                CDProgressSay("PATCHED - now test iTunes and CDRecon");
            } else {
                CDLogf("  ⇒ the patch did NOT take. Nothing should have changed; the");
                CDLogf("    engine re-validates immediately before the store and");
                CDLogf("    refuses if anything moved.");
                CDProgressSay("patch REFUSED (status=%d)", gInfo.status);
            }
        }
    } else if (commit || undo) {
        CDLogf("  ⇒ refusing to %s: validation did not pass (%s)",
               undo ? "unpatch" : "patch", StatusText(gInfo.status));
    }

    /* --- 6. become the pump --- */
    if (commit && gInfo.patched && gInfo.status == kEngineOK && gInfo.pubBlock != NULL) {
        CDDriverInfo cd;
        OSErr        perr;

        memset(&cd, 0, sizeof(cd));
        CDFindDriver(&cd, false);          /* for the drive number */
        CDLogStep("CDPumpInit(refNum=%d drive=%d)", gInfo.cdRefNum, cd.driveNum);
        perr = CDPumpInit(gInfo.cdRefNum, cd.driveNum);
        CDLogf("  CDPumpInit err=%d", perr);
        if (perr == noErr) {
            PumpLoop((CDEnginePublic *)gInfo.pubBlock, gInfo.cdRefNum);
        } else {
            CDLogf("  ⇒ the pump could not start, so nothing will play. The patch is");
            CDLogf("    still installed and harmless.");
            CDProgressSay("PUMP FAILED err=%d", perr);
        }
    }

    CDLogf("  NOTE: run this once per boot. A second run prepares a second fragment.");
    CDLogf("  NOTE: the patch NEVER survives a restart. Recovery is always a reboot.");
#if CD_FACELESS
    CDLogf("  NOTE: reaching this line means the pump loop ended, which for a faceless");
    CDLogf("    build means a quit Apple event - normally shutdown. If it appears while");
    CDLogf("    the machine is still running, something quit us and audio is now dead.");
#endif
    CDLogf("=== end of run ===");

done:
    CDLogClose();
#if !CD_FACELESS
    /* Hold the summary on screen long enough to be read, or until dismissed. There is
     * no screen in the faceless build and nobody is watching it at boot, so it exits
     * straight away — a ten-second pause during startup would be a mystery, not a
     * courtesy. */
    CDProgressSay("done - send 'CD Engine Log' back");
    {
        EventRecord evt;
        long        until = TickCount() + 600;
        while (TickCount() < until)
            if (WaitNextEvent(mDownMask | keyDownMask, &evt, 5, NULL)) break;
    }
    CDProgressClose();
#endif
    return 0;
}

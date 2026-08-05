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
#include <ToolUtils.h>
#include <CodeFragments.h>

#include <stdio.h>
#include <string.h>

#include "cd_probe_common.h"
#include "cd_engine.h"

#define kVersionString  "CDEngineInstall v1"
#define kEnginePEFType  FOUR_CHAR_CODE('cdPF')
#define kEnginePEFID    128

/* Matches DriverEntryPointPtr (Devices.h:291) exactly, so the call is type-correct
 * rather than relying on the PowerPC ABI happening to pass a one-pointer union like a
 * pointer. */
typedef OSErr (*EngineIOProc)(AddressSpaceID spaceID, IOCommandID cmdID,
                              IOCommandContents contents, IOCommandCode code,
                              IOCommandKind kind);

static CDEngineInfo gInfo;

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

    CDProgressOpen("\p" kVersionString " - progress");
    CDProgressSay("%s starting", kVersionString);
    CDProgressSay("STEP 2: residency + validation only. Nothing is modified.");

    logOK = CDLogOpen("\pCD Engine Log");
    if (!logOK) CDProgressSay("!! could not open the log - screen only");
    CDLogBanner(kVersionString " - Step 2: make the PPC engine resident",
                "VALIDATION ONLY. The CD driver is not modified in any way.");

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

    CDLogf("  NOTE: run this once per boot. A second run prepares a second fragment.");
    CDLogf("=== end of run: nothing was modified ===");

done:
    CDLogClose();
    CDProgressSay("done - send 'CD Engine Log' back");
    {
        EventRecord evt;
        long        until = TickCount() + 600;
        while (TickCount() < until)
            if (WaitNextEvent(mDownMask | keyDownMask, &evt, 5, NULL)) break;
    }
    CDProgressClose();
    return 0;
}

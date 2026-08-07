/*
 * CDTraceRead — reads the resident PowerPC engine's trace ring.
 *
 * The engine's globals live in its own CFM data section, which no other process can
 * locate, so the engine publishes one system-heap block through a value-based Gestalt
 * selector ('CDau'). This app asks Gestalt for it and decodes the ring. No driver is
 * touched and nothing is modified.
 *
 * Replaces the 68K-generation CDTraceDump, which found the ring through the patched
 * dCtlDriver. That anchor is gone by design: the new patch leaves dCtlDriver, the DRVR
 * header and the driver's name completely alone, changing only the Control descriptor's
 * TVector. Nothing about the driver reveals us any more, which is exactly why iTunes
 * and Audio CD Access carried on working — and why a publication channel was needed.
 *
 * WHAT TO LOOK FOR
 *   - patched=1 and a rising callCount: the handler is live and being called.
 *   - audioCallCount > 0: something issued one of the csCodes we will service in the
 *     next step.
 *   - ioTrap bit 9 (noQueueBit): immediate vs queued. Phase 2a established that
 *     AudioStatus and ReadQ arrive QUEUED, so the engine must complete those itself
 *     rather than chaining and rewriting.
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
#include <Memory.h>
#include <OSUtils.h>
#include <ToolUtils.h>
#include <Gestalt.h>
#include <Folders.h>

#include <stdio.h>
#include <string.h>

#include "cd_probe_common.h"
#include "cd_cscodes.h"
#include "cd_engine.h"

#define kVersionString  "CDTraceRead v4"

/* noQueueBit is bit 9 of the trap word. An immediate call ends in RTS; a queued one
 * must end at jIODone. */
#define kNoQueueBit     0x0200


static const char *CsCodeName(short cs)
{
    switch (cs) {
        case kcsKillIO:            return "KillIO";
        case kcsVerifyTheDisc:     return "VerifyTheDisc";
        case kcsFormatTheDisc:     return "FormatTheDisc";
        case kcsEjectTheDisc:      return "EjectTheDisc";
        case kcsGetDriveIcon:      return "GetDriveIcon";
        case kcsGetMediaIcon:      return "GetMediaIcon";
        case kcsGetDriveInfo:      return "GetDriveInfo";
        case kcsAccRun:            return "accRun (periodic, TASK level)";
        case kcsSetPowerMode:      return "SetPowerMode";
        case kcsModifyPostEvent:   return "ModifyPostEvent";
        case kcsChangeBlockSize:   return "ChangeBlockSize";
        case kcsSetUserEject:      return "SetUserEject";
        case kcsSetPollFreq:       return "SetPollFreq";
        case kcsReadTOC:           return "ReadTOC";
        case kcsReadTheQSubcode:   return "ReadQ";
        case kcsAudioTrackSearch:  return "*AudioTrackSearch";
        case kcsAudioPlay:         return "*AudioPlay";
        case kcsAudioPause:        return "*AudioPause";
        case kcsAudioStop:         return "*AudioStop";
        case kcsAudioStatus:       return "*AudioStatus";
        case kcsAudioScan:         return "*AudioScan";
        case kcsAudioControl:      return "*AudioControl (volume)";
        case kcsReadAudioVolume:   return "*ReadAudioVolume";
        case kcsGetSpindleSpeed:   return "GetSpindleSpeed";
        case kcsSetSpindleSpeed:   return "SetSpindleSpeed";
        case kcsGetPlayMode:       return "GetPlayMode";
        case kcsDriverGestalt:     return "DriverGestalt";
        default:                   return "?";
    }
}

int main(void)
{
    Boolean         logOK;
    long            gv = 0;
    OSErr           gerr;
    CDEnginePublic *pub;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    CDProgressOpen("\p" kVersionString " - progress");
    CDProgressSay("%s starting", kVersionString);

    logOK = CDLogOpen("\pCD Trace Log");
    if (!logOK) CDProgressSay("!! could not open the log - screen only");
    CDLogBanner(kVersionString " - read the resident engine's trace ring",
                "read-only; nothing is modified");

    CDLogStep("Gestalt('CDau')");
    gerr = Gestalt(kEnginePublicSelector, &gv);
    CDLogf("  Gestalt err=%d value=0x%08lX", gerr, (unsigned long)gv);
    if (gerr == -5551)
        CDLogf("  (-5551 = gestaltUndefSelectorErr: nothing registered the selector)");

    /* Fallback: the state file the installer writes. Gestalt registration failed
     * outright on the first attempt, so the reader no longer depends on it. */
    if (gerr != noErr || gv == 0) {
        short   vRefNum;
        long    dirID;
        FSSpec  spec;
        short   ref;
        long    len;
        long    buf[2];

        CDLogStep("fallback: read the state file");
        buf[0] = 0; buf[1] = 0;
        if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder,
                       &vRefNum, &dirID) == noErr &&
            FSMakeFSSpec(vRefNum, dirID, kEngineStateFileName, &spec) == noErr &&
            FSpOpenDF(&spec, fsRdPerm, &ref) == noErr) {
            len = sizeof(buf);
            (void)FSRead(ref, &len, (Ptr)buf);
            FSClose(ref);
            CDLogf("  state file: magic=0x%08lX addr=0x%08lX",
                   (unsigned long)buf[0], (unsigned long)buf[1]);
            if ((OSType)buf[0] == kEngineMagic && buf[1] != 0) {
                gv = buf[1];
                CDLogf("  ⇒ using the address from the state file");
            }
        } else {
            CDLogf("  no state file (or unreadable)");
        }
    }

    if (gv == 0) {
        CDLogf("=== THE ENGINE IS NOT RESIDENT ===");
        CDLogf("  Nothing has published 'CDau', so either CDEngineInstall has not been");
        CDLogf("  run this boot, or it declined. The patch never survives a restart, so");
        CDLogf("  this is the expected state after any reboot. Run CDEngineInstall_v2");
        CDLogf("  (hold option to patch) and try again.");
        CDProgressSay("engine NOT resident - run CDEngineInstall first");
        goto done;
    }

    pub = (CDEnginePublic *)gv;
    if (pub->magic != kEngineMagic) {
        CDLogf("  magic at that address is 0x%08lX, expected 'CDE1' (0x%08lX)",
               (unsigned long)pub->magic, (unsigned long)kEngineMagic);
        CDLogf("  ⇒ refusing to read further: the selector points at something that is");
        CDLogf("    not ours.");
        CDProgressSay("BAD MAGIC - not our block");
        goto done;
    }

    /* ★ The layout is not stable across engine versions — version 2 replaced the
     * single-slot request mailbox with a ring, moving every field after it. Reading a
     * version we were not built against would print confident nonsense, which is worse
     * than refusing. */
    if (pub->version != kEngineVersion) {
        CDLogf("  the engine reports version %d; this reader was built for version %d",
               pub->version, kEngineVersion);
        CDLogf("  ⇒ refusing to read further: the field offsets differ, so every number");
        CDLogf("    below would be nonsense. Rebuild both from the same tree.");
        CDProgressSay("VERSION MISMATCH %d vs %d", pub->version, kEngineVersion);
        goto done;
    }

    CDLogf("=== the resident engine ===");
    CDLogf("  version=%d  patched=%d  cdRefNum=%d",
           pub->version, pub->patched, pub->cdRefNum);
    CDLogf("  requests: %ld posted, %ld serviced, %ld dropped",
           pub->reqWrite, pub->reqRead, pub->reqDropped);
    if (pub->reqDropped > 0)
        CDLogf("  ⚠ %ld request(s) were DROPPED before the pump saw them. The ring "
               "overflowed, which means the pump is not getting enough time.",
               pub->reqDropped);
    CDLogf("  origTVector=0x%08lX  ourTVector=0x%08lX",
           (unsigned long)pub->origTVector, (unsigned long)pub->ourTVector);
    CDLogf("  ring=0x%08lX entries=%ld", (unsigned long)pub->ring, pub->ringEntries);
    CDLogf("  callCount=%ld  audioCallCount=%ld  writeCount=%ld",
           pub->callCount, pub->audioCallCount, pub->writeCount);
    CDProgressSay("patched=%d  %ld calls, %ld audio",
                  pub->patched, pub->callCount, pub->audioCallCount);

    if (!pub->patched)
        CDLogf("  ⚠ patched=0: the engine is resident but the descriptor is NOT ours,");
    if (pub->callCount == 0)
        CDLogf("  ⚠ callCount=0: nothing has reached the handler yet. Touch the drive"
               " (iTunes, CDRecon) and read again.");

    /* --- the ring, oldest first --- */
    if (pub->ring != NULL && pub->ringEntries > 0) {
        CDEngineTrace *ring  = (CDEngineTrace *)pub->ring;
        long           total = pub->writeCount;
        long           have  = (total < pub->ringEntries) ? total : pub->ringEntries;
        long           first = total - have;
        long           i, immediate = 0, queued = 0;

        CDLogf("=== trace ring: %ld entries, oldest first ===", have);
        for (i = 0; i < have; i++) {
            long           idx = (first + i) & (pub->ringEntries - 1);
            CDEngineTrace *e   = &ring[idx];
            Boolean        imm = (e->ioTrap & kNoQueueBit) != 0;

            if (imm) immediate++; else queued++;

            CDLogf("  %5ld  t=%-10lu cs=%-3d %-30s ioTrap=0x%04X %s",
                   first + i, e->ticks, e->csCode, CsCodeName(e->csCode),
                   e->ioTrap, imm ? "IMMEDIATE" : "queued");
            CDLogHexAt("    csParam", e->csParam, 8, 0);
        }
        CDLogf("  immediate=%ld queued=%ld", immediate, queued);
        CDLogf("  (a csCode marked * is one the engine will service rather than chain)");
    }

    CDLogf("=== end of run ===");

done:
    CDLogClose();
    CDProgressSay("done - send 'CD Trace Log' back");
    {
        EventRecord evt;
        long        until = TickCount() + 400;
        while (TickCount() < until)
            if (WaitNextEvent(mDownMask | keyDownMask, &evt, 5, NULL)) break;
    }
    CDProgressClose();
    return 0;
}

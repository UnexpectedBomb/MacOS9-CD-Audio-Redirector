/*
 * CDTraceDump — reads what the Phase-2a patch recorded.
 *
 * The patch (a 68K INIT + resident blob) replaces the CD driver's DCE `dCtlDriver`
 * with a DRVR shell of our own, and logs every Control call that passes through into
 * a ring in the system heap. This app finds all of that from the DCE alone: read
 * `dCtlDriver`, check for our magic at offset 0x40, follow the pointers. No Gestalt
 * selector is involved, which avoids installing a 68K callback that the native
 * Gestalt Manager would have to call.
 *
 * WHAT THE OUTPUT IS FOR
 * ----------------------
 *   1. Is the patch installed and is it being called at all? `dCtlDriver` pointing
 *      at our shell plus a rising `callCount` is the proof that the whole residency
 *      and interception mechanism works.
 *   2. ★ Are the audio calls IMMEDIATE or QUEUED? Every trace entry records
 *      `ioTrap`, and bit 9 is `noQueueBit`. This is the fact Phase 2b needs:
 *      Retro68's own libDRVRRuntime shows a Control routine ending in a plain RTS
 *      for an immediate call but jumping to `jIODone` for a queued one. If the games'
 *      audio calls are immediate, 2b can call the original and rewrite `csParam`
 *      afterwards, which is what synthesising AudioStatus and ReadQ requires.
 *   3. The real call sequence a game issues — the trace PHASE0.md §P5b deferred.
 *
 * ALSO AN UNINSTALLER. Hold OPTION at launch and it restores the original
 * `dCtlDriver` from the saved copy in the shell. That makes the patch testable
 * without a reboot between attempts, and it is the recovery path if the patch
 * misbehaves while the machine is still usable.
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

#include <stdio.h>
#include <string.h>

#include "cd_probe_common.h"
#include "cd_cscodes.h"
#include "cd_patch_shell.h"

#define kVersionString  "CDTraceDump v1"

/* noQueueBit is bit 9 of the trap word. An immediate call ends in RTS; a queued one
 * must end at jIODone. */
#define kNoQueueBit     0x0200

static CDDriverInfo gCD;

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
    KeyMap        km;
    Boolean       uninstall, logOK;
    DCtlHandle    dceH;
    DCtlPtr       dce;
    CDPatchShell *shell;
    CDPatchInfo  *info;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    GetKeys(km);
    uninstall = KeyIsDown(km, kOptionKeyCode);

    memset(&gCD, 0, sizeof(gCD));

    CDProgressOpen("\p" kVersionString " - progress");
    CDProgressSay("%s starting", kVersionString);
    if (uninstall) CDProgressSay("option held: will UNINSTALL the patch");

    logOK = CDLogOpen("\pCD Trace Log");
    if (!logOK) CDProgressSay("!! could not open the log - screen only");
    CDLogBanner(kVersionString " - Phase 2a trace ring dump",
                uninstall ? "OPTION HELD: restoring the original dCtlDriver"
                          : "read-only");

    CDFindDriver(&gCD, false);
    if (!gCD.found) {
        CDLogf("no CD driver found");
        CDProgressSay("NO CD DRIVER FOUND");
        goto done;
    }

    dceH = GetDCtlEntry(gCD.refNum);
    if (dceH == NULL || *dceH == NULL) {
        CDLogf("no DCE for refNum=%d", gCD.refNum);
        goto done;
    }
    dce   = *dceH;
    shell = (CDPatchShell *)dce->dCtlDriver;
    info  = &shell->info;

    CDLogf("dCtlDriver = 0x%08lX", (unsigned long)shell);

    if (shell == NULL || ((unsigned long)shell & 1) ||
        info->magic != kPatchMagic) {
        CDLogf("=== THE PATCH IS NOT INSTALLED ===");
        CDLogf("  magic at +0x40 = 0x%08lX, expected 'CDAU' (0x%08lX)",
               (unsigned long)info->magic, (unsigned long)kPatchMagic);
        CDLogf("  Either the extension is not in the Extensions folder, or shift");
        CDLogf("  was held at boot, or the INIT ran before .AppleCD loaded and");
        CDLogf("  refused to patch. Check 'CD Patch Log' in the System Folder for");
        CDLogf("  the INIT's own one-line verdict.");
        CDProgressSay("PATCH NOT INSTALLED - see CD Patch Log");
        goto done;
    }

    /* --- installed: report the shell --- */
    CDLogf("=== PATCH IS INSTALLED ===");
    CDLogf("  version=%d  origDriver=0x%08lX  ring=0x%08lX  ringEntries=%ld",
           info->version, (unsigned long)info->origDriver,
           (unsigned long)info->ring, info->ringEntries);
    CDLogf("  callCount=%ld  audioCallCount=%ld  writeCount=%ld",
           info->callCount, info->audioCallCount, info->writeCount);
    CDProgressSay("patch installed: %ld calls, %ld audio",
                  info->callCount, info->audioCallCount);

    CDLogf("  our shell's DRVR header: flags=0x%04X delay=%d "
           "open=0x%04X prime=0x%04X ctl=0x%04X status=0x%04X close=0x%04X",
           (unsigned short)shell->drvrFlags, shell->drvrDelay,
           (unsigned short)shell->drvrOpen, (unsigned short)shell->drvrPrime,
           (unsigned short)shell->drvrCtl, (unsigned short)shell->drvrStatus,
           (unsigned short)shell->drvrClose);
    CDLogf("  jump stubs (0x4EF9 + target; only Control should differ from the "
           "original's entries):");
    CDLogHexAt("stubs", shell->stubs, 32, kStubOpen);

    if (info->callCount == 0) {
        CDLogf("  ⚠ the patch is installed but has NEVER been called. Either no CD");
        CDLogf("    activity has happened yet, or the Device Manager is not routing");
        CDLogf("    through our shell. Touch the drive (insert a disc, or run");
        CDLogf("    CDRecon) and dump again before concluding anything.");
    }

    /* --- the ring, oldest first --- */
    if (info->ring != NULL && info->ringEntries > 0) {
        CDTraceEntry *ring  = (CDTraceEntry *)info->ring;
        long          total = info->writeCount;
        long          have  = (total < info->ringEntries) ? total
                                                          : info->ringEntries;
        long          first = total - have;
        long          i;
        long          immediate = 0, queued = 0;

        CDLogf("=== trace ring: %ld entries, oldest first ===", have);
        CDLogf("  (a csCode marked * is one Phase 2b will intercept)");

        for (i = 0; i < have; i++) {
            long          idx = (first + i) & (info->ringEntries - 1);
            CDTraceEntry *e   = &ring[idx];
            Boolean       imm = (e->ioTrap & kNoQueueBit) != 0;

            if (imm) immediate++; else queued++;

            CDLogf("  %5ld  t=%-10lu cs=%-3d %-30s ioTrap=0x%04X %s",
                   first + i, e->ticks, e->csCode, CsCodeName(e->csCode),
                   e->ioTrap, imm ? "IMMEDIATE" : "queued");
            CDLogHexAt("    csParam", e->csParam, 8, 0);
        }

        CDLogf("=== ★ the Phase 2b answer ===");
        CDLogf("  immediate (noQueueBit set) = %ld, queued = %ld",
               immediate, queued);
        if (queued == 0 && immediate > 0) {
            CDLogf("  ⇒ every observed call is IMMEDIATE, so a Control routine ends");
            CDLogf("    in a plain RTS. Phase 2b can therefore CALL the original");
            CDLogf("    and rewrite csParam afterwards, which is exactly what");
            CDLogf("    synthesising AudioStatus and ReadQ needs. Good news.");
        } else if (queued > 0) {
            CDLogf("  ⇒ queued calls DO occur. For those, the original ends at");
            CDLogf("    jIODone and never returns to us, so 2b cannot simply call");
            CDLogf("    it and post-process. Those csCodes must be answered");
            CDLogf("    entirely by us, completing the request ourselves the way");
            CDLogf("    libDRVRRuntime does: result in D0, then push jIODone and");
            CDLogf("    RTS. Check WHICH csCodes are queued in the list above -");
            CDLogf("    if only accRun is, the audio path is still safe.");
        }
    }

    /* --- optional uninstall --- */
    if (uninstall) {
        CDLogf("=== UNINSTALLING ===");
        if (info->origDriver == NULL) {
            CDLogf("  refusing: saved origDriver is NULL");
        } else {
            CDLogStep("restoring dCtlDriver to 0x%08lX",
                      (unsigned long)info->origDriver);
            dce->dCtlDriver = info->origDriver;
            CDLogf("  done. dCtlDriver = 0x%08lX",
                   (unsigned long)dce->dCtlDriver);
            CDLogf("  The shell and ring are deliberately NOT freed: a Control call");
            CDLogf("  already in flight could still be inside our handler, and");
            CDLogf("  leaking two small system-heap blocks until restart is far");
            CDLogf("  cheaper than freeing memory something might still be using.");
            CDProgressSay("UNINSTALLED - original driver restored");
        }
    }

    CDLogf("=== end of run ===");

done:
    CDLogClose();
    CDProgressSay("done - send 'CD Trace Log' back");
    {
        EventRecord evt;
        long        until = TickCount() + 300;
        while (TickCount() < until)
            if (WaitNextEvent(mDownMask | keyDownMask, &evt, 5, NULL)) break;
    }
    CDProgressClose();
    return 0;
}

/*
 * CDSoundRecon — why does the GAME never ask for CD audio?
 *
 * ★ THE FINDING THIS EXISTS TO CHASE
 * Warcraft plays music on the MDD and not on the G4 mini, and on the mini it makes
 * **zero Control calls to the CD driver** across a whole session - no AudioPlay, no
 * AudioStatus, not even a ReadTOC. Every instrument this project built watches the CD
 * driver, so all of them came back empty: the game's decision is taken somewhere else
 * and it never walks through that door.
 *
 * The Sound control panel says where. With the disc in, the MDD's Input tab lists
 * **CD, Line In, None**; the mini lists **nothing at all**. The mini has no audio input
 * hardware - no line-in jack and no analog CD wire - so that is very likely accurate
 * rather than broken. A game that asks "is there a CD sound source?" and is told no
 * would disable its music before ever addressing the drive.
 *
 * ⚠ But the control panel is a SYMPTOM, not an API. We do not know which call the game
 * makes, and faking the wrong one buys nothing. Five hypotheses about the playback stall
 * were wrong before instrumentation settled it, so this measures instead of guessing.
 *
 * WHAT IT DUMPS, so the two machines can simply be diffed:
 *   1. Gestalt('snd ') with every documented bit named - especially
 *      gestaltBuiltInSoundInput (4) and gestaltHasSoundInputDevice (5).
 *   2. The full SPBGetIndexedDevice enumeration, and for each device the selectors a
 *      game would read: siDeviceName, siInputSource, and siInputSourceNames - which is
 *      exactly the list the control panel's Input tab shows.
 *   3. Every 'sinp' Component Manager component, with its name and info.
 *   4. QuickTime's version, since QuickTime is the other plausible route to CD audio.
 *   5. The audio CD volume's root files with type, creator and both fork lengths -
 *      the file-based route a game could use instead of Red Book.
 *
 * ★ COMPLETELY PASSIVE. It issues no Control or Status call to the CD driver, changes
 * no block size, starts no playback and opens no file. It is safe to run on the MDD
 * with none of this project's software installed, which is the whole point: the machine
 * where the music WORKS is the reference.
 *
 * Output: "CD Sound Recon Log" in the System Folder. Run it on BOTH machines and diff.
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
#include <Sound.h>
#include <SoundInput.h>
#include <Components.h>

#include <stdio.h>
#include <string.h>

#include "cd_probe_common.h"

#ifndef CD_ARTIFACT_NAME
#error "CD_ARTIFACT_NAME is not defined. The CMakeLists must pass the target name."
#endif
#define kVersionString  CD_ARTIFACT_NAME

/* ---- Gestalt('snd ') ------------------------------------------------------- */

typedef struct { int bit; const char *name; } BitName;

/* From Gestalt.h. Bits 4 and 5 are the ones that would make a game give up. */
static const BitName kSoundBits[] = {
    { 0,  "gestaltStereoCapability" },
    { 1,  "gestaltStereoMixing" },
    { 3,  "gestaltSoundIOMgrPresent" },
    { 4,  "gestaltBuiltInSoundInput      <-- KEY" },
    { 5,  "gestaltHasSoundInputDevice    <-- KEY" },
    { 6,  "gestaltPlayAndRecord" },
    { 7,  "gestalt16BitSoundIO" },
    { 8,  "gestaltStereoInput" },
    { 9,  "gestaltLineLevelInput" },
    { 10, "gestaltSndPlayDoubleBuffer" },
    { 11, "gestaltMultiChannels" },
    { 12, "gestalt16BitAudioSupport" },
    { -1, NULL }
};

static void DumpSoundGestalt(void)
{
    long  v = 0;
    OSErr err;
    int   i;

    CDLogf("--- Gestalt('snd ') sound attributes ---");
    err = Gestalt(gestaltSoundAttr, &v);
    if (err != noErr) {
        CDLogf("  Gestalt('snd ') FAILED err=%d. On a machine with no sound input at",
               err);
        CDLogf("  all this may itself be the answer a game gets.");
        return;
    }
    CDLogf("  raw = 0x%08lX", (unsigned long)v);
    for (i = 0; kSoundBits[i].name != NULL; i++)
        CDLogf("    bit %2d %-32s %s", kSoundBits[i].bit, kSoundBits[i].name,
               (v & (1L << kSoundBits[i].bit)) ? "SET" : "-");

    if (!(v & (1L << 5)))
        CDLogf("  ⇒ gestaltHasSoundInputDevice is CLEAR. A game testing this bit would "
               "conclude there is no CD audio source and stop here.");
}

/* ---- the input source list, which is what the control panel shows ---------- */

/* siInputSourceNames hands back a Handle holding a count word followed by Pascal
 * strings. Parsed defensively: a malformed list must not be able to walk off the end. */
static void DumpSourceNames(Handle h)
{
    long  size;
    short count, i;
    unsigned char *p, *end;
    char  name[64];

    if (h == NULL) { CDLogf("      (no source-name list)"); return; }
    size = GetHandleSize(h);
    if (size < 2) { CDLogf("      (source-name list is %ld bytes)", size); return; }

    HLock(h);
    p   = (unsigned char *)*h;
    end = p + size;
    count = (short)((p[0] << 8) | p[1]);
    p += 2;
    CDLogf("      source count = %d", count);
    for (i = 0; i < count && p < end; i++) {
        int len = p[0];
        if (p + 1 + len > end) { CDLogf("      (list truncated at %d)", i); break; }
        if (len > (int)sizeof(name) - 1) len = sizeof(name) - 1;
        memcpy(name, p + 1, len);
        name[len] = 0;
        CDLogf("      source %d: '%s'", i, name);
        p += 1 + p[0];
    }
    HUnlock(h);
}

static void DumpOneDevice(short index, Str255 devName)
{
    char  cname[64];
    long  refnum = 0;
    OSErr err;

    CDPToC(devName, cname, sizeof(cname));
    CDLogf("  device %d: '%s'", index, cname);

    err = SPBOpenDevice(devName, siWritePermission, &refnum);
    if (err != noErr) {
        CDLogf("    SPBOpenDevice err=%d - cannot query this device further", err);
        return;
    }

    {
        Str255 nm;
        nm[0] = 0;
        if (SPBGetDeviceInfo(refnum, siDeviceName, (Ptr)nm) == noErr) {
            CDPToC(nm, cname, sizeof(cname));
            CDLogf("    siDeviceName      = '%s'", cname);
        }
    }
    {
        short src = -1;
        if (SPBGetDeviceInfo(refnum, siInputSource, (Ptr)&src) == noErr)
            CDLogf("    siInputSource     = %d (1-based index into the list below)", src);
        else
            CDLogf("    siInputSource     unavailable");
    }
    {
        Handle h = NULL;
        if (SPBGetDeviceInfo(refnum, siInputSourceNames, (Ptr)&h) == noErr) {
            CDLogf("    siInputSourceNames:");
            DumpSourceNames(h);
        } else {
            CDLogf("    siInputSourceNames unavailable  <-- the control panel's Input "
                   "list comes from here");
        }
    }

    (void)SPBCloseDevice(refnum);
}

static void DumpInputDevices(void)
{
    short  index;
    int    found = 0;

    CDLogf("--- SPBGetIndexedDevice enumeration ---");
    for (index = 1; index < 32; index++) {
        Str255 devName;
        Handle icon = NULL;
        OSErr  err;

        devName[0] = 0;
        err = SPBGetIndexedDevice(index, devName, &icon);
        if (err != noErr) {
            if (index == 1)
                CDLogf("  SPBGetIndexedDevice(1) err=%d - NO sound input devices at all",
                       err);
            break;
        }
        found++;
        DumpOneDevice(index, devName);
        if (icon != NULL) DisposeHandle(icon);
    }
    CDLogf("  ⇒ %d sound input device(s)", found);
    if (found == 0)
        CDLogf("  ⇒ nothing to select a CD source FROM. A game enumerating devices "
               "would find none and disable its CD music without touching the drive.");
}

/* ---- 'sinp' components ----------------------------------------------------- */

static void DumpSinpComponents(void)
{
    ComponentDescription want;
    Component            c = NULL;
    long                 n;
    int                  i = 0;

    CDLogf("--- Component Manager 'sinp' (sound input) components ---");
    memset(&want, 0, sizeof(want));
    want.componentType = FOUR_CHAR_CODE('sinp');

    n = CountComponents(&want);
    CDLogf("  CountComponents('sinp') = %ld", n);

    while ((c = FindNextComponent(c, &want)) != NULL) {
        ComponentDescription got;
        Handle nameH = NewHandle(0);
        Handle infoH = NewHandle(0);
        char   cname[64];

        memset(&got, 0, sizeof(got));
        if (GetComponentInfo(c, &got, nameH, infoH, NULL) == noErr) {
            cname[0] = 0;
            if (nameH != NULL && GetHandleSize(nameH) > 0) {
                HLock(nameH);
                CDPToC((ConstStr255Param)*nameH, cname, sizeof(cname));
                HUnlock(nameH);
            }
            CDLogf("  component %d: subType='%.4s' manuf='%.4s' name='%s'",
                   i, (char *)&got.componentSubType,
                   (char *)&got.componentManufacturer, cname);
        }
        if (nameH != NULL) DisposeHandle(nameH);
        if (infoH != NULL) DisposeHandle(infoH);
        i++;
        if (i > 32) break;
    }
    if (i == 0)
        CDLogf("  ⇒ no 'sinp' components. There is no sound input driver to ask.");
}

/* ---- QuickTime, the other route to CD audio -------------------------------- */

static void DumpQuickTime(void)
{
    long v = 0;

    CDLogf("--- QuickTime ---");
    if (Gestalt(gestaltQuickTime, &v) == noErr)
        CDLogf("  QuickTime version = 0x%08lX", (unsigned long)v);
    else
        CDLogf("  QuickTime NOT present");
}

/* ---- the audio volume's track files, the file-based route ------------------ */

static void DumpAudioVolumeFiles(void)
{
    HParamBlockRec hpb;
    short          vIndex;

    CDLogf("--- mounted volumes, and any track files on them ---");
    for (vIndex = 1; vIndex < 32; vIndex++) {
        Str255 volName;
        char   cname[64];
        short  vRef;

        memset(&hpb, 0, sizeof(hpb));
        volName[0] = 0;
        hpb.volumeParam.ioNamePtr  = volName;
        hpb.volumeParam.ioVolIndex = vIndex;
        if (PBHGetVInfoSync(&hpb) != noErr) break;

        vRef = hpb.volumeParam.ioVRefNum;
        CDPToC(volName, cname, sizeof(cname));
        CDLogf("  volume '%s' vRef=%d driver=%d files=%d",
               cname, vRef, hpb.volumeParam.ioVDRefNum, hpb.volumeParam.ioVNmFls);

        /* Root files, with both fork lengths. An audio-CD pseudo-volume's tracks are
         * reference files: dataEOF is typically 0 and the payload is reached through
         * QuickTime, which is exactly the distinction that matters here. */
        {
            CInfoPBRec cpb;
            short      fIndex;
            int        shown = 0;

            for (fIndex = 1; fIndex < 40; fIndex++) {
                Str255 fName;
                fName[0] = 0;
                memset(&cpb, 0, sizeof(cpb));
                cpb.hFileInfo.ioNamePtr   = fName;
                cpb.hFileInfo.ioVRefNum   = vRef;
                cpb.hFileInfo.ioDirID     = fsRtDirID;
                cpb.hFileInfo.ioFDirIndex = fIndex;
                if (PBGetCatInfoSync(&cpb) != noErr) break;
                if (cpb.hFileInfo.ioFlAttrib & ioDirMask) continue;

                CDPToC(fName, cname, sizeof(cname));
                CDLogf("      '%s' type='%.4s' creator='%.4s' data=%ld rsrc=%ld",
                       cname,
                       (char *)&cpb.hFileInfo.ioFlFndrInfo.fdType,
                       (char *)&cpb.hFileInfo.ioFlFndrInfo.fdCreator,
                       cpb.hFileInfo.ioFlLgLen, cpb.hFileInfo.ioFlRLgLen);
                if (++shown >= 12) { CDLogf("      (more not listed)"); break; }
            }
        }
    }
}

/* ---- main ------------------------------------------------------------------ */

int main(void)
{
    Boolean logOK;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    CDProgressOpen("\p" kVersionString " - progress");
    CDProgressSay("%s starting", kVersionString);

    logOK = CDLogOpen("\pCD Sound Recon Log");
    if (!logOK) CDProgressSay("!! could not open 'CD Sound Recon Log' - screen only");
    CDLogBanner(kVersionString " - why the game never asks for CD audio",
                "PASSIVE: no CD driver calls, no playback, nothing opened");

    CDProgressSay("reading Gestalt sound attributes");
    DumpSoundGestalt();

    CDProgressSay("enumerating sound input devices");
    DumpInputDevices();

    CDProgressSay("enumerating 'sinp' components");
    DumpSinpComponents();

    DumpQuickTime();

    CDProgressSay("listing volumes and track files");
    DumpAudioVolumeFiles();

    CDLogf("=== end of run. Run this on BOTH machines and diff the two logs. The");
    CDLogf("    machine where the game's music WORKS is the reference. ===");
    CDLogFlush();
    CDLogClose();

    CDProgressSay("done - see 'CD Sound Recon Log' in the System Folder");
    {
        EventRecord evt;
        long deadline = TickCount() + 600;      /* 10 s to read the window */
        while (TickCount() < deadline) (void)WaitNextEvent(0, &evt, 1, NULL);
    }
    CDProgressClose();
    return 0;
}

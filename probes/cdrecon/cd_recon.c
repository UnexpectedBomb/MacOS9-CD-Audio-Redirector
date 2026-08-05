/*
 * CDRecon — Phase-0 recon for the OS 9 CD Audio Redirector.
 *
 * WHY THIS EXISTS
 * ---------------
 * Mixed-mode Mac CD games (Warcraft: Orcs & Humans, pre-1.09 Quake, …) play
 * their music by asking the CD-ROM drive to play a Red Book audio track over the
 * old ANALOG CD-audio wire. Late Macs — the G4 mini especially — have no such
 * wire, so the music is silent while digital sound effects are fine. The planned
 * fix is an extension that intercepts the legacy audio Control calls and services
 * them digitally. See ../../FEASIBILITY.md, ../../REVIEW.md and ../../PHASE0.md.
 *
 * Before any of that gets written, three things have to be established on real
 * hardware. This app answers them in one read-mostly run:
 *
 *   P2  WHICH driver owns the optical drive, and is it a classic 'DRVR' or a
 *       native 'ndrv'? That single fact decides the entire interception
 *       mechanism — "save the original Control entry" means two completely
 *       different things in the two cases, and the eSATA work already found that
 *       a mis-shaped native dispatch field fails as a garbage-UPP branch rather
 *       than a clean error.
 *
 *   P3  Does a mixed-mode disc expose its audio tracks as FILES? Under classic
 *       Mac OS with QuickTime an audio CD mounts as a volume of AIFF-readable
 *       track files. If that happens for the audio session of a mixed-mode disc
 *       too, "DAE" is a plain FSRead and a large chunk of the planned design is
 *       unnecessary. Cheap to check; big if true.
 *
 *   P4  THE GATE: can we read CD-DA sectors at all? This tries route A — ask the
 *       driver for a 2352-byte block size, then do a driver-level PBRead at an
 *       audio track's LBA. Plausible PCM ⇒ the project is GO and the ATA Manager
 *       never has to be touched. Failure ⇒ route B (ATAPI READ CD 0xBE via the
 *       ATA Manager, using the 'dvrf' handle this run logs), and if nothing works
 *       the extension approach is blocked.
 *
 * It also sweeps the driver's read-only status surface, because those answers
 * feed Phase 1 and because an ERROR is as informative as a success here: an
 * AudioStatus that returns an error is the H2 signal (the ATAPI-era driver does
 * not implement the audio calls at all), while one that succeeds points at H1
 * (accepted, but with no route to a speaker).
 *
 * WHAT IT TOUCHES
 * ---------------
 * Everything is read-only EXCEPT the P4 DAE attempt, which changes the drive's
 * block size and changes it straight back. Hold OPTION at launch to skip P4 and
 * keep the run 100% read-only. Every step is logged and flushed BEFORE the driver
 * call it describes, so if a call hangs the machine the last line in the log
 * names the call that did it.
 *
 * Output: "CD Recon Log" in the System Folder, appended — read from the LAST
 * banner, not the head — plus a summary window.
 *
 * Native PowerPC application, Retro68. No stdio: printf-based apps quit
 * immediately on real OS 9, so this uses an explicitly-initialised Toolbox, a
 * window, and a flushed log file.
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
#include <Disks.h>
#include <Files.h>
#include <Memory.h>
#include <OSUtils.h>
#include <DriverGestalt.h>
#include <ToolUtils.h>

#include <stdio.h>
#include <string.h>

#include "cd_probe_common.h"
#include "cd_cscodes.h"

#define kVersionString  "CDRecon v2"

typedef enum {
    kDAENotRun = 0,
    kDAEGo,             /* sectors read, contents look like PCM               */
    kDAEReadOK,         /* sectors read, but contents look like silence       */
    kDAEBlockSizeRefused,
    kDAEReadFailed,
    kDAESkipped
} DAEVerdict;

typedef struct {
    CDDriverInfo cd;
    CDTOC        toc;
    short        blockSize;
    OSErr        audioStatusErr;    /* the H1 vs H2 signal                     */
    Boolean      audioStatusIsCtl;  /* answered as Control rather than Status   */
    Boolean      audioFilesFound;   /* P3                                      */
    Str255       audioFileVolume;
    DAEVerdict   dae;
    OSErr        daeErr;
    long         daeLBA;
    long         daeNonZeroBytes;
    long         daeTotalBytes;
    long         daePeakLE, daePeakBE;
} Recon;

static Recon gR;

/* ---- read-only status surface -------------------------------------------- */

static void ProbeStatusSurface(void)
{
    short refNum = gR.cd.refNum;
    short buf[11];
    OSErr err;

    CDLogf("--- read-only status surface ---");

    if (CDStatusCall(refNum, kcsGetBlockSize, buf, sizeof(buf)) == noErr) {
        gR.blockSize = buf[0];
        CDLogf("  GetBlockSize = %d", buf[0]);
    }
    if (CDStatusCall(refNum, kcsGetDriveType, buf, sizeof(buf)) == noErr)
        CDLogf("  GetDriveType = %d (3 = CD300 or later)", buf[0]);
    if (CDStatusCall(refNum, kcsGet2KOffset, buf, sizeof(buf)) == noErr)
        CDLogf("  Get2KOffset = %d", buf[0]);
    if (CDStatusCall(refNum, kcsGetCDFeatures, buf, sizeof(buf)) == noErr)
        CDLogf("  GetCDFeatures: speed=%d features=0x%04X",
               buf[0], (unsigned short)buf[1]);
    if (CDStatusCall(refNum, kcsReturnDeviceIdent, buf, sizeof(buf)) == noErr)
        CDLogf("  ReturnDeviceIdent = 0x%04X", (unsigned short)buf[0]);
    (void)CDStatusCall(refNum, kcsDriveStatus, buf, sizeof(buf));
    (void)CDStatusCall(refNum, kcsWhoIsThere, buf, sizeof(buf));

    /* --- the audio surface: the H1 vs H2 discriminator --- *
     * AudioStatus answers ⇒ the driver implements the legacy audio calls, so a
     * game's AudioPlay is being accepted and simply has nowhere to go (H1).
     * Refused both as Status and as Control ⇒ the driver does not implement them
     * and games are getting an error back (H2), which means the extension has to
     * synthesise convincing replies, not merely add sound. */
    CDLogf("--- audio surface (H1 vs H2) ---");
    err = CDStatusCall(refNum, kcsAudioStatus, buf, sizeof(buf));
    gR.audioStatusErr   = err;
    gR.audioStatusIsCtl = false;
    if (err != noErr) {
        CDLogf("  AudioStatus as Status failed (err=%d); retrying as Control",
               err);
        err = CDControlCall(refNum, kcsAudioStatus, NULL, 0, buf, sizeof(buf));
        if (err == noErr) {
            gR.audioStatusErr   = noErr;
            gR.audioStatusIsCtl = true;
        } else {
            gR.audioStatusErr = err;
        }
    }
    CDLogf("  ⇒ AudioStatus err=%d answered-as=%s ⇒ %s",
           gR.audioStatusErr,
           gR.audioStatusIsCtl ? "Control" : "Status",
           (gR.audioStatusErr == noErr)
               ? "H1: driver accepts the audio calls"
               : "H2: driver rejects the audio calls");

    err = CDStatusCall(refNum, kcsReadTheQSubcode, buf, sizeof(buf));
    if (err != noErr)
        (void)CDControlCall(refNum, kcsReadTheQSubcode, NULL, 0,
                            buf, sizeof(buf));

    if (CDStatusCall(refNum, kcsReadAudioVolume, buf, sizeof(buf)) == noErr)
        CDLogf("  ReadAudioVolume: left=%d right=%d",
               ((unsigned char *)buf)[0], ((unsigned char *)buf)[1]);

    (void)CDStatusCall(refNum, kcsGetPlayMode, buf, sizeof(buf));
}

/* ---- P3: do the audio tracks show up as FILES? --------------------------- */

static void SurveyVolumes(void)
{
    HParamBlockRec hpb;
    short          index;

    CDLogf("--- P3: mounted volumes ---");

    for (index = 1; index < 64; index++) {
        Str255 vName;
        char   cname[64];
        OSErr  err;

        memset(&hpb, 0, sizeof(hpb));
        vName[0] = 0;
        hpb.volumeParam.ioNamePtr  = vName;
        hpb.volumeParam.ioVolIndex = index;
        err = PBHGetVInfoSync(&hpb);
        if (err != noErr) break;

        CDPToC(vName, cname, sizeof(cname));
        CDLogf("  vol %d: '%s' vRefNum=%d drvNum=%d drvRefNum=%d sig=0x%04X "
               "fsID=%d files=%ld",
               index, cname, hpb.volumeParam.ioVRefNum,
               hpb.volumeParam.ioVDrvInfo, hpb.volumeParam.ioVDRefNum,
               (unsigned short)hpb.volumeParam.ioVSigWord,
               hpb.volumeParam.ioVFSID, hpb.volumeParam.ioVNmFls);

        if (hpb.volumeParam.ioVDRefNum != gR.cd.refNum) continue;

        CDLogf("    ^ on the CD driver — listing the root directory:");
        {
            CInfoPBRec cpb;
            short      fIndex;

            for (fIndex = 1; fIndex < 200; fIndex++) {
                Str255 fName;
                char   fcname[64];
                OSErr  ferr;

                memset(&cpb, 0, sizeof(cpb));
                fName[0] = 0;
                cpb.hFileInfo.ioNamePtr   = fName;
                cpb.hFileInfo.ioVRefNum   = hpb.volumeParam.ioVRefNum;
                cpb.hFileInfo.ioDirID     = fsRtDirID;
                cpb.hFileInfo.ioFDirIndex = fIndex;
                ferr = PBGetCatInfoSync(&cpb);
                if (ferr != noErr) break;

                CDPToC(fName, fcname, sizeof(fcname));
                if (cpb.hFileInfo.ioFlAttrib & ioDirMask) {
                    CDLogf("      dir  '%s'", fcname);
                } else {
                    OSType type    = cpb.hFileInfo.ioFlFndrInfo.fdType;
                    OSType creator = cpb.hFileInfo.ioFlFndrInfo.fdCreator;
                    CDLogf("      file '%s' type='%.4s' creator='%.4s' "
                           "dataEOF=%ld",
                           fcname, (char *)&type, (char *)&creator,
                           cpb.hFileInfo.ioFlLgLen);
                    /* An audio track surfaced as a file: AIFF/AIFC, or one of
                     * the QuickTime CD-audio track types. */
                    if (type == 'AIFF' || type == 'AIFC' ||
                        type == 'cdda' || type == 'trak') {
                        gR.audioFilesFound = true;
                        BlockMoveData(vName, gR.audioFileVolume, vName[0] + 1);
                        CDLogf("      ^^^ AUDIO TRACK AS A FILE — P3 is GO: DAE "
                               "may be a plain FSRead");
                    }
                }
            }
        }
    }

    if (!gR.audioFilesFound)
        CDLogf("  no audio-track files on any CD-driver volume (expected if the "
               "audio session does not mount)");
}

/* ---- P4: THE GATE — can we read CD-DA sectors? --------------------------- */

/* Route A: ask the driver to switch to a 2352-byte block size, then do a
 * driver-level PBRead at an audio track's LBA, ~10 s in so a silent pregap does
 * not masquerade as a failed read. Restores the block size on every exit path. */
static void ProbeDAE(void)
{
    short          param[11];
    OSErr          err;
    unsigned char *buf;
    const long     kSectors  = 4;
    const long     kReadSize = kSectors * kCDDASectorBytes;
    short          savedBlockSize = gR.blockSize ? gR.blockSize : 2048;
    int            t;

    CDLogf("--- P4: DAE gate (route A: 2352-byte blocks + driver read) ---");

    /* Pick the first audio track, 10 seconds in. */
    for (t = 0; t < gR.toc.trackCount; t++) {
        if (!gR.toc.track[t].isData) {
            gR.daeLBA = gR.toc.track[t].lba + 10 * kCDDASectorsPerSec;
            CDLogf("  target: track %d, lba %ld (track start %ld + 10 s)",
                   gR.toc.track[t].number, gR.daeLBA, gR.toc.track[t].lba);
            break;
        }
    }

    if (gR.toc.audioCount == 0 || gR.daeLBA <= 0) {
        gR.dae = kDAESkipped;
        CDLogf("  skipped: no audio track located. Put a mixed-mode game disc or "
               "an audio CD in the drive and re-run.");
        return;
    }
    if (gR.cd.driveNum == 0) {
        gR.dae = kDAESkipped;
        CDLogf("  skipped: no drive number for refNum=%d, so no driver-level "
               "read is possible", gR.cd.refNum);
        return;
    }

    buf = (unsigned char *)NewPtrClear(kReadSize);
    if (buf == NULL) {
        gR.dae = kDAESkipped;
        CDLogf("  skipped: out of memory");
        return;
    }

    /* 1. block size → 2352 */
    memset(param, 0, sizeof(param));
    param[0] = kCDDASectorBytes;
    CDLogStep("ChangeBlockSize -> 2352");
    err = CDControlCall(gR.cd.refNum, kcsChangeBlockSize, param, sizeof(param),
                        NULL, 0);
    CDLogf("  ChangeBlockSize(2352) err=%d", err);
    if (err != noErr) {
        gR.dae    = kDAEBlockSizeRefused;
        gR.daeErr = err;
        CDLogf("  ⇒ route A BLOCKED: the driver will not do 2352-byte blocks. "
               "Next: route B, ATAPI READ CD (0xBE) via the ATA Manager, using "
               "the 'dvrf' handle logged above.");
        DisposePtr((Ptr)buf);
        return;
    }

    /* 2. driver-level read at the audio LBA */
    {
        ParamBlockRec pb;

        memset(&pb, 0, sizeof(pb));
        pb.ioParam.ioRefNum    = gR.cd.refNum;
        pb.ioParam.ioVRefNum   = gR.cd.driveNum;
        pb.ioParam.ioBuffer    = (Ptr)buf;
        pb.ioParam.ioReqCount  = kReadSize;
        pb.ioParam.ioPosMode   = fsFromStart;
        pb.ioParam.ioPosOffset = gR.daeLBA * kCDDASectorBytes;

        CDLogStep("PBRead lba=%ld off=%ld len=%ld drive=%d",
                  gR.daeLBA, pb.ioParam.ioPosOffset, kReadSize,
                  gR.cd.driveNum);
        err = PBReadSync(&pb);
        gR.daeErr = err;
        CDLogf("  PBRead err=%d actCount=%ld", err, pb.ioParam.ioActCount);

        if (err == noErr && pb.ioParam.ioActCount > 0) {
            /* Is this plausibly PCM? Real music is high-entropy under either
             * byte order; a silent pregap is all zeros. Peak amplitude both ways
             * plus a non-zero-byte count distinguishes music from a block of
             * zeros or a repeating pattern. */
            long i, n = pb.ioParam.ioActCount;
            long nonZero = 0, peakLE = 0, peakBE = 0;

            if (n > kReadSize) n = kReadSize;
            for (i = 0; i + 1 < n; i += 2) {
                short le  = (short)((buf[i + 1] << 8) | buf[i]);
                short be  = (short)((buf[i] << 8) | buf[i + 1]);
                long  ale = le < 0 ? -(long)le : le;
                long  abe = be < 0 ? -(long)be : be;
                if (ale > peakLE) peakLE = ale;
                if (abe > peakBE) peakBE = abe;
                if (buf[i] != 0)     nonZero++;
                if (buf[i + 1] != 0) nonZero++;
            }
            gR.daeNonZeroBytes = nonZero;
            gR.daeTotalBytes   = n;
            gR.daePeakLE       = peakLE;
            gR.daePeakBE       = peakBE;
            CDLogf("  content: nonZeroBytes=%ld/%ld peakLE=%ld peakBE=%ld",
                   nonZero, n, peakLE, peakBE);
            CDLogHex("cdda", buf, 64);

            if (nonZero * 4 > n) {          /* >25% non-zero ⇒ not silence */
                gR.dae = kDAEGo;
                CDLogf("  ⇒ P4 GO: CD-DA sectors are readable straight through "
                       "the driver. The extension approach is viable and route B "
                       "is unnecessary.");
            } else {
                gR.dae = kDAEReadOK;
                CDLogf("  ⇒ read SUCCEEDED but the data is at or near silence. "
                       "Either the LBA landed in a quiet passage or the read "
                       "returned zeros. Re-run against a different track before "
                       "concluding anything.");
            }
        } else {
            gR.dae = kDAEReadFailed;
            CDLogf("  ⇒ route A read FAILED. Next: route B, ATAPI READ CD "
                   "(0xBE) via the ATA Manager, using the 'dvrf' handle above.");
        }
    }

    /* 3. restore the block size, on every path */
    memset(param, 0, sizeof(param));
    param[0] = savedBlockSize;
    CDLogStep("ChangeBlockSize -> %d (restore)", savedBlockSize);
    err = CDControlCall(gR.cd.refNum, kcsChangeBlockSize, param, sizeof(param),
                        NULL, 0);
    CDLogf("  ChangeBlockSize(%d) restore err=%d", savedBlockSize, err);
    if (err != noErr)
        CDLogf("  !! block size NOT restored. Eject and re-insert the disc "
               "before trusting any further read from this drive.");

    DisposePtr((Ptr)buf);
}

/* ---- summary window ------------------------------------------------------ */

static void DrawLabelled(short v, const char *label, const char *value)
{
    Str255 s;
    char   tmp[256];
    int    n;

    n = snprintf(tmp, sizeof(tmp), "%s%s", label, value ? value : "");
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    s[0] = (unsigned char)n;
    BlockMoveData(tmp, s + 1, n);
    MoveTo(12, v);
    DrawString(s);
}

static const char *DAEName(void)
{
    switch (gR.dae) {
        case kDAEGo:               return "GO - CD-DA readable via the driver";
        case kDAEReadOK:           return "read OK but silent - re-run";
        case kDAEBlockSizeRefused: return "blocked - 2352 refused (try route B)";
        case kDAEReadFailed:       return "blocked - read failed (try route B)";
        case kDAESkipped:          return "skipped - no audio track / no drive";
        default:                   return "not run";
    }
}

static void ShowSummary(void)
{
    WindowPtr   win;
    Rect        bounds;
    EventRecord evt;
    char        tmp[256];
    short       v = 22;

    SetRect(&bounds, 40, 60, 40 + 580, 60 + 320);
    win = NewWindow(NULL, &bounds, "\p" kVersionString, true, documentProc,
                    (WindowPtr)-1L, false, 0);
    if (win == NULL) return;
    SetPort(win);
    TextFont(kFontIDGeneva);
    TextSize(9);

    DrawLabelled(v, "CD Audio Redirector - Phase 0 recon", ""); v += 18;

    if (!gR.cd.found) {
        DrawLabelled(v, "No driver reported devt=='cdrm'.", ""); v += 14;
        DrawLabelled(v, "See 'CD Recon Log' in the System Folder.", ""); v += 14;
    } else {
        char nameC[64];
        CDPToC(gR.cd.name, nameC, sizeof(nameC));

        snprintf(tmp, sizeof(tmp), "%d  (drive %d)",
                 gR.cd.refNum, gR.cd.driveNum);
        DrawLabelled(v, "P2  CD driver refNum: ", tmp); v += 14;

        snprintf(tmp, sizeof(tmp), "%s   name '%s'",
                 gR.cd.isNative ? "NATIVE 'ndrv'" : "classic 'DRVR'", nameC);
        DrawLabelled(v, "P2  driver kind:      ", tmp); v += 14;

        snprintf(tmp, sizeof(tmp), "devt='%.4s'  intf='%.4s'  blockSize=%d",
                 (char *)&gR.cd.deviceType, (char *)&gR.cd.interfaceType,
                 gR.blockSize);
        DrawLabelled(v, "P2  identity:         ", tmp); v += 18;

        if (gR.toc.valid)
            snprintf(tmp, sizeof(tmp), "tracks %d..%d, %d audio",
                     gR.toc.firstTrack, gR.toc.lastTrack, gR.toc.audioCount);
        else
            snprintf(tmp, sizeof(tmp), "ReadTOC failed - is a disc mounted?");
        DrawLabelled(v, "    TOC:              ", tmp); v += 14;

        snprintf(tmp, sizeof(tmp), "err=%d as %s -> %s",
                 gR.audioStatusErr,
                 gR.audioStatusIsCtl ? "Control" : "Status",
                 (gR.audioStatusErr == noErr) ? "H1 (accepted)"
                                              : "H2 (rejected)");
        DrawLabelled(v, "    AudioStatus:      ", tmp); v += 18;

        if (gR.audioFilesFound) {
            char volC[64];
            CDPToC(gR.audioFileVolume, volC, sizeof(volC));
            snprintf(tmp, sizeof(tmp), "YES on '%s' - DAE may be plain FSRead",
                     volC);
        } else {
            snprintf(tmp, sizeof(tmp), "no audio-track files mounted");
        }
        DrawLabelled(v, "P3  tracks as files:  ", tmp); v += 18;

        DrawLabelled(v, "P4  DAE gate:         ", DAEName()); v += 14;
        if (gR.dae == kDAEGo || gR.dae == kDAEReadOK) {
            snprintf(tmp, sizeof(tmp),
                     "lba=%ld nonZero=%ld/%ld peakLE=%ld peakBE=%ld",
                     gR.daeLBA, gR.daeNonZeroBytes, gR.daeTotalBytes,
                     gR.daePeakLE, gR.daePeakBE);
            DrawLabelled(v, "    DAE detail:       ", tmp); v += 14;
        }
        v += 8;
        DrawLabelled(v, "Full detail: 'CD Recon Log' in the System Folder.", "");
        v += 14;
        DrawLabelled(v, "Read from the LAST banner - the log appends.", "");
        v += 18;
    }

    DrawLabelled(v, "Click or press a key to quit.", "");

    for (;;) {
        if (WaitNextEvent(mDownMask | keyDownMask, &evt, 10, NULL)) {
            if (evt.what == mouseDown || evt.what == keyDown) break;
        }
    }
    DisposeWindow(win);
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    KeyMap  km;
    Boolean skipDAE, safeMode, logOK;

    /* Explicit Toolbox init. This app deliberately does not use RetroConsole:
     * its Toolbox init is lazy (it fires on the first printf), and building
     * windows before that happens scribbles the heap. */
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    GetKeys(km);
    skipDAE  = KeyIsDown(km, kOptionKeyCode);
    safeMode = KeyIsDown(km, kShiftKeyCode);

    memset(&gR, 0, sizeof(gR));

    /* Progress window FIRST, before anything that talks to a driver. v1 opened
     * no window until all the probing was done, so its first hardware run looked
     * identical to a dead app: blank menu bar, live cursor, nothing else. Now
     * every driver call announces itself here before it is issued, so a hang is
     * visible, attributable, and distinguishable from merely slow. */
    CDProgressOpen("\p" kVersionString " - progress");
    CDProgressSay("%s starting", kVersionString);
    if (skipDAE)  CDProgressSay("option held: P4 (DAE probe) will be SKIPPED");
    if (safeMode) CDProgressSay("shift held: SAFE MODE, no unit-table sweep");

    logOK = CDLogOpen("\pCD Recon Log");
    if (!logOK)
        CDProgressSay("!! could not open 'CD Recon Log' - screen only");
    CDLogBanner(kVersionString " - CD Audio Redirector Phase 0 recon",
                skipDAE ? "option held: P4 (DAE) will be SKIPPED"
                        : "option not held: P4 (DAE) will run");

    CDLogf("--- P2: locate and classify the optical driver ---");
    CDFindDriver(&gR.cd, !safeMode);

    if (gR.cd.found) {
        ProbeStatusSurface();
        CDReadTOC(gR.cd.refNum, &gR.toc);
        CDProgressSay("surveying mounted volumes (P3)");
        SurveyVolumes();
        if (skipDAE) {
            gR.dae = kDAESkipped;
            CDLogf("--- P4: SKIPPED (option key held) ---");
        } else {
            CDProgressSay("P4: the DAE gate");
            ProbeDAE();
        }
    }

    CDLogf("=== end of run: foundCD=%d native=%d tocValid=%d audioTracks=%d "
           "audioFiles=%d audioStatusErr=%d daeVerdict=%d",
           gR.cd.found, gR.cd.isNative, gR.toc.valid, gR.toc.audioCount,
           gR.audioFilesFound, gR.audioStatusErr, (int)gR.dae);
    CDLogClose();

    CDProgressSay("done - showing the summary");
    CDProgressClose();
    ShowSummary();
    return 0;
}

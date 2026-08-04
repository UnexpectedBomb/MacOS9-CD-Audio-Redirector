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
 * them digitally (DAE → Sound Manager). See ../../FEASIBILITY.md and ../../REVIEW.md.
 *
 * Before any of that gets written, three things have to be established on real
 * hardware. This app answers them in one read-mostly run:
 *
 *   P2  WHICH driver owns the optical drive, and is it a classic 'DRVR' or a
 *       native 'ndrv'? That single fact decides the entire interception
 *       mechanism — "save the original Control entry" means two completely
 *       different things in the two cases, and the eSATA work already found that
 *       a mis-shaped native dispatch field fails as a garbage-UPP branch rather
 *       than a clean error. So: dump the DCE, and classify it.
 *
 *   P3  Does a mixed-mode disc expose its audio tracks as FILES? Under classic
 *       Mac OS with QuickTime, an audio CD mounts as a volume of AIFF-readable
 *       track files. If that happens for the audio session of a mixed-mode disc
 *       too, then "DAE" is a plain FSRead and the whole READ CD (0xBE) branch of
 *       the design evaporates. Cheap to check; large if true.
 *
 *   P4  THE GATE: can we read CD-DA sectors at all? This tries route A — ask the
 *       driver for a 2352-byte block size, then do a driver-level PBRead at an
 *       audio track's LBA. If that yields plausible PCM, the project is GO and
 *       we never need to touch the ATA Manager. If it fails, route B (ATAPI
 *       READ CD 0xBE through the ATA Manager) is next, and if nothing works the
 *       extension approach is blocked.
 *
 * It also sweeps the driver's read-only status surface (TOC, Q sub-channel,
 * AudioStatus, drive/feature words) because those answers feed Phase 1, and an
 * ERROR is just as informative as a success here: an AudioStatus that returns
 * an error is the H2 signal (the ATAPI-era driver doesn't implement the audio
 * calls at all), while one that succeeds points at H1 (accepted, but no route to
 * a speaker).
 *
 * WHAT IT WRITES TO / TOUCHES
 * ---------------------------
 * Everything is read-only EXCEPT the P4 DAE attempt, which changes the drive's
 * block size and changes it straight back. Hold OPTION at launch to skip P4 and
 * keep the run 100% read-only. Every step is logged and the log is FLUSHED
 * BEFORE each driver call, so if a call hangs the machine the last line in the
 * log names the call that did it. (Project discipline: no debugger, so make the
 * breadcrumb trail do the work.)
 *
 * Output: "CD Recon Log" in the System Folder, appended (read from the LAST
 * banner, not the head — this file accumulates across runs), plus a summary
 * window with the verdicts.
 *
 * Native PowerPC application, built with Retro68. No stdio: printf-based apps
 * quit immediately on real OS 9, so this uses an explicitly-initialised Toolbox,
 * a window, and a flushed log file.
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
#include <Folders.h>
#include <Memory.h>
#include <OSUtils.h>
#include <Resources.h>
#include <DriverGestalt.h>
#include <DriverServices.h>
#include <DriverFamilyMatching.h>   /* DriverDescription / MacDriverType */
#include <NameRegistry.h>
#include <Sound.h>
#include <ToolUtils.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cd_cscodes.h"

#define kVersionString  "CDRecon v1"

/* ---- low-memory globals -------------------------------------------------- *
 * Declared by hand as absolute addresses rather than via LowMem.h, because the
 * accessors there come in 68K-inline and PPC-macro flavours depending on which
 * conditional block is active, and a link-time surprise on the OS 9 machine is
 * expensive. These three addresses are stable across every 68K/PPC Mac OS. */
#define LM_UTableBase        (*(Ptr *)0x011C)     /* unit table base           */
#define LM_UnitEntryCount    (*(short *)0x01D2)   /* unit table entry count    */
#define LM_DrvQHdr           ((QHdrPtr)0x0308)    /* drive queue header        */

/* Keyboard: option key, for the "skip the invasive probe" escape hatch. */
#define kOptionKeyCode 0x3A
#define KeyIsDown(km, code) \
    ((((unsigned char *)(km))[(code) >> 3] & (1 << ((code) & 7))) != 0)

/* ---- collected results, for the summary window ---------------------------- */

typedef enum {
    kDAENotRun = 0,
    kDAEGo,             /* sectors read, contents look like PCM               */
    kDAEReadOK,         /* sectors read, but contents look like silence/zeros */
    kDAEBlockSizeRefused,
    kDAEReadFailed
} DAEVerdict;

typedef struct {
    short       cdRefNum;          /* driver refNum of the optical driver      */
    short       cdDriveNum;        /* its drive number, 0 if none              */
    Boolean     foundCD;
    Boolean     isNative;          /* native 'ndrv' vs classic 'DRVR'          */
    Str255      driverName;
    OSType      deviceType;        /* DriverGestalt 'devt'                     */
    OSType      interfaceType;     /* DriverGestalt 'intf'                     */
    short       blockSize;
    Boolean     tocOK;
    short       firstTrack, lastTrack;
    short       audioTrackCount;
    OSErr       audioStatusErr;    /* the H1 vs H2 signal                      */
    Boolean     audioStatusIsCtl;  /* answered as Control rather than Status    */
    Boolean     audioFilesFound;   /* P3: audio tracks visible as files         */
    Str255      audioFileVolume;
    DAEVerdict  dae;
    OSErr       daeErr;
    long        daeLBA;
    long        daeNonZeroBytes;
    long        daePeakLE, daePeakBE;
} Recon;

static Recon gR;

/* ---- logging -------------------------------------------------------------- *
 * One append-mode text file in the System Folder. Bounded vsnprintf, never
 * vsprintf: a long line through an unbounded vsprintf is exactly how the USB2
 * work smashed its stack and got a MacsBug PC full of ASCII. */

static short gLogRef = 0;

static void LogOpen(void)
{
    short  vRefNum;
    long   dirID;
    FSSpec spec;
    long   eof;

    if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return;
    if (FSMakeFSSpec(vRefNum, dirID, "\pCD Recon Log", &spec) != noErr) {
        if (FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript) != noErr) return;
    }
    if (FSpOpenDF(&spec, fsRdWrPerm, &gLogRef) != noErr) { gLogRef = 0; return; }
    if (GetEOF(gLogRef, &eof) == noErr) SetFPos(gLogRef, fsFromStart, eof);
}

static void LogFlush(void)
{
    FSSpec  spec;
    Str255  name;
    short   vRefNum;
    long    dirID;

    if (gLogRef == 0) return;
    /* FlushVol by refNum-less form: flush the volume the log lives on. */
    if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) == noErr) {
        (void)spec; (void)name;
        FlushVol(NULL, vRefNum);
    }
}

static void Logf(const char *fmt, ...)
{
    char    buf[512];
    va_list ap;
    long    len;
    int     n;

    if (gLogRef == 0) return;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);   /* bounded, always */
    va_end(ap);
    if (n < 0) return;
    if (n > (int)(sizeof(buf) - 3)) n = (int)(sizeof(buf) - 3);
    buf[n++] = '\r';                                 /* classic Mac line end */
    buf[n] = 0;

    len = n;
    FSWrite(gLogRef, &len, buf);
}

/* Log a line, then flush, so a hang in the NEXT call leaves this line on disc.
 * Used immediately before every driver call. */
static void LogStep(const char *fmt, ...)
{
    char    buf[512];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    Logf("STEP %s", buf);
    LogFlush();
}

static void LogClose(void)
{
    if (gLogRef == 0) return;
    LogFlush();
    FSClose(gLogRef);
    gLogRef = 0;
}

/* Hex-dump n bytes of p, 16 per line, prefixed by tag. */
static void LogHex(const char *tag, const void *p, long n)
{
    const unsigned char *b = (const unsigned char *)p;
    char  line[128];
    long  i, off = 0;

    while (off < n) {
        long chunk = n - off;
        int  used  = 0;
        if (chunk > 16) chunk = 16;
        for (i = 0; i < chunk; i++)
            used += snprintf(line + used, sizeof(line) - used, "%02X ", b[off + i]);
        Logf("  %s +%04ld: %s", tag, off, line);
        off += chunk;
    }
}

/* Pascal string → C string, for logging. */
static void PToC(ConstStr255Param src, char *dst, int dstSize)
{
    int len = (src == NULL) ? 0 : src[0];
    if (len > dstSize - 1) len = dstSize - 1;
    if (len > 0) BlockMoveData(src + 1, dst, len);
    dst[len] = 0;
}

/* ---- Device Manager helpers ---------------------------------------------- */

/* Issue a Status call with an all-zero csParam and log the result. Returns the
 * OSErr and, if the caller wants them, the returned csParam bytes. */
static OSErr StatusCall(short refNum, short csCode, void *paramOut, long paramOutLen)
{
    CntrlParam pb;
    OSErr      err;

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = refNum;
    pb.csCode    = csCode;

    LogStep("Status csCode=%d refNum=%d", csCode, refNum);
    err = PBStatusSync((ParmBlkPtr)&pb);

    Logf("  Status csCode=%-3d err=%-6d csParam:", csCode, err);
    LogHex("csParam", pb.csParam, 22);
    if (paramOut != NULL) {
        if (paramOutLen > 22) paramOutLen = 22;
        BlockMoveData(pb.csParam, paramOut, paramOutLen);
    }
    return err;
}

/* Same, as a Control call. Only ever used here for calls documented read-only
 * (ReadTOC / ReadQ / AudioStatus / ReadAudioVolume) plus the deliberate
 * block-size change in the P4 probe. Never a blind sweep: an unknown Control
 * code on an optical drive could be destructive on a burner. */
static OSErr ControlCall(short refNum, short csCode, const void *paramIn,
                         long paramInLen, void *paramOut, long paramOutLen)
{
    CntrlParam pb;
    OSErr      err;

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = refNum;
    pb.csCode    = csCode;
    if (paramIn != NULL) {
        if (paramInLen > 22) paramInLen = 22;
        BlockMoveData(paramIn, pb.csParam, paramInLen);
    }

    LogStep("Control csCode=%d refNum=%d", csCode, refNum);
    err = PBControlSync((ParmBlkPtr)&pb);

    Logf("  Control csCode=%-3d err=%-6d csParam:", csCode, err);
    LogHex("csParam", pb.csParam, 22);
    if (paramOut != NULL) {
        if (paramOutLen > 22) paramOutLen = 22;
        BlockMoveData(pb.csParam, paramOut, paramOutLen);
    }
    return err;
}

/* DriverGestalt query. Returns noErr and the response, or the driver's error.
 * Drivers that don't implement a selector return an error — that is data too. */
static OSErr DriverGestaltQuery(short refNum, OSType selector, UInt32 *response)
{
    DriverGestaltParam pb;
    OSErr              err;

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum              = refNum;
    pb.csCode                 = kcsDriverGestalt;
    pb.driverGestaltSelector  = selector;

    err = PBStatusSync((ParmBlkPtr)&pb);
    if (response != NULL) *response = pb.driverGestaltResponse;
    return err;
}

/* ---- P2: find the optical driver and classify it -------------------------- */

/* Dump the DCE and decide classic DRVR vs native ndrv.
 *
 * The classifier is GetDriverInformation (DriverLoaderLib): it only knows about
 * natively-installed drivers, so success means native and gives us the unit
 * number, the Name Registry entry, the fragment main address and the
 * DriverDescription. Failure means we are looking at a classic 'DRVR', whose
 * Control/Status entry points are 16-bit OFFSETS in the DRVR header — which is
 * what a classic interception would patch, so those get logged too. */
static void DumpDCE(short refNum, Boolean *isNativeOut, StringPtr nameOut)
{
    DCtlHandle       dceH;
    DCtlPtr          dce;
    AuxDCEPtr        aux;
    UnitNumber       unitNum   = 0;
    DriverFlags      flags     = 0;
    DriverOpenCount  openCount = 0;
    Str255           name;
    RegEntryID       regID;
    DriverDescription desc;
    OSErr            err;
    char             cname[64];

    *isNativeOut = false;
    nameOut[0] = 0;

    /* GetDCtlEntry returns a HANDLE to the DCE (DCtlHandle == DCtlPtr *), so it
     * needs one dereference. The DCE is a system structure and is not going to
     * move under us here, but nothing below holds the pointer across a call that
     * could relocate memory. */
    dceH = GetDCtlEntry(refNum);
    if (dceH == NULL || *dceH == NULL) {
        Logf("  refNum=%d: no DCE", refNum);
        return;
    }
    dce = *dceH;
    aux = (AuxDCEPtr)dce;

    Logf("  DCE refNum=%d @ 0x%08lX", refNum, (unsigned long)dce);
    Logf("    dCtlDriver=0x%08lX flags=0x%04X (RAMBased=%d) storage=0x%08lX",
         (unsigned long)dce->dCtlDriver, (unsigned short)dce->dCtlFlags,
         (dce->dCtlFlags & dRAMBasedMask) ? 1 : 0,
         (unsigned long)dce->dCtlStorage);
    Logf("    dCtlPosition=%ld dCtlDelay=%d dCtlEMask=0x%04X dCtlMenu=%d",
         dce->dCtlPosition, dce->dCtlDelay,
         (unsigned short)dce->dCtlEMask, dce->dCtlMenu);
    Logf("    aux: slot=%d slotId=%d devBase=0x%08lX owner=0x%08lX extDev=%d",
         aux->dCtlSlot, aux->dCtlSlotId, (unsigned long)aux->dCtlDevBase,
         (unsigned long)aux->dCtlOwner, aux->dCtlExtDev);
    Logf("    aux: dCtlNodeID=0x%08lX", (unsigned long)aux->dCtlNodeID);

    /* Native? */
    memset(&regID, 0, sizeof(regID));
    memset(&desc, 0, sizeof(desc));
    name[0] = 0;
    err = GetDriverInformation(refNum, &unitNum, &flags, &openCount,
                               name, &regID, NULL, NULL, NULL, &desc);
    if (err == noErr) {
        *isNativeOut = true;
        BlockMoveData(name, nameOut, name[0] + 1);
        PToC(name, cname, sizeof(cname));
        Logf("    GetDriverInformation: NATIVE ndrv — unit=%u flags=0x%04X "
             "openCount=%lu name='%s'",
             (unsigned)unitNum, (unsigned short)flags,
             (unsigned long)openCount, cname);
        PToC(desc.driverType.nameInfoStr, cname, sizeof(cname));
        Logf("    DriverDescription: type='%s' version=0x%08lX",
             cname, (unsigned long)*(UInt32 *)&desc.driverType.version);
    } else {
        Logf("    GetDriverInformation: err=%d ⇒ treat as CLASSIC 'DRVR'", err);
        /* Classic: dCtlDriver is a Handle when dRAMBased, else a Ptr. The DRVR
         * header's entry points are offsets — the classic patch targets. */
        {
            DRVRHeaderPtr h = NULL;
            if (dce->dCtlFlags & dRAMBasedMask) {
                Handle hh = (Handle)dce->dCtlDriver;
                if (hh != NULL && *hh != NULL) h = (DRVRHeaderPtr)(*hh);
            } else {
                h = (DRVRHeaderPtr)dce->dCtlDriver;
            }
            if (h != NULL) {
                Logf("    DRVR hdr: flags=0x%04X delay=%d emask=0x%04X menu=%d",
                     (unsigned short)h->drvrFlags, h->drvrDelay,
                     (unsigned short)h->drvrEMask, h->drvrMenu);
                Logf("    DRVR entry offsets: open=%d prime=%d ctl=%d "
                     "status=%d close=%d",
                     h->drvrOpen, h->drvrPrime, h->drvrCtl,
                     h->drvrStatus, h->drvrClose);
                /* drvrName is a Pascal string at the end of the header. */
                if (h->drvrName[0] > 0 && h->drvrName[0] < 64) {
                    BlockMoveData(h->drvrName, nameOut, h->drvrName[0] + 1);
                    PToC(h->drvrName, cname, sizeof(cname));
                    Logf("    DRVR name='%s'", cname);
                }
                LogHex("DRVRhdr", h, 32);
            }
        }
    }

    /* Whatever it is, dump the first bytes at dCtlDriver so the shape is on
     * record even if both classifications surprise us. */
    if (dce->dCtlDriver != NULL && !(dce->dCtlFlags & dRAMBasedMask))
        LogHex("dCtlDriver", dce->dCtlDriver, 32);
}

/* Walk the unit table, DriverGestalt-query each populated entry for 'devt', and
 * pick the one that says 'cdrm'.
 *
 * Deliberately NOT matching on the name ".AppleCD": the ATAPI-era driver's name
 * varies across builds, and a name match is exactly the assumption that breaks
 * silently. 'devt' == 'cdrm' is the driver telling us what it is.
 *
 * The unit table is walked rather than the drive queue because the drive queue
 * only lists drives, and we want this to work with the tray empty too. */
static void FindCDDriver(void)
{
    Ptr    utable = LM_UTableBase;
    short  count  = LM_UnitEntryCount;
    short  i;

    Logf("--- P2: unit table sweep (base=0x%08lX entries=%d) ---",
         (unsigned long)utable, count);

    if (utable == NULL || count <= 0 || count > 512) {
        Logf("  unit table looks implausible; aborting sweep");
        return;
    }

    for (i = 0; i < count; i++) {
        DCtlHandle dceH = ((DCtlHandle *)utable)[i];
        short      refNum;
        UInt32     devt = 0, intf = 0;
        OSErr      errT, errI;

        if (dceH == NULL) continue;
        refNum = (short)~i;            /* refNum = one's complement of unit num */

        errT = DriverGestaltQuery(refNum, kdgDeviceType, &devt);
        if (errT != noErr) continue;   /* no 'devt' ⇒ not a disk-family driver */

        errI = DriverGestaltQuery(refNum, kdgInterface, &intf);
        Logf("  unit=%-3d refNum=%-5d devt='%.4s' intf='%.4s'%s",
             i, refNum, (char *)&devt,
             (errI == noErr) ? (char *)&intf : "????",
             (devt == kdgCDType) ? "   <-- CD-ROM" : "");

        if (devt == kdgCDType && !gR.foundCD) {
            gR.foundCD       = true;
            gR.cdRefNum      = refNum;
            gR.deviceType    = devt;
            gR.interfaceType = (errI == noErr) ? intf : 0;
        }
    }

    if (!gR.foundCD) {
        Logf("  no driver reported devt=='cdrm'.");
        return;
    }

    Logf("--- P2: chosen CD driver refNum=%d ---", gR.cdRefNum);
    DumpDCE(gR.cdRefNum, &gR.isNative, gR.driverName);

    /* Extra selectors that matter for the later design: 'dvrf' is the
     * interface-specific device reference we would need for route-B (ATA
     * Manager) DAE, and 'nmrg' is the Name Registry entry for the device. */
    {
        UInt32 v = 0;
        if (DriverGestaltQuery(gR.cdRefNum, kdgDeviceReference, &v) == noErr)
            Logf("  'dvrf' device reference = 0x%08lX (route-B ATA handle)",
                 (unsigned long)v);
        if (DriverGestaltQuery(gR.cdRefNum, kdgNameRegistryEntry, &v) == noErr)
            Logf("  'nmrg' name registry entry ptr = 0x%08lX", (unsigned long)v);
        if (DriverGestaltQuery(gR.cdRefNum, kdgSync, &v) == noErr)
            Logf("  'sync' synchronous-only = %lu", (unsigned long)v);
        if (DriverGestaltQuery(gR.cdRefNum, kdgVersion, &v) == noErr)
            Logf("  'vers' driver version = 0x%08lX", (unsigned long)v);
    }

    /* Cross-reference the drive queue for this driver's drive number, needed for
     * the driver-level PBRead in P4. */
    {
        DrvQElPtr q = (DrvQElPtr)LM_DrvQHdr->qHead;
        Logf("--- drive queue ---");
        while (q != NULL) {
            Logf("  drive=%d refNum=%d fsID=%d size=%u/%u%s",
                 q->dQDrive, q->dQRefNum, q->dQFSID,
                 q->dQDrvSz, q->dQDrvSz2,
                 (q->dQRefNum == gR.cdRefNum) ? "   <-- our CD" : "");
            if (q->dQRefNum == gR.cdRefNum && gR.cdDriveNum == 0)
                gR.cdDriveNum = q->dQDrive;
            q = (DrvQElPtr)q->qLink;
        }
        if (gR.cdDriveNum == 0)
            Logf("  no drive queue entry for refNum=%d (empty tray?)",
                 gR.cdRefNum);
    }
}

/* ---- read-only status surface + TOC -------------------------------------- */

/* ReadTOC with a given action, trying both csParam encodings.
 *
 * The action code sits at csParam byte offset 0, but nothing in the available
 * source material settles whether the driver reads a BYTE there or a WORD. Both
 * are tried: 0x000N puts N in the low byte of word 0, 0x0N00 puts it in the high
 * byte (= byte offset 0). Whichever the driver accepts is the answer, and it
 * goes in cd_cscodes.h afterwards. */
static OSErr ReadTOCAction(short refNum, short action, Boolean asControl,
                           void *out, long outLen, const char **encUsed)
{
    short  param[11];
    OSErr  err;

    /* encoding 1: word at offset 0 */
    memset(param, 0, sizeof(param));
    param[0] = action;
    err = asControl
        ? ControlCall(refNum, kcsReadTOC, param, sizeof(param), out, outLen)
        : StatusCall(refNum, kcsReadTOC, out, outLen);
    if (err == noErr) { if (encUsed) *encUsed = "word"; return noErr; }

    /* Status calls take no input; retrying the encoding only makes sense for the
     * Control form, which is where csParam is an input. */
    if (!asControl) { if (encUsed) *encUsed = "n/a"; return err; }

    /* encoding 2: byte at offset 0 */
    memset(param, 0, sizeof(param));
    param[0] = (short)(action << 8);
    err = ControlCall(refNum, kcsReadTOC, param, sizeof(param), out, outLen);
    if (encUsed) *encUsed = (err == noErr) ? "byte" : "neither";
    return err;
}

static void ProbeStatusSurface(void)
{
    short  refNum = gR.cdRefNum;
    short  buf[11];
    OSErr  err;

    Logf("--- read-only status surface ---");

    /* Simple one-word answers. Errors are expected on some of these and are
     * themselves the interesting result. */
    if (StatusCall(refNum, kcsGetBlockSize, buf, sizeof(buf)) == noErr) {
        gR.blockSize = buf[0];
        Logf("  GetBlockSize = %d", buf[0]);
    }
    if (StatusCall(refNum, kcsGetDriveType, buf, sizeof(buf)) == noErr)
        Logf("  GetDriveType = %d (3 = CD300 or later)", buf[0]);
    if (StatusCall(refNum, kcsGet2KOffset, buf, sizeof(buf)) == noErr)
        Logf("  Get2KOffset = %d", buf[0]);
    if (StatusCall(refNum, kcsGetCDFeatures, buf, sizeof(buf)) == noErr)
        Logf("  GetCDFeatures: speed=%d features=0x%04X",
             buf[0], (unsigned short)buf[1]);
    if (StatusCall(refNum, kcsReturnDeviceIdent, buf, sizeof(buf)) == noErr)
        Logf("  ReturnDeviceIdent = 0x%04X", (unsigned short)buf[0]);
    (void)StatusCall(refNum, kcsDriveStatus, buf, sizeof(buf));
    (void)StatusCall(refNum, kcsWhoIsThere, buf, sizeof(buf));

    /* --- the audio surface. This is the H1 vs H2 discriminator. --- *
     * If AudioStatus answers, the driver implements the legacy audio calls and
     * the games' AudioPlay is being accepted but has nowhere to go (H1). If it
     * refuses both as Status and as Control, the driver doesn't implement them
     * and the games are getting an error back (H2) — which means our extension
     * has to synthesise convincing replies, not just add sound. */
    Logf("--- audio surface (H1 vs H2) ---");
    err = StatusCall(refNum, kcsAudioStatus, buf, sizeof(buf));
    gR.audioStatusErr    = err;
    gR.audioStatusIsCtl  = false;
    if (err != noErr) {
        Logf("  AudioStatus as Status failed (err=%d); retrying as Control", err);
        err = ControlCall(refNum, kcsAudioStatus, NULL, 0, buf, sizeof(buf));
        if (err == noErr) { gR.audioStatusErr = noErr; gR.audioStatusIsCtl = true; }
        else               gR.audioStatusErr = err;
    }
    Logf("  ⇒ AudioStatus err=%d answered-as=%s",
         gR.audioStatusErr, gR.audioStatusIsCtl ? "Control" : "Status");

    err = StatusCall(refNum, kcsReadTheQSubcode, buf, sizeof(buf));
    if (err != noErr)
        (void)ControlCall(refNum, kcsReadTheQSubcode, NULL, 0, buf, sizeof(buf));

    err = StatusCall(refNum, kcsReadAudioVolume, buf, sizeof(buf));
    if (err == noErr)
        Logf("  ReadAudioVolume: left=%d right=%d",
             ((unsigned char *)buf)[0], ((unsigned char *)buf)[1]);

    (void)StatusCall(refNum, kcsGetPlayMode, buf, sizeof(buf));

    /* --- TOC --- */
    Logf("--- TOC ---");
    {
        const char *enc = "?";
        unsigned char *p = (unsigned char *)buf;

        err = ReadTOCAction(refNum, kTOCActionFirstLast, false,
                            buf, sizeof(buf), &enc);
        if (err != noErr)
            err = ReadTOCAction(refNum, kTOCActionFirstLast, true,
                                buf, sizeof(buf), &enc);
        if (err == noErr) {
            gR.tocOK      = true;
            gR.firstTrack = kBCDToBin(p[0]);
            gR.lastTrack  = kBCDToBin(p[1]);
            Logf("  ReadTOC first/last: first=%d last=%d (BCD %02X %02X) enc=%s",
                 gR.firstTrack, gR.lastTrack, p[0], p[1], enc);
        } else {
            Logf("  ReadTOC first/last FAILED err=%d (no disc, or the driver "
                 "does not implement ReadTOC)", err);
        }

        err = ReadTOCAction(refNum, kTOCActionLeadOut, false,
                            buf, sizeof(buf), &enc);
        if (err != noErr)
            err = ReadTOCAction(refNum, kTOCActionLeadOut, true,
                                buf, sizeof(buf), &enc);
        if (err == noErr)
            Logf("  ReadTOC lead-out: %02d:%02d:%02d (BCD) enc=%s",
                 kBCDToBin(p[0]), kBCDToBin(p[1]), kBCDToBin(p[2]), enc);
    }

    /* Per-track addresses. Needs a caller-supplied buffer: csParam+2 is the
     * buffer address, +6 the size, +8 the starting track (BCD). Each entry is
     * 4 bytes: control/adr byte then M, S, F (BCD). */
    if (gR.tocOK) {
        unsigned char *tocBuf = (unsigned char *)NewPtrClear(4 * 100);
        if (tocBuf != NULL) {
            short param[11];
            OSErr terr;
            int   n = gR.lastTrack - gR.firstTrack + 1;
            if (n < 1)   n = 1;
            if (n > 99)  n = 99;

            memset(param, 0, sizeof(param));
            param[0] = kTOCActionTrackAddrs;
            *(Ptr *)&param[1]  = (Ptr)tocBuf;               /* +2: buffer      */
            *(long *)&param[3] = (long)(4 * n);             /* +6: size        */
            ((unsigned char *)param)[8] = kBinToBCD(gR.firstTrack); /* +8       */

            LogStep("ReadTOC track-addresses (n=%d)", n);
            terr = ControlCall(gR.cdRefNum, kcsReadTOC, param, sizeof(param),
                               NULL, 0);
            if (terr != noErr) {
                /* try the byte-at-offset-0 encoding of the action */
                memset(param, 0, sizeof(param));
                param[0] = (short)(kTOCActionTrackAddrs << 8);
                *(Ptr *)&param[1]  = (Ptr)tocBuf;
                *(long *)&param[3] = (long)(4 * n);
                ((unsigned char *)param)[8] = kBinToBCD(gR.firstTrack);
                terr = ControlCall(gR.cdRefNum, kcsReadTOC, param,
                                   sizeof(param), NULL, 0);
            }
            Logf("  ReadTOC track-addresses err=%d", terr);
            if (terr == noErr) {
                int t;
                LogHex("toc", tocBuf, 4 * n);
                for (t = 0; t < n; t++) {
                    unsigned char *e   = tocBuf + 4 * t;
                    int  ctrl  = (e[0] >> 4) & 0x0F;
                    int  m     = kBCDToBin(e[1]);
                    int  s     = kBCDToBin(e[2]);
                    int  f     = kBCDToBin(e[3]);
                    long lba   = ((long)m * 60 + s) * kCDDASectorsPerSec + f
                                 - kCDDALeadInSectors;
                    /* control field bit 2 set ⇒ data track; clear ⇒ audio. */
                    Boolean isData = (ctrl & 0x04) != 0;
                    Logf("    track %2d: ctrl=0x%X %s  %02d:%02d:%02d  lba=%ld",
                         gR.firstTrack + t, ctrl,
                         isData ? "DATA " : "AUDIO", m, s, f, lba);
                    if (!isData) {
                        gR.audioTrackCount++;
                        /* remember the first audio track's LBA for the P4 probe,
                         * offset ~10 s in to skip any silent pregap */
                        if (gR.daeLBA == 0)
                            gR.daeLBA = lba + 10 * kCDDASectorsPerSec;
                    }
                }
                Logf("  ⇒ %d audio track(s) found", gR.audioTrackCount);
            }
            DisposePtr((Ptr)tocBuf);
        }
    }
}

/* ---- P3: do the audio tracks show up as FILES? --------------------------- */

/* Survey every mounted volume, and list the root directory of any volume served
 * by the CD driver. If Audio CD Access / Foreign File Access surfaces the audio
 * session as a volume of AIFF track files, DAE is a plain FSRead and a large
 * chunk of the planned design is unnecessary. */
static void SurveyVolumes(void)
{
    HParamBlockRec  hpb;
    short           index;

    Logf("--- P3: mounted volumes ---");

    for (index = 1; index < 64; index++) {
        Str255 vName;
        OSErr  err;

        memset(&hpb, 0, sizeof(hpb));
        vName[0] = 0;
        hpb.volumeParam.ioNamePtr  = vName;
        hpb.volumeParam.ioVolIndex = index;
        err = PBHGetVInfoSync(&hpb);
        if (err != noErr) break;

        {
            char cname[64];
            PToC(vName, cname, sizeof(cname));
            Logf("  vol %d: '%s' vRefNum=%d drvNum=%d drvRefNum=%d sig=0x%04X "
                 "fsID=%d files=%ld",
                 index, cname, hpb.volumeParam.ioVRefNum,
                 hpb.volumeParam.ioVDrvInfo, hpb.volumeParam.ioVDRefNum,
                 (unsigned short)hpb.volumeParam.ioVSigWord,
                 hpb.volumeParam.ioVFSID, hpb.volumeParam.ioVNmFls);
        }

        /* Is this volume on our optical drive? */
        if (hpb.volumeParam.ioVDRefNum != gR.cdRefNum) continue;

        Logf("    ^ this volume is on the CD driver — listing root:");
        {
            CInfoPBRec cpb;
            short      fIndex;

            for (fIndex = 1; fIndex < 200; fIndex++) {
                Str255 fName;
                char   cname[64];
                OSErr  ferr;

                memset(&cpb, 0, sizeof(cpb));
                fName[0] = 0;
                cpb.hFileInfo.ioNamePtr   = fName;
                cpb.hFileInfo.ioVRefNum   = hpb.volumeParam.ioVRefNum;
                cpb.hFileInfo.ioDirID     = fsRtDirID;
                cpb.hFileInfo.ioFDirIndex = fIndex;
                ferr = PBGetCatInfoSync(&cpb);
                if (ferr != noErr) break;

                PToC(fName, cname, sizeof(cname));
                if (cpb.hFileInfo.ioFlAttrib & ioDirMask) {
                    Logf("      dir  '%s'", cname);
                } else {
                    OSType type    = cpb.hFileInfo.ioFlFndrInfo.fdType;
                    OSType creator = cpb.hFileInfo.ioFlFndrInfo.fdCreator;
                    Logf("      file '%s' type='%.4s' creator='%.4s' "
                         "dataEOF=%ld",
                         cname, (char *)&type, (char *)&creator,
                         cpb.hFileInfo.ioFlLgLen);
                    /* An audio track surfaced as a file: AIFF/AIFC, or the
                     * QuickTime CD-audio track type. */
                    if (type == 'AIFF' || type == 'AIFC' || type == 'cdda' ||
                        type == 'trak') {
                        gR.audioFilesFound = true;
                        BlockMoveData(vName, gR.audioFileVolume, vName[0] + 1);
                        Logf("      ^^^ AUDIO TRACK AS A FILE — P3 is GO");
                    }
                }
            }
        }
    }

    if (!gR.audioFilesFound)
        Logf("  no audio-track files found on any CD-driver volume "
             "(expected if the audio session doesn't mount)");
}

/* ---- P4: THE GATE — can we read CD-DA sectors? --------------------------- */

/* Route A: ask the driver to switch to a 2352-byte block size, then do a
 * driver-level PBRead at an audio track's LBA. Restores the block size on every
 * exit path.
 *
 * If the driver refuses 2352, route A is out and route B (ATAPI READ CD 0xBE via
 * the ATA Manager, using the 'dvrf' device reference logged above) is next. The
 * point of this probe is to find out which, cheaply, before anyone writes a
 * streaming engine. */
static void ProbeDAE(void)
{
    short          param[11];
    OSErr          err;
    unsigned char *buf;
    const long     kSectors  = 4;
    const long     kReadSize = kSectors * kCDDASectorBytes;
    short          savedBlockSize = gR.blockSize ? gR.blockSize : 2048;

    Logf("--- P4: DAE probe (route A: block size 2352 + driver read) ---");

    if (gR.audioTrackCount == 0 || gR.daeLBA <= 0) {
        Logf("  skipped: no audio track LBA known (need a mixed-mode or audio "
             "disc in the drive)");
        return;
    }
    if (gR.cdDriveNum == 0) {
        Logf("  skipped: no drive number for refNum=%d, cannot issue a "
             "driver-level read", gR.cdRefNum);
        return;
    }

    buf = (unsigned char *)NewPtrClear(kReadSize);
    if (buf == NULL) { Logf("  skipped: out of memory"); return; }

    /* 1. switch block size to 2352 */
    memset(param, 0, sizeof(param));
    param[0] = kCDDASectorBytes;
    LogStep("ChangeBlockSize -> 2352");
    err = ControlCall(gR.cdRefNum, kcsChangeBlockSize, param, sizeof(param),
                      NULL, 0);
    Logf("  ChangeBlockSize(2352) err=%d", err);
    if (err != noErr) {
        gR.dae    = kDAEBlockSizeRefused;
        gR.daeErr = err;
        Logf("  ⇒ route A BLOCKED: driver will not do 2352-byte blocks. "
             "Next: route B, ATAPI READ CD (0xBE) via the ATA Manager.");
        DisposePtr((Ptr)buf);
        return;
    }

    /* 2. driver-level read at the audio LBA */
    {
        ParamBlockRec pb;
        memset(&pb, 0, sizeof(pb));
        pb.ioParam.ioRefNum    = gR.cdRefNum;
        pb.ioParam.ioVRefNum   = gR.cdDriveNum;
        pb.ioParam.ioBuffer    = (Ptr)buf;
        pb.ioParam.ioReqCount  = kReadSize;
        pb.ioParam.ioPosMode   = fsFromStart;
        pb.ioParam.ioPosOffset = gR.daeLBA * kCDDASectorBytes;

        LogStep("PBRead lba=%ld off=%ld len=%ld drive=%d",
                gR.daeLBA, pb.ioParam.ioPosOffset, kReadSize, gR.cdDriveNum);
        err = PBReadSync(&pb);
        gR.daeErr = err;
        Logf("  PBRead err=%d actCount=%ld", err, pb.ioParam.ioActCount);

        if (err == noErr && pb.ioParam.ioActCount > 0) {
            /* Is this plausibly PCM? Real music is high-entropy under both byte
             * orders; a silent pregap is all zeros. Peak amplitude read both
             * ways tells us we got audio rather than a block of zeros or a
             * repeating pattern. */
            long  i, nonZero = 0;
            long  peakLE = 0, peakBE = 0;
            long  n = pb.ioParam.ioActCount;
            if (n > kReadSize) n = kReadSize;

            for (i = 0; i + 1 < n; i += 2) {
                short le = (short)((buf[i + 1] << 8) | buf[i]);
                short be = (short)((buf[i] << 8) | buf[i + 1]);
                long  ale = le < 0 ? -(long)le : le;
                long  abe = be < 0 ? -(long)be : be;
                if (ale > peakLE) peakLE = ale;
                if (abe > peakBE) peakBE = abe;
                if (buf[i] != 0)     nonZero++;
                if (buf[i + 1] != 0) nonZero++;
            }
            gR.daeNonZeroBytes = nonZero;
            gR.daePeakLE = peakLE;
            gR.daePeakBE = peakBE;
            Logf("  content: nonZeroBytes=%ld/%ld peakLE=%ld peakBE=%ld",
                 nonZero, n, peakLE, peakBE);
            LogHex("cdda", buf, 64);

            if (nonZero * 4 > n) {   /* >25% non-zero ⇒ not silence */
                gR.dae = kDAEGo;
                Logf("  ⇒ P4 GO: CD-DA sectors are readable through the driver. "
                     "The extension approach is viable.");
            } else {
                gR.dae = kDAEReadOK;
                Logf("  ⇒ read SUCCEEDED but the data is (near) silence. Either "
                     "the LBA landed in a silent passage or the read returned "
                     "zeros. Re-run with a different track/offset before "
                     "concluding.");
            }
        } else {
            gR.dae = kDAEReadFailed;
            Logf("  ⇒ route A read FAILED. Next: route B, ATAPI READ CD (0xBE) "
                 "via the ATA Manager ('dvrf' handle above).");
        }
    }

    /* 3. restore block size — on every path */
    memset(param, 0, sizeof(param));
    param[0] = savedBlockSize;
    LogStep("ChangeBlockSize -> %d (restore)", savedBlockSize);
    err = ControlCall(gR.cdRefNum, kcsChangeBlockSize, param, sizeof(param),
                      NULL, 0);
    Logf("  ChangeBlockSize(%d) restore err=%d", savedBlockSize, err);
    if (err != noErr)
        Logf("  !! block size NOT restored. Eject and re-insert the disc "
             "before trusting further reads.");

    DisposePtr((Ptr)buf);
}

/* ---- summary window ------------------------------------------------------ */

static void DrawLabelled(short v, const char *label, const char *value)
{
    Str255 s;
    int    n = 0;
    char   tmp[256];

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
        case kDAEGo:               return "GO — CD-DA readable via the driver";
        case kDAEReadOK:           return "read OK but silent — re-run";
        case kDAEBlockSizeRefused: return "blocked — 2352 refused (try route B)";
        case kDAEReadFailed:       return "blocked — read failed (try route B)";
        default:                   return "not run";
    }
}

static void ShowSummary(void)
{
    WindowPtr win;
    Rect      bounds;
    EventRecord evt;
    char      tmp[256];
    short     v = 22;

    SetRect(&bounds, 40, 60, 40 + 560, 60 + 340);
    win = NewWindow(NULL, &bounds, "\p" kVersionString, true, documentProc,
                    (WindowPtr)-1L, false, 0);
    if (win == NULL) return;
    SetPort(win);
    TextFont(kFontIDGeneva);
    TextSize(9);

    DrawLabelled(v, "CD Audio Redirector — Phase 0 recon", ""); v += 18;

    if (!gR.foundCD) {
        DrawLabelled(v, "No driver reported devt=='cdrm'. ", ""); v += 14;
        DrawLabelled(v, "See 'CD Recon Log' in the System Folder.", ""); v += 14;
    } else {
        char nameC[64];
        PToC(gR.driverName, nameC, sizeof(nameC));

        snprintf(tmp, sizeof(tmp), "%d  (drive %d)", gR.cdRefNum, gR.cdDriveNum);
        DrawLabelled(v, "P2  CD driver refNum: ", tmp); v += 14;

        snprintf(tmp, sizeof(tmp), "%s   name '%s'",
                 gR.isNative ? "NATIVE 'ndrv'" : "classic 'DRVR'", nameC);
        DrawLabelled(v, "P2  driver kind:      ", tmp); v += 14;

        snprintf(tmp, sizeof(tmp), "devt='%.4s'  intf='%.4s'  blockSize=%d",
                 (char *)&gR.deviceType, (char *)&gR.interfaceType,
                 gR.blockSize);
        DrawLabelled(v, "P2  identity:         ", tmp); v += 18;

        if (gR.tocOK)
            snprintf(tmp, sizeof(tmp), "tracks %d..%d, %d audio",
                     gR.firstTrack, gR.lastTrack, gR.audioTrackCount);
        else
            snprintf(tmp, sizeof(tmp), "ReadTOC failed (disc in the drive?)");
        DrawLabelled(v, "    TOC:              ", tmp); v += 14;

        snprintf(tmp, sizeof(tmp), "err=%d answered as %s  ⇒ %s",
                 gR.audioStatusErr,
                 gR.audioStatusIsCtl ? "Control" : "Status",
                 (gR.audioStatusErr == noErr)
                     ? "H1 (driver accepts audio calls)"
                     : "H2 (driver rejects them)");
        DrawLabelled(v, "    AudioStatus:      ", tmp); v += 18;

        if (gR.audioFilesFound) {
            char volC[64];
            PToC(gR.audioFileVolume, volC, sizeof(volC));
            snprintf(tmp, sizeof(tmp), "YES on '%s' — DAE could be plain FSRead",
                     volC);
        } else {
            snprintf(tmp, sizeof(tmp), "no audio-track files mounted");
        }
        DrawLabelled(v, "P3  tracks as files:  ", tmp); v += 18;

        snprintf(tmp, sizeof(tmp), "%s", DAEName());
        DrawLabelled(v, "P4  DAE gate:         ", tmp); v += 14;
        if (gR.dae == kDAEGo || gR.dae == kDAEReadOK) {
            snprintf(tmp, sizeof(tmp), "lba=%ld nonZero=%ld peakLE=%ld peakBE=%ld",
                     gR.daeLBA, gR.daeNonZeroBytes, gR.daePeakLE, gR.daePeakBE);
            DrawLabelled(v, "    DAE detail:       ", tmp); v += 14;
        }
        v += 8;
        DrawLabelled(v, "Full detail: 'CD Recon Log' in the System Folder.", "");
        v += 14;
        DrawLabelled(v, "Read from the LAST banner — the log appends.", "");
        v += 18;
    }

    DrawLabelled(v, "Click or press a key to quit.", "");

    /* Wait for a click or key. */
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
    Boolean skipDAE;

    /* Explicit Toolbox init. This app deliberately does not use RetroConsole:
     * its Toolbox init is lazy (fires on the first printf), and building windows
     * before that happens scribbles the heap. */
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    GetKeys(km);
    skipDAE = KeyIsDown(km, kOptionKeyCode);

    memset(&gR, 0, sizeof(gR));

    LogOpen();
    Logf("");
    Logf("========================================================");
    Logf("=== %s — CD Audio Redirector Phase 0 recon", kVersionString);
    Logf("=== option key held: %s", skipDAE ? "YES (P4 skipped)" : "no");
    Logf("========================================================");
    LogFlush();

    FindCDDriver();

    if (gR.foundCD) {
        ProbeStatusSurface();
        SurveyVolumes();
        if (skipDAE)
            Logf("--- P4: SKIPPED (option key held) ---");
        else
            ProbeDAE();
    }

    Logf("=== end of run: foundCD=%d native=%d tocOK=%d audioTracks=%d "
         "audioFiles=%d daeVerdict=%d",
         gR.foundCD, gR.isNative, gR.tocOK, gR.audioTrackCount,
         gR.audioFilesFound, (int)gR.dae);
    LogClose();

    ShowSummary();
    return 0;
}

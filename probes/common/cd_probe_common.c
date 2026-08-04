/*
 * cd_probe_common.c — shared plumbing for the CD Audio Redirector probes.
 * See cd_probe_common.h for the why.
 */

#include "cd_probe_common.h"
#include "cd_cscodes.h"

#include <Disks.h>
#include <Folders.h>
#include <Memory.h>
#include <DriverGestalt.h>
#include <DriverFamilyMatching.h>   /* DriverDescription / MacDriverType */
#include <NameRegistry.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- logging -------------------------------------------------------------- */

static short gLogRef   = 0;
static short gLogVRef  = 0;

void CDLogOpen(ConstStr255Param fileName)
{
    short  vRefNum;
    long   dirID;
    FSSpec spec;
    long   eof;

    gLogRef = 0;
    if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return;
    gLogVRef = vRefNum;
    if (FSMakeFSSpec(vRefNum, dirID, fileName, &spec) != noErr) {
        if (FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript) != noErr) return;
    }
    if (FSpOpenDF(&spec, fsRdWrPerm, &gLogRef) != noErr) { gLogRef = 0; return; }
    if (GetEOF(gLogRef, &eof) == noErr) SetFPos(gLogRef, fsFromStart, eof);
}

void CDLogFlush(void)
{
    if (gLogRef == 0) return;
    FlushVol(NULL, gLogVRef);
}

void CDLogClose(void)
{
    if (gLogRef == 0) return;
    CDLogFlush();
    FSClose(gLogRef);
    gLogRef = 0;
}

void CDLogf(const char *fmt, ...)
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

void CDLogStep(const char *fmt, ...)
{
    char    buf[480];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    CDLogf("STEP %s", buf);
    CDLogFlush();
}

void CDLogHex(const char *tag, const void *p, long n)
{
    const unsigned char *b = (const unsigned char *)p;
    char  line[128];
    long  i, off = 0;

    while (off < n) {
        long chunk = n - off;
        int  used  = 0;
        if (chunk > 16) chunk = 16;
        for (i = 0; i < chunk; i++)
            used += snprintf(line + used, sizeof(line) - used,
                             "%02X ", b[off + i]);
        CDLogf("  %s +%04ld: %s", tag, off, line);
        off += chunk;
    }
}

void CDPToC(ConstStr255Param src, char *dst, int dstSize)
{
    int len = (src == NULL) ? 0 : src[0];
    if (len > dstSize - 1) len = dstSize - 1;
    if (len > 0) BlockMoveData(src + 1, dst, len);
    dst[len] = 0;
}

void CDLogBanner(const char *probeName, const char *note)
{
    CDLogf("");
    CDLogf("========================================================");
    CDLogf("=== %s", probeName);
    if (note != NULL && note[0] != 0) CDLogf("=== %s", note);
    CDLogf("========================================================");
    CDLogFlush();
}

/* ---- Device Manager wrappers ---------------------------------------------- */

OSErr CDStatusCall(short refNum, short csCode, void *paramOut, long paramOutLen)
{
    CntrlParam pb;
    OSErr      err;

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = refNum;
    pb.csCode    = csCode;

    CDLogStep("Status csCode=%d refNum=%d", csCode, refNum);
    err = PBStatusSync((ParmBlkPtr)&pb);

    CDLogf("  Status csCode=%-3d err=%-6d csParam:", csCode, err);
    CDLogHex("csParam", pb.csParam, 22);
    if (paramOut != NULL) {
        if (paramOutLen > 22) paramOutLen = 22;
        BlockMoveData(pb.csParam, paramOut, paramOutLen);
    }
    return err;
}

OSErr CDControlCall(short refNum, short csCode,
                    const void *paramIn, long paramInLen,
                    void *paramOut, long paramOutLen)
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

    CDLogStep("Control csCode=%d refNum=%d", csCode, refNum);
    err = PBControlSync((ParmBlkPtr)&pb);

    CDLogf("  Control csCode=%-3d err=%-6d csParam:", csCode, err);
    CDLogHex("csParam", pb.csParam, 22);
    if (paramOut != NULL) {
        if (paramOutLen > 22) paramOutLen = 22;
        BlockMoveData(pb.csParam, paramOut, paramOutLen);
    }
    return err;
}

OSErr CDDriverGestalt(short refNum, OSType selector, UInt32 *response)
{
    DriverGestaltParam pb;
    OSErr              err;

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum             = refNum;
    pb.csCode                = kcsDriverGestalt;
    pb.driverGestaltSelector = selector;

    err = PBStatusSync((ParmBlkPtr)&pb);
    if (response != NULL) *response = pb.driverGestaltResponse;
    return err;
}

/* ---- discovery ------------------------------------------------------------ */

void CDDumpDCE(CDDriverInfo *info)
{
    DCtlHandle        dceH;
    DCtlPtr           dce;
    AuxDCEPtr         aux;
    UnitNumber        unitNum   = 0;
    DriverFlags       flags     = 0;
    DriverOpenCount   openCount = 0;
    Str255            name;
    RegEntryID        regID;
    DriverDescription desc;
    OSErr             err;
    char              cname[64];

    info->isNative = false;
    info->name[0]  = 0;

    /* GetDCtlEntry returns a HANDLE to the DCE (DCtlHandle == DCtlPtr *), so it
     * needs one dereference. */
    dceH = GetDCtlEntry(info->refNum);
    if (dceH == NULL || *dceH == NULL) {
        CDLogf("  refNum=%d: no DCE", info->refNum);
        return;
    }
    dce = *dceH;
    aux = (AuxDCEPtr)dce;

    CDLogf("  DCE refNum=%d @ 0x%08lX", info->refNum, (unsigned long)dce);
    CDLogf("    dCtlDriver=0x%08lX flags=0x%04X (RAMBased=%d) storage=0x%08lX",
           (unsigned long)dce->dCtlDriver, (unsigned short)dce->dCtlFlags,
           (dce->dCtlFlags & dRAMBasedMask) ? 1 : 0,
           (unsigned long)dce->dCtlStorage);
    CDLogf("    dCtlPosition=%ld dCtlDelay=%d dCtlEMask=0x%04X dCtlMenu=%d",
           dce->dCtlPosition, dce->dCtlDelay,
           (unsigned short)dce->dCtlEMask, dce->dCtlMenu);
    CDLogf("    aux: slot=%d slotId=%d devBase=0x%08lX owner=0x%08lX extDev=%d",
           aux->dCtlSlot, aux->dCtlSlotId, (unsigned long)aux->dCtlDevBase,
           (unsigned long)aux->dCtlOwner, aux->dCtlExtDev);
    CDLogf("    aux: dCtlNodeID=0x%08lX  <-- non-zero implies a native driver "
           "with a Name Registry entry", (unsigned long)aux->dCtlNodeID);

    /* The classifier. GetDriverInformation only knows about natively-installed
     * drivers, so success means native — and hands back the unit number, the
     * Name Registry entry, and the DriverDescription for free. Failure means we
     * are looking at a classic 'DRVR', whose Control/Status entry points are
     * 16-bit OFFSETS in the DRVR header. Those offsets are what a classic
     * interception patches, so log them either way. */
    memset(&regID, 0, sizeof(regID));
    memset(&desc, 0, sizeof(desc));
    name[0] = 0;
    err = GetDriverInformation(info->refNum, &unitNum, &flags, &openCount,
                               name, &regID, NULL, NULL, NULL, &desc);
    if (err == noErr) {
        info->isNative = true;
        BlockMoveData(name, info->name, name[0] + 1);
        CDPToC(name, cname, sizeof(cname));
        CDLogf("    GetDriverInformation: NATIVE ndrv — unit=%u flags=0x%04X "
               "openCount=%lu name='%s'",
               (unsigned)unitNum, (unsigned short)flags,
               (unsigned long)openCount, cname);
        CDPToC(desc.driverType.nameInfoStr, cname, sizeof(cname));
        CDLogf("    DriverDescription: type='%s' version=0x%08lX",
               cname, (unsigned long)*(UInt32 *)&desc.driverType.version);
        CDLogf("    ⇒ INTERCEPTION MECHANISM: native dispatch, not DRVR offsets");
    } else {
        DRVRHeaderPtr h = NULL;

        CDLogf("    GetDriverInformation: err=%d ⇒ classic 'DRVR'", err);
        if (dce->dCtlFlags & dRAMBasedMask) {
            Handle hh = (Handle)dce->dCtlDriver;
            if (hh != NULL && *hh != NULL) h = (DRVRHeaderPtr)(*hh);
        } else {
            h = (DRVRHeaderPtr)dce->dCtlDriver;
        }
        if (h != NULL) {
            CDLogf("    DRVR hdr: flags=0x%04X delay=%d emask=0x%04X menu=%d",
                   (unsigned short)h->drvrFlags, h->drvrDelay,
                   (unsigned short)h->drvrEMask, h->drvrMenu);
            CDLogf("    DRVR entry offsets: open=%d prime=%d ctl=%d status=%d "
                   "close=%d  <-- ctl/status are the patch targets",
                   h->drvrOpen, h->drvrPrime, h->drvrCtl,
                   h->drvrStatus, h->drvrClose);
            if (h->drvrName[0] > 0 && h->drvrName[0] < 64) {
                BlockMoveData(h->drvrName, info->name, h->drvrName[0] + 1);
                CDPToC(h->drvrName, cname, sizeof(cname));
                CDLogf("    DRVR name='%s'", cname);
            }
            CDLogHex("DRVRhdr", h, 32);
        }
    }

    /* Whatever it is, put the shape of the code at dCtlDriver on record, in case
     * both classifications surprise us. */
    if (dce->dCtlDriver != NULL && !(dce->dCtlFlags & dRAMBasedMask))
        CDLogHex("dCtlDriver", dce->dCtlDriver, 32);
}

void CDFindDriveNumber(CDDriverInfo *info)
{
    DrvQElPtr q = (DrvQElPtr)LM_DrvQHdr->qHead;

    CDLogf("--- drive queue ---");
    while (q != NULL) {
        CDLogf("  drive=%d refNum=%d fsID=%d size=%u/%u%s",
               q->dQDrive, q->dQRefNum, q->dQFSID,
               q->dQDrvSz, q->dQDrvSz2,
               (q->dQRefNum == info->refNum) ? "   <-- our CD" : "");
        if (q->dQRefNum == info->refNum && info->driveNum == 0)
            info->driveNum = q->dQDrive;
        q = (DrvQElPtr)q->qLink;
    }
    if (info->driveNum == 0)
        CDLogf("  no drive queue entry for refNum=%d (empty tray?)",
               info->refNum);
}

void CDFindDriver(CDDriverInfo *info)
{
    Ptr    utable = LM_UTableBase;
    short  count  = LM_UnitEntryCount;
    short  i;

    memset(info, 0, sizeof(*info));

    CDLogf("--- unit table sweep (base=0x%08lX entries=%d) ---",
           (unsigned long)utable, count);

    if (utable == NULL || count <= 0 || count > 512) {
        CDLogf("  unit table looks implausible; aborting sweep");
        return;
    }

    for (i = 0; i < count; i++) {
        DCtlHandle dceH = ((DCtlHandle *)utable)[i];
        short      refNum;
        UInt32     devt = 0, intf = 0;
        OSErr      errT, errI;

        if (dceH == NULL) continue;
        refNum = (short)~i;             /* refNum = ~unitNumber */

        errT = CDDriverGestalt(refNum, kdgDeviceType, &devt);
        if (errT != noErr) continue;    /* no 'devt' ⇒ not a disk-family driver */

        errI = CDDriverGestalt(refNum, kdgInterface, &intf);
        CDLogf("  unit=%-3d refNum=%-5d devt='%.4s' intf='%.4s'%s",
               i, refNum, (char *)&devt,
               (errI == noErr) ? (char *)&intf : "????",
               (devt == kdgCDType) ? "   <-- CD-ROM" : "");

        if (devt == kdgCDType && !info->found) {
            info->found         = true;
            info->refNum        = refNum;
            info->deviceType    = devt;
            info->interfaceType = (errI == noErr) ? intf : 0;
        }
    }

    if (!info->found) {
        CDLogf("  no driver reported devt=='cdrm'.");
        return;
    }

    CDLogf("--- chosen CD driver refNum=%d ---", info->refNum);
    CDDumpDCE(info);

    /* Selectors that matter for the later design: 'dvrf' is the
     * interface-specific device reference route-B DAE (ATA Manager) would need,
     * 'nmrg' the Name Registry entry for the device. */
    {
        UInt32 v = 0;
        if (CDDriverGestalt(info->refNum, kdgDeviceReference, &v) == noErr) {
            info->deviceRef = v;
            CDLogf("  'dvrf' device reference = 0x%08lX (route-B ATA handle)",
                   (unsigned long)v);
        }
        if (CDDriverGestalt(info->refNum, kdgNameRegistryEntry, &v) == noErr)
            CDLogf("  'nmrg' name registry entry ptr = 0x%08lX",
                   (unsigned long)v);
        if (CDDriverGestalt(info->refNum, kdgSync, &v) == noErr)
            CDLogf("  'sync' synchronous-only = %lu", (unsigned long)v);
        if (CDDriverGestalt(info->refNum, kdgVersion, &v) == noErr)
            CDLogf("  'vers' driver version = 0x%08lX", (unsigned long)v);
    }

    CDFindDriveNumber(info);
}

/* ---- TOC ----------------------------------------------------------------- */

long CDMSFToLBA(int m, int s, int f)
{
    return ((long)m * 60 + s) * kCDDASectorsPerSec + f - kCDDALeadInSectors;
}

OSErr CDReadTOCAction(short refNum, short action, Boolean asControl,
                      void *out, long outLen, const char **encUsed)
{
    short param[11];
    OSErr err;

    /* encoding 1: the action as a word at csParam offset 0 */
    memset(param, 0, sizeof(param));
    param[0] = action;
    err = asControl
        ? CDControlCall(refNum, kcsReadTOC, param, sizeof(param), out, outLen)
        : CDStatusCall(refNum, kcsReadTOC, out, outLen);
    if (err == noErr) { if (encUsed) *encUsed = "word"; return noErr; }

    /* A Status call takes no input, so retrying the encoding only means
     * something for the Control form, where csParam is an input. */
    if (!asControl) { if (encUsed) *encUsed = "n/a"; return err; }

    /* encoding 2: the action as a byte at csParam offset 0 */
    memset(param, 0, sizeof(param));
    param[0] = (short)(action << 8);
    err = CDControlCall(refNum, kcsReadTOC, param, sizeof(param), out, outLen);
    if (encUsed) *encUsed = (err == noErr) ? "byte" : "neither";
    return err;
}

void CDReadTOC(short refNum, CDTOC *toc)
{
    short          buf[11];
    unsigned char *p = (unsigned char *)buf;
    const char    *enc = "?";
    OSErr          err;

    memset(toc, 0, sizeof(*toc));

    CDLogf("--- TOC ---");

    err = CDReadTOCAction(refNum, kTOCActionFirstLast, false,
                          buf, sizeof(buf), &enc);
    if (err != noErr)
        err = CDReadTOCAction(refNum, kTOCActionFirstLast, true,
                              buf, sizeof(buf), &enc);
    if (err != noErr) {
        CDLogf("  ReadTOC first/last FAILED err=%d (no disc, or the driver does "
               "not implement ReadTOC)", err);
        return;
    }
    toc->valid      = true;
    toc->firstTrack = kBCDToBin(p[0]);
    toc->lastTrack  = kBCDToBin(p[1]);
    CDLogf("  ReadTOC first/last: first=%d last=%d (BCD %02X %02X) enc=%s",
           toc->firstTrack, toc->lastTrack, p[0], p[1], enc);

    err = CDReadTOCAction(refNum, kTOCActionLeadOut, false,
                          buf, sizeof(buf), &enc);
    if (err != noErr)
        err = CDReadTOCAction(refNum, kTOCActionLeadOut, true,
                              buf, sizeof(buf), &enc);
    if (err == noErr)
        CDLogf("  ReadTOC lead-out: %02d:%02d:%02d enc=%s",
               kBCDToBin(p[0]), kBCDToBin(p[1]), kBCDToBin(p[2]), enc);

    /* Per-track addresses. csParam+2 is a caller-supplied buffer address, +6 its
     * size, +8 the starting track in BCD. Each entry is 4 bytes: a control/adr
     * byte then M, S, F in BCD. */
    {
        int            n = toc->lastTrack - toc->firstTrack + 1;
        unsigned char *tocBuf;
        short          param[11];
        OSErr          terr;

        if (n < 1)  n = 1;
        if (n > 99) n = 99;

        tocBuf = (unsigned char *)NewPtrClear(4 * n);
        if (tocBuf == NULL) { CDLogf("  (out of memory for TOC buffer)"); return; }

        memset(param, 0, sizeof(param));
        param[0] = kTOCActionTrackAddrs;
        *(Ptr *)&param[1]  = (Ptr)tocBuf;
        *(long *)&param[3] = (long)(4 * n);
        ((unsigned char *)param)[8] = kBinToBCD(toc->firstTrack);

        CDLogStep("ReadTOC track-addresses (n=%d)", n);
        terr = CDControlCall(refNum, kcsReadTOC, param, sizeof(param), NULL, 0);
        if (terr != noErr) {
            memset(param, 0, sizeof(param));
            param[0] = (short)(kTOCActionTrackAddrs << 8);
            *(Ptr *)&param[1]  = (Ptr)tocBuf;
            *(long *)&param[3] = (long)(4 * n);
            ((unsigned char *)param)[8] = kBinToBCD(toc->firstTrack);
            terr = CDControlCall(refNum, kcsReadTOC, param, sizeof(param),
                                 NULL, 0);
        }
        CDLogf("  ReadTOC track-addresses err=%d", terr);

        if (terr == noErr) {
            int t;
            CDLogHex("toc", tocBuf, 4 * n);
            for (t = 0; t < n; t++) {
                unsigned char *e = tocBuf + 4 * t;
                int  ctrl = (e[0] >> 4) & 0x0F;
                /* control field bit 2 set ⇒ data track, clear ⇒ audio */
                Boolean isData = (ctrl & 0x04) != 0;

                toc->track[t].number = toc->firstTrack + t;
                toc->track[t].ctrl   = ctrl;
                toc->track[t].isData = isData;
                toc->track[t].m      = kBCDToBin(e[1]);
                toc->track[t].s      = kBCDToBin(e[2]);
                toc->track[t].f      = kBCDToBin(e[3]);
                toc->track[t].lba    = CDMSFToLBA(toc->track[t].m,
                                                  toc->track[t].s,
                                                  toc->track[t].f);
                toc->trackCount++;
                if (!isData) toc->audioCount++;

                CDLogf("    track %2d: ctrl=0x%X %s  %02d:%02d:%02d  lba=%ld",
                       toc->track[t].number, ctrl, isData ? "DATA " : "AUDIO",
                       toc->track[t].m, toc->track[t].s, toc->track[t].f,
                       toc->track[t].lba);
            }
            CDLogf("  ⇒ %d track(s), %d audio", toc->trackCount,
                   toc->audioCount);
        }
        DisposePtr((Ptr)tocBuf);
    }
}

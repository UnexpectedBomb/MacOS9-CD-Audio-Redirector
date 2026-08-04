/*
 * cd_probe_common.h — shared plumbing for the CD Audio Redirector probes.
 *
 * Everything in here is deliberately boring: a flushed append-only log, thin
 * wrappers over PBControl/PBStatus that log both the call and its result, and the
 * driver discovery that every probe needs. The interesting per-probe logic lives
 * in the probe.
 *
 * Design constraints worth remembering:
 *   - No stdio. printf-based Retro68 apps quit immediately on real OS 9.
 *   - Bounded vsnprintf only. An unbounded vsprintf into a fixed buffer is how
 *     the USB2 work smashed its stack and got a MacsBug PC full of ASCII.
 *   - The log is FLUSHED before each driver call, so if a call hangs the machine
 *     the last line on disc names the call that did it. There is no debugger.
 */

#ifndef CD_PROBE_COMMON_H
#define CD_PROBE_COMMON_H

#include <MacTypes.h>
#include <Devices.h>
#include <Files.h>

/* ---- low-memory globals -------------------------------------------------- *
 * By hand as absolute addresses rather than via LowMem.h: the accessors there
 * come in 68K-inline and PPC-macro flavours depending on which conditional block
 * is active, and a link-time surprise on the OS 9 machine costs a whole cycle.
 * These three addresses are stable across every 68K/PPC Mac OS. */
#define LM_UTableBase        (*(Ptr *)0x011C)     /* unit table base           */
#define LM_UnitEntryCount    (*(short *)0x01D2)   /* unit table entry count    */
#define LM_DrvQHdr           ((QHdrPtr)0x0308)    /* drive queue header        */

/* Option key, for "skip the invasive part" escape hatches. */
#define kOptionKeyCode 0x3A
#define KeyIsDown(km, code) \
    ((((unsigned char *)(km))[(code) >> 3] & (1 << ((code) & 7))) != 0)

/* ---- what discovery found ------------------------------------------------- */

typedef struct {
    Boolean  found;
    short    refNum;        /* driver reference number                        */
    short    driveNum;      /* drive queue number, 0 if the tray is empty     */
    Boolean  isNative;      /* native 'ndrv' vs classic 'DRVR'                */
    Str255   name;
    OSType   deviceType;    /* DriverGestalt 'devt' — 'cdrm' for a CD          */
    OSType   interfaceType; /* DriverGestalt 'intf' — 'ata ', 'scsi', 'usb '   */
    UInt32   deviceRef;     /* DriverGestalt 'dvrf' — the route-B ATA handle   */
} CDDriverInfo;

/* ---- logging -------------------------------------------------------------- */

/* Open (create if needed) an append-mode text log of the given name in the
 * System Folder. Safe to call when it fails: every log call then no-ops. */
void CDLogOpen(ConstStr255Param fileName);
void CDLogClose(void);
void CDLogFlush(void);

void CDLogf(const char *fmt, ...);

/* Log a line and flush immediately. Use right before any driver call. */
void CDLogStep(const char *fmt, ...);

void CDLogHex(const char *tag, const void *p, long n);

/* Pascal → C string, bounded, for logging. */
void CDPToC(ConstStr255Param src, char *dst, int dstSize);

/* Write a standard run banner. */
void CDLogBanner(const char *probeName, const char *note);

/* ---- Device Manager wrappers ---------------------------------------------- *
 * Each logs the call before issuing it and the csParam bytes after. paramOut may
 * be NULL. Both copy at most 22 bytes (the size of CntrlParam.csParam). */

OSErr CDStatusCall(short refNum, short csCode, void *paramOut, long paramOutLen);

OSErr CDControlCall(short refNum, short csCode,
                    const void *paramIn, long paramInLen,
                    void *paramOut, long paramOutLen);

OSErr CDDriverGestalt(short refNum, OSType selector, UInt32 *response);

/* ---- discovery ------------------------------------------------------------ */

/* Walk the unit table, ask each populated entry what it is via DriverGestalt
 * 'devt', and return the one that says 'cdrm'.
 *
 * Deliberately NOT a name match on ".AppleCD": the ATAPI-era driver's name
 * varies across builds, and a name match is exactly the assumption that breaks
 * silently. Deliberately the unit table rather than the drive queue, because the
 * drive queue only lists drives and this has to work with the tray empty.
 *
 * Logs the whole sweep. Fills in *info; info->found says whether it worked. */
void CDFindDriver(CDDriverInfo *info);

/* Dump the DCE and classify classic DRVR vs native ndrv (the P2 question).
 * Sets info->isNative and info->name. */
void CDDumpDCE(CDDriverInfo *info);

/* Fill in info->driveNum from the drive queue, logging the queue. */
void CDFindDriveNumber(CDDriverInfo *info);

/* ---- TOC ----------------------------------------------------------------- */

typedef struct {
    Boolean  valid;
    short    firstTrack, lastTrack;
    short    audioCount;
    short    trackCount;            /* entries filled in below                */
    struct {
        short    number;
        Boolean  isData;
        short    ctrl;
        short    m, s, f;
        long     lba;
    } track[100];
} CDTOC;

/* ReadTOC action, trying both csParam encodings (byte-at-offset-0 vs word),
 * because nothing in the available source material settles which the driver
 * wants. Reports which one worked through *encUsed. */
OSErr CDReadTOCAction(short refNum, short action, Boolean asControl,
                      void *out, long outLen, const char **encUsed);

/* Read first/last track, lead-out, and the per-track addresses into *toc. */
void CDReadTOC(short refNum, CDTOC *toc);

/* MSF (binary, not BCD) → logical block address. */
long CDMSFToLBA(int m, int s, int f);

#endif /* CD_PROBE_COMMON_H */

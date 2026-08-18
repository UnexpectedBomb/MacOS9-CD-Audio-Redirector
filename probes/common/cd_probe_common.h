/*
 * cd_probe_common.h — shared plumbing for the CD Audio Redirector probes.
 *
 * Everything in here is deliberately boring: a flushed append-only log, an
 * on-screen progress window, thin wrappers over PBControl/PBStatus that log both
 * the call and its result, and the driver discovery every probe needs. The
 * interesting per-probe logic lives in the probe.
 *
 * DESIGN CONSTRAINTS, EACH ONE PAID FOR
 * -------------------------------------
 *   - No stdio. printf-based Retro68 apps quit immediately on real OS 9.
 *   - Bounded vsnprintf only. An unbounded vsprintf into a fixed buffer is how
 *     the USB2 work smashed its stack and got a MacsBug PC full of ASCII.
 *   - EVERY log line is flushed. v1 only flushed inside CDLogStep, so a
 *     force-quit could lose exactly the lines that named the problem.
 *   - EVERY driver call is announced on screen before it is issued. v1 hung on
 *     the first run with no visible progress and no surviving breadcrumb, which
 *     is the failure mode the log was supposed to make impossible. A sync PB call
 *     to a driver that never completes it spins forever, so "which call" is the
 *     only question that matters, and it must be answerable without a debugger.
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

/* Modifier keys, for the escape hatches. */
#define kOptionKeyCode 0x3A
#define kShiftKeyCode  0x38
#define KeyIsDown(km, code) \
    ((((unsigned char *)(km))[(code) >> 3] & (1 << ((code) & 7))) != 0)

/* ★ printf format checking on all three of these.
 *
 * Without it the compiler will not look inside a format string, and a "%ld" with no
 * argument compiles silently and prints a garbage address at runtime. That happened
 * while writing the end-of-track phase, in a diagnostic whose entire job is to be
 * trustworthy. GCC checks these for free once told what they are, so the whole class
 * is now a build error rather than a thing to be careful about. */
#define CD_PRINTFLIKE(fmtArg, firstVararg) \
    __attribute__((format(printf, (fmtArg), (firstVararg))))

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
    Boolean  viaDriveQueue; /* found by the safe path rather than the sweep    */
} CDDriverInfo;

/* ---- progress window ----------------------------------------------------- *
 * Opened before any driver call so that "hung" and "slow" are distinguishable
 * from across the room, and so the last line on screen names the call in flight
 * even if the log never reaches disc. */

void CDProgressOpen(ConstStr255Param title);
void CDProgressSay(const char *fmt, ...) CD_PRINTFLIKE(1, 2);
void CDProgressClose(void);

/* ---- logging -------------------------------------------------------------- */

/* Open (create if needed) an append-mode text log of the given name in the System
 * Folder. Returns false if it could not be opened — worth surfacing, because a
 * silent logging failure on a volume whose System Folder is not writable turns
 * every later probe into a run with no evidence. */
/* ★ The slowest single log write+flush seen, in ticks, and how many crossed half a
 * second. The log write goes to the STARTUP disc, not the CD, which makes it the one
 * suspect that explains two applications stalling in lockstep. See cd_engine.h's
 * kStallSiteLogWrite. */
void CDLogWriteStats(long *worstTicks, long *slowWrites);

Boolean CDLogOpen(ConstStr255Param fileName);
void    CDLogClose(void);
void    CDLogFlush(void);

/* Writes one line and flushes it. */
void CDLogf(const char *fmt, ...) CD_PRINTFLIKE(1, 2);

/* ★ Silence the log and the progress window for the duration of a call.
 *
 * Added for the pump, which re-reads the TOC on every play request so that a disc
 * inserted after launch is seen. At full verbosity that would append ~40 lines and
 * a matching number of flushed FSWrites to the start of every piece of music — on
 * the play path, where the whole measured budget is 1.5 seconds.
 *
 * NESTS via a counter, so an inner quiet region does not un-mute the outer one.
 * The caller is responsible for logging a one-line summary of what happened inside:
 * a silent failure leaving no trace at all is exactly the bug class that has bitten
 * this project twice (the re-opened CDLogOpen, the discarded SetGestaltValue). */
void CDLogSetQuiet(Boolean quiet);

/* ★ Report on the logger itself.
 *
 * Added because the v3 run lost every pump log line after "PUMP RUNNING" while the
 * pump demonstrably kept playing — and there was no way to tell whether the writes
 * were being suppressed by a stuck quiet region, or failing in FSWrite, or never
 * attempted. A logger that can fail silently has to be able to report on itself
 * through some other channel.
 *
 * quietDepth: current nesting depth; anything > 0 means CDLogf is discarding.
 * lastErr:    the most recent FSWrite result, which CDLogf used to throw away.
 * writes:     count of log lines actually handed to FSWrite. */
void CDLogDiag(short *quietDepth, short *lastErr, long *writes);

/* Writes one line, flushes it, AND puts it on screen. Use immediately before
 * every driver call. */
void CDLogStep(const char *fmt, ...) CD_PRINTFLIKE(1, 2);

/* Hex dump. Offsets are printed in HEX (a decimal offset in a hex dump is just
 * cruel) and relative to baseOff, so a caller feeding the dump in chunks still
 * gets true offsets — CDLogHex restarts at 0 every call, which made a 1.5 KB
 * chunked driver dump print "+0000" on all 96 lines. */
void CDLogHex(const char *tag, const void *p, long n);
void CDLogHexAt(const char *tag, const void *p, long n, long baseOff);

/* Pascal → C string, bounded, for logging. */
void CDPToC(ConstStr255Param src, char *dst, int dstSize);

/* Write a standard run banner. */
void CDLogBanner(const char *probeName, const char *note);

/* ---- Device Manager wrappers ---------------------------------------------- *
 * Each announces the call before issuing it and logs the csParam bytes after.
 * paramOut may be NULL. Both copy at most 22 bytes (the size of
 * CntrlParam.csParam). */

OSErr CDStatusCall(short refNum, short csCode, void *paramOut, long paramOutLen);

OSErr CDControlCall(short refNum, short csCode,
                    const void *paramIn, long paramInLen,
                    void *paramOut, long paramOutLen);

OSErr CDDriverGestalt(short refNum, OSType selector, UInt32 *response);

/* ---- discovery ------------------------------------------------------------ */

/* Find the optical driver and classify it.
 *
 * Two-stage on purpose. Stage 1 asks only the drivers listed in the DRIVE QUEUE,
 * which are by definition block drivers and are well behaved. Stage 2, the full
 * unit-table sweep, only runs if stage 1 found no CD and allowFullSweep is true —
 * because a sync Status call to an arbitrary driver that defers and never
 * completes the call hangs the machine, and the unit table is full of drivers
 * that have nothing to do with discs. v1 swept unconditionally and hung.
 *
 * Pass allowFullSweep = false (shift held) for the safe path only. */
void CDFindDriver(CDDriverInfo *info, Boolean allowFullSweep);

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

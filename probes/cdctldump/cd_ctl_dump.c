/*
 * CDCtlDump — Phase 2 pre-work: dump .AppleCD's Control entry so the patch can be
 * designed against what the driver actually does.
 *
 * WHY THIS EXISTS, AND WHY IT IS AN APP AND NOT THE INIT
 * -----------------------------------------------------
 * Phase 2 patches the CD driver's classic DRVR Control entry. Everything about
 * that patch depends on facts that have so far only been assumed:
 *
 *   1. Is the real Control routine actually at dCtlDriver + 0x0154, or is 0x0154 a
 *      small dispatch stub that jumps somewhere else? The five entry offsets in the
 *      header (0x114, 0x134, 0x154, 0x174, 0x194) are evenly spaced 0x20 apart,
 *      which is the signature of a jump table, not of five routines.
 *
 *   2. HOW DOES IT RETURN? A classic DRVR either RTSes with D0 = result, or jumps
 *      to jIODone. That single fact decides the entire patch shape. If it RTSes we
 *      can JSR to it from our own handler, inspect and rewrite the returned
 *      csParam, and return normally — which is what synthesising AudioStatus and
 *      ReadQ requires. If it jumps to jIODone, calling it never comes back to us
 *      and the patch has to be built the other way round: side effects first, then
 *      an unconditional tail jump, with no chance to post-process the results.
 *
 *   3. Does it dispatch on csCode with a table or a compare chain, and is there any
 *      register or DCE state it depends on that a patch must preserve?
 *
 * Getting any of these wrong in boot code means a machine that hangs at startup
 * with no debugger and no breadcrumb. The project's own rule covers this exactly:
 * two speculative ROMs were shipped on the USB2 work and both theories were wrong,
 * while a probe answered the question in one run. So this is a probe: an ordinary
 * app, one double-click, zero boot risk, and it produces a hex dump to disassemble
 * offline before a single line of the INIT is written.
 *
 * WHAT IT DOES
 * ------------
 * Read-only throughout. No Control calls, no block-size changes, no playback. It
 * locates the driver via the drive queue (never the unit-table sweep, which hung
 * v1 of the Phase-0 probes), decodes the DCE flags, then dumps the driver's code:
 *
 *   - the DRVR header and all five entry stubs
 *   - a generous window from the driver's base
 *   - the Control entry specifically, labelled
 *   - and, if the Control entry begins with a jump, it FOLLOWS that jump one level
 *     and dumps the target too, so a jump table costs no extra hardware run
 *
 * Reading past the end of the driver's allocation would bus-error, so the dump
 * length comes from GetPtrSize when that is trustworthy and from a conservative
 * fixed window when it is not. Progress is logged every 256 bytes, so if a read
 * does fault, the log names the offset that did it.
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

#define kVersionString  "CDCtlDump v1"

/* How much of the driver to dump when GetPtrSize will not tell us. The header plus
 * the five stubs end at 0x1B4, so 0x600 covers all of that and the start of
 * whatever the stubs point at, while staying well inside any plausible CD driver. */
#define kConservativeDumpBytes  0x600
#define kMaxDumpBytes           0x2000
#define kFollowDumpBytes        0x300

static CDDriverInfo gCD;

/* ---- flag decoding ------------------------------------------------------- */

static void LogDCEFlags(unsigned short flags)
{
    CDLogf("  dCtlFlags = 0x%04X:", flags);
    CDLogf("    dReadEnable   %s", (flags & dReadEnableMask)   ? "SET" : "-");
    CDLogf("    dWritEnable   %s", (flags & dWritEnableMask)   ? "SET" : "-");
    CDLogf("    dCtlEnable    %s", (flags & dCtlEnableMask)    ? "SET" : "-");
    CDLogf("    dStatEnable   %s", (flags & dStatEnableMask)   ? "SET" : "-");
    CDLogf("    dNeedGoodbye  %s", (flags & dNeedGoodByeMask)  ? "SET" : "-");
    CDLogf("    dNeedTime     %s  <-- if SET the driver already receives periodic",
           (flags & dNeedTimeMask) ? "SET" : "-");
    CDLogf("                       accRun (csCode 65) Control calls at TASK level");
    CDLogf("                       from SystemTask, which is where Phase 2 could");
    CDLogf("                       refill the audio ring with no new pump.");
    CDLogf("    dNeedLock     %s", (flags & dNeedLockMask)     ? "SET" : "-");
    CDLogf("    dRAMBased     %s  (set = dCtlDriver is a Handle, clear = a Ptr)",
           (flags & dRAMBasedMask) ? "SET" : "-");
}

/* ---- code dump ----------------------------------------------------------- */

/* Dump bytes with a progress breadcrumb every 256, so a bus error names the
 * offset that caused it rather than leaving a silent dead machine. */
static void DumpCode(const char *tag, const unsigned char *base, long bytes)
{
    long off;

    CDLogf("--- code dump: %s @ 0x%08lX, %ld bytes ---",
           tag, (unsigned long)base, bytes);

    for (off = 0; off < bytes; off += 16) {
        long n = bytes - off;
        if (n > 16) n = 16;

        if ((off & 0xFF) == 0) {
            CDLogStep("%s dumping +0x%04lX", tag, off);
        }
        /* CDLogHexAt, not CDLogHex: the latter restarts its offset at 0 on every
         * call, so feeding it 16 bytes at a time printed "+0000" on all 96 lines
         * of the first driver dump and the offsets had to be reconstructed by
         * counting lines. */
        CDLogHexAt(tag, base + off, n, off);
    }
}

/* Minimal 68K decode of the handful of instructions a dispatch stub is likely to
 * start with. Enough to follow one level of indirection without a second hardware
 * run; anything more exotic gets read out of the hex by hand.
 *
 * Returns the jump target, or NULL if the first instruction is not a jump this
 * knows about. */
static const unsigned char *DecodeFirstJump(const unsigned char *p,
                                            const char **whatOut)
{
    unsigned short op = (unsigned short)((p[0] << 8) | p[1]);

    *whatOut = "not a recognised jump";

    /* ★ 0xAAFE is _MixedModeDispatch: this is not 68K code at all, it is a Mixed
     * Mode RoutineDescriptor. The v1 probe did not know this shape and reported
     * "not a recognised jump ⇒ the code appears to start here", which was wrong in
     * a way that would have mattered. All five of .AppleCD's entries turn out to
     * be exactly this, wrapping native PowerPC routines. */
    if (op == 0xAAFE) {
        unsigned char  isa    = p[13];
        unsigned long  proc   = ((unsigned long)p[16] << 24)
                              | ((unsigned long)p[17] << 16)
                              | ((unsigned long)p[18] << 8)
                              |  (unsigned long)p[19];
        unsigned long  info   = ((unsigned long)p[8]  << 24)
                              | ((unsigned long)p[9]  << 16)
                              | ((unsigned long)p[10] << 8)
                              |  (unsigned long)p[11];
        CDLogf("  ⇒ MIXED MODE ROUTINE DESCRIPTOR, not 68K code:");
        CDLogf("      version=0x%02X flags=0x%02X routineCount=%d",
               p[2], p[3], (int)((p[10] << 8) | p[11]));
        CDLogf("      procInfo=0x%08lX  (low nibble %lu: 2 = kRegisterBased)",
               info, info & 0x0F);
        CDLogf("      ISA=0x%02X  (0 = 68K, 1 = PowerPC)", isa);
        CDLogf("      procDescriptor (TVector) = 0x%08lX", proc);
        CDLogf("      ⇒ this entry is a CALLABLE routine that RETURNS normally,");
        CDLogf("        so a patch can chain to it and then rewrite csParam.");
        *whatOut = "Mixed Mode routine descriptor (see the decode above)";
        return NULL;   /* the target is PPC code; not 68K to follow */
    }

    /* JMP xxx.L  — 4EF9 followed by a 32-bit absolute address */
    if (op == 0x4EF9) {
        unsigned long t = ((unsigned long)p[2] << 24) | ((unsigned long)p[3] << 16)
                        | ((unsigned long)p[4] << 8)  |  (unsigned long)p[5];
        *whatOut = "JMP xxx.L (absolute long)";
        return (const unsigned char *)t;
    }
    /* JMP d16(PC) — 4EFA followed by a signed 16-bit displacement from PC+2 */
    if (op == 0x4EFA) {
        short d = (short)((p[2] << 8) | p[3]);
        *whatOut = "JMP d16(PC)";
        return p + 2 + d;
    }
    /* JSR xxx.L */
    if (op == 0x4EB9) {
        unsigned long t = ((unsigned long)p[2] << 24) | ((unsigned long)p[3] << 16)
                        | ((unsigned long)p[4] << 8)  |  (unsigned long)p[5];
        *whatOut = "JSR xxx.L (absolute long) — note: JSR, so it RETURNS here";
        return (const unsigned char *)t;
    }
    /* JSR d16(PC) */
    if (op == 0x4EBA) {
        short d = (short)((p[2] << 8) | p[3]);
        *whatOut = "JSR d16(PC) — note: JSR, so it RETURNS here";
        return p + 2 + d;
    }
    /* BRA.W — 6000 followed by a signed 16-bit displacement */
    if (op == 0x6000) {
        short d = (short)((p[2] << 8) | p[3]);
        *whatOut = "BRA.W";
        return p + 2 + d;
    }
    /* BRA.B — 60xx where xx is a signed 8-bit displacement, non-zero */
    if ((op & 0xFF00) == 0x6000 && (op & 0x00FF) != 0x00 && (op & 0x00FF) != 0xFF) {
        signed char d = (signed char)(op & 0x00FF);
        *whatOut = "BRA.B";
        return p + 2 + d;
    }

    return NULL;
}

/* A pointer we are about to dereference. Nothing here is worth a bus error. */
static Boolean Plausible(const void *p)
{
    unsigned long v = (unsigned long)p;
    return (v > 0x1000) && (v < 0x80000000UL) && ((v & 1) == 0);
}

int main(void)
{
    Boolean        logOK;
    DCtlHandle     dceH;
    DCtlPtr        dce;
    unsigned char *base;
    long           dumpBytes;
    DRVRHeaderPtr  hdr;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    memset(&gCD, 0, sizeof(gCD));

    CDProgressOpen("\p" kVersionString " - progress");
    CDProgressSay("%s starting", kVersionString);

    logOK = CDLogOpen("\pCD Ctl Dump Log");
    if (!logOK) CDProgressSay("!! could not open the log - screen only");
    CDLogBanner(kVersionString " - Phase 2 pre-work: dump the Control entry",
                "READ-ONLY. No Control calls, no block-size change, no playback.");

    /* Drive queue only. The unit-table sweep hung the Phase-0 probes on their
     * first hardware run, and there is no reason to risk it again. */
    CDFindDriver(&gCD, false);
    if (!gCD.found) {
        CDLogf("no CD driver found; nothing to dump");
        CDProgressSay("NO CD DRIVER FOUND");
        goto done;
    }

    dceH = GetDCtlEntry(gCD.refNum);
    if (dceH == NULL || *dceH == NULL) {
        CDLogf("no DCE for refNum=%d", gCD.refNum);
        goto done;
    }
    dce = *dceH;

    CDLogf("=== DCE detail for refNum=%d ===", gCD.refNum);
    LogDCEFlags((unsigned short)dce->dCtlFlags);
    CDLogf("  dCtlDelay = %d ticks%s", dce->dCtlDelay,
           (dce->dCtlFlags & dNeedTimeMask)
               ? "  <-- accRun period. Phase 2 may need this shortened while"
               : "");
    if (dce->dCtlFlags & dNeedTimeMask)
        CDLogf("                          playing, and restored on stop.");
    CDLogf("  dCtlStorage = 0x%08lX  dCtlPosition = %ld",
           (unsigned long)dce->dCtlStorage, dce->dCtlPosition);

    if (dce->dCtlFlags & dRAMBasedMask) {
        Handle hh = (Handle)dce->dCtlDriver;
        CDLogf("  dRAMBased is SET: dCtlDriver is a Handle. Phase 2's patch must "
               "account for the driver being relocatable.");
        base = (hh != NULL && *hh != NULL) ? (unsigned char *)(*hh) : NULL;
    } else {
        base = (unsigned char *)dce->dCtlDriver;
    }

    if (!Plausible(base)) {
        CDLogf("  dCtlDriver (0x%08lX) is not a plausible pointer; stopping",
               (unsigned long)base);
        goto done;
    }

    /* How much can we safely read? If dCtlDriver came from NewPtr, GetPtrSize
     * knows. If it points into the middle of something larger, it will not, and a
     * conservative window is the answer instead. */
    {
        Size  sz;
        OSErr memErr;

        CDLogStep("GetPtrSize(0x%08lX)", (unsigned long)base);
        sz     = GetPtrSize((Ptr)base);
        memErr = MemError();
        CDLogf("  GetPtrSize = %ld, MemError = %d", (long)sz, memErr);

        /* The floor used to be 0x200, which REJECTED the real answer: the block
         * turned out to be 476 bytes (a DRVR shell of header + five descriptors),
         * so the probe fell back to a 1536-byte window and read ~1060 bytes past
         * the end of the allocation. It did not fault, because the neighbouring
         * heap was mapped, but the guard existed precisely to prevent that. A few
         * hundred bytes is a perfectly plausible size for a driver shell. */
        if (memErr == noErr && sz >= 0x80 && sz <= 0x100000) {
            dumpBytes = (sz < kMaxDumpBytes) ? (long)sz : kMaxDumpBytes;
            CDLogf("  ⇒ trusting GetPtrSize; dumping %ld bytes", dumpBytes);
        } else {
            dumpBytes = kConservativeDumpBytes;
            CDLogf("  ⇒ GetPtrSize not trustworthy here; dumping a conservative "
                   "%ld bytes (header + all five entry stubs end at 0x1B4)",
                   dumpBytes);
        }
    }

    /* The header, for the record and to re-confirm the entry offsets. */
    hdr = (DRVRHeaderPtr)base;
    CDLogf("=== DRVR header ===");
    CDLogf("  flags=0x%04X delay=%d emask=0x%04X menu=%d",
           (unsigned short)hdr->drvrFlags, hdr->drvrDelay,
           (unsigned short)hdr->drvrEMask, hdr->drvrMenu);
    CDLogf("  open=0x%04X prime=0x%04X ctl=0x%04X status=0x%04X close=0x%04X",
           (unsigned short)hdr->drvrOpen, (unsigned short)hdr->drvrPrime,
           (unsigned short)hdr->drvrCtl, (unsigned short)hdr->drvrStatus,
           (unsigned short)hdr->drvrClose);
    CDLogf("  ⇒ the Control routine (or its stub) is at base + 0x%04X = 0x%08lX",
           (unsigned short)hdr->drvrCtl,
           (unsigned long)(base + hdr->drvrCtl));
    CDProgressSay("Control entry at base + 0x%04X",
                  (unsigned short)hdr->drvrCtl);

    /* The main dump. */
    DumpCode("drvr", base, dumpBytes);

    /* The Control entry, called out separately so it is unmissable in the log, and
     * then followed one level if it is a jump. */
    {
        const unsigned char *ctl = base + (unsigned short)hdr->drvrCtl;
        const char          *what;
        const unsigned char *target;

        CDLogf("=== THE CONTROL ENTRY — this is what Phase 2 patches ===");
        CDLogf("  address = 0x%08lX (base + 0x%04X)",
               (unsigned long)ctl, (unsigned short)hdr->drvrCtl);
        CDLogf("  Questions this dump has to answer:");
        CDLogf("    (a) is this the routine, or a stub that jumps elsewhere?");
        CDLogf("    (b) does it RTS with D0 = result, or JMP to jIODone?");
        CDLogf("        RTS  ⇒ our handler can JSR it, then rewrite csParam,");
        CDLogf("               which is what synthesising AudioStatus needs.");
        CDLogf("        IODone ⇒ calling it never returns to us, so the patch");
        CDLogf("               must do side effects then tail-jump, with no");
        CDLogf("               opportunity to post-process the result.");
        CDLogf("    (c) how does it dispatch on csCode: table or compare chain?");

        if (Plausible(ctl)) DumpCode("ctl", ctl, 0x80);

        target = DecodeFirstJump(ctl, &what);
        CDLogf("  first instruction: %s", what);
        if (target != NULL) {
            CDLogf("  ⇒ jump target = 0x%08lX", (unsigned long)target);
            if (Plausible(target)) {
                CDLogf("  following it one level so a jump table costs no second "
                       "hardware run:");
                CDProgressSay("following the jump to 0x%08lX",
                              (unsigned long)target);
                DumpCode("ctltarget", target, kFollowDumpBytes);
            } else {
                CDLogf("  target is not a plausible pointer; not following it");
            }
        } else {
            CDLogf("  ⇒ the Control code appears to start here rather than "
                   "jumping away. Read the 'ctl' dump above for the dispatch and "
                   "the return.");
        }
    }

    /* Status too: ReadTOC, ReadQ and AudioStatus all turned out to be Control
     * calls on this driver, so Status matters less, but the symmetry is cheap and
     * tells us whether the stubs really are a table. */
    {
        const unsigned char *st = base + (unsigned short)hdr->drvrStatus;
        CDLogf("=== the Status entry, for comparison ===");
        if (Plausible(st)) DumpCode("status", st, 0x40);
    }

    CDLogf("=== end of run ===");

done:
    CDLogClose();
    CDProgressSay("done - send 'CD Ctl Dump Log' back");
    {
        EventRecord evt;
        long        until = TickCount() + 300;   /* leave the window up ~5 s */
        while (TickCount() < until) {
            if (WaitNextEvent(mDownMask | keyDownMask, &evt, 5, NULL)) break;
        }
    }
    CDProgressClose();
    return 0;
}

/*
 * cd_patch_blob.c — Phase 2a: the RESIDENT half of the CD driver Control patch.
 *
 * This is a 68K flat code resource ('CDpt' 128) that the INIT loads into the SYSTEM
 * HEAP, detaches, locks, and calls exactly once. From then on it is resident and
 * independent of the INIT, whose own code is discarded when it returns.
 *
 * PHASE 2a DELIBERATELY DOES NO AUDIO. It installs the patch, records every Control
 * call that passes through, and hands every one of them to the original driver
 * unchanged. That proves the risky half — residency, the patch, transparent
 * passthrough — with a payload that cannot misbehave, and it delivers the game call
 * trace that PHASE0.md §P5b deferred. The engine arrives in 2b.
 *
 * ★ WHY PASSTHROUGH IS A TAIL JUMP AND NOT A CALL
 * Retro68's own libDRVRRuntime shows the canonical DRVR epilogue:
 *
 *     btst #1,%a0@(6)     ; noQueueBit in pb->ioTrap
 *     bnes  ...           ; immediate -> plain RTS
 *     movel 8fc,%sp@-     ; queued    -> push jIODone
 *     rts                 ;              and "return" into it
 *
 * So a Control routine's ending depends on whether the request was queued, and a
 * queued one must end at jIODone rather than returning. If we CALLED the original
 * and it took the queued path, it would never come back to us. Tail-jumping instead
 * — leaving the Device Manager's own return address on the stack — makes whatever
 * the original does happen exactly as it would have without us. Perfect
 * transparency, and it is safe under either ending.
 *
 * That also means 2a cannot rewrite results, which is fine because it does not want
 * to. 2b does, and the `ioTrap` recorded in every trace entry is precisely the fact
 * 2b needs: if the games' audio calls arrive with noQueueBit set they end in RTS,
 * and a call-then-rewrite handler is straightforward.
 *
 * ★ INTERRUPT SAFETY
 * Control can be issued asynchronously, so the handler may run below task level. It
 * does nothing but plain stores into a pre-allocated ring. No allocation, no File
 * Manager, no logging, no waiting (reference_os9_no_filemgr_at_interrupt).
 *
 * ★ GLOBALS
 * A Retro68 flat code resource reaches its globals through absolute addresses that
 * `Retro68Relocate` fixes up at the loaded address. We call RETRO68_RELOCATE() once,
 * at the installer entry, and never call Retro68FreeGlobals — the globals have to
 * outlive the call. After relocation the code needs no register setup, so it is
 * callable from driver and interrupt context alike.
 */

#include <MacTypes.h>
#include <Devices.h>
#include <Disks.h>
#include <Files.h>
#include <Memory.h>
#include <OSUtils.h>
#include <Events.h>
#include <DriverGestalt.h>
#include "Retro68Runtime.h"

#include "cd_cscodes.h"
#include "cd_patch_shell.h"

/* Low-memory globals, by absolute address: the accessors come in flavours that
 * differ between headers and this has to be unambiguous in boot code. */
#define LM_DrvQHdr          ((QHdrPtr)0x0308)
#define LM_UTableBase       (*(Ptr *)0x011C)
#define LM_UnitEntryCount   (*(short *)0x01D2)

#define kRingEntries    512          /* power of two, so masking picks the slot */

/* ---- resident state ------------------------------------------------------ */

static CDPatchShell *gShell     = NULL;
static Ptr           gOrigCtl   = NULL;   /* absolute address of the original's
                                           * Control routine descriptor         */
static CDTraceEntry *gRing      = NULL;

/* ---- the Control entry ---------------------------------------------------- *
 * Entered per the classic DRVR contract: A0 = ParmBlkPtr, A1 = DCtlPtr, result in
 * D0. C cannot express that, so the entry is a hand-written shim. It preserves
 * A0/A1 across the C tracer, then tail-jumps to the original with the Device
 * Manager's return address still in place.
 *
 * The stack at the `rts` is [origCtl][retaddr], so the rts pops origCtl into PC and
 * the original sees exactly the stack it would have seen if we had never existed.
 *
 * Registers: the DRVR contract permits clobbering A0-A3 and D0-D3, and the C
 * function preserves the callee-saved set itself, so nothing else needs saving. */
asm (
"       .text                          \n"
"       .globl CDCtlEntry              \n"
"CDCtlEntry:                           \n"
"       movem.l %a0-%a1,-(%sp)         \n"   /* keep pb/dce across the C call   */
"       move.l  %a1,-(%sp)             \n"   /* arg 2: dce                      */
"       move.l  %a0,-(%sp)             \n"   /* arg 1: pb                       */
"       jsr     CDTraceControl         \n"
"       addq.l  #8,%sp                 \n"
"       movem.l (%sp)+,%a0-%a1         \n"
"       move.l  gOrigCtl,-(%sp)        \n"   /* tail-jump target                */
"       rts                            \n"
);

extern void CDCtlEntry(void);

/* Called from the shim at whatever level the Control call arrived on. Stores only. */
void CDTraceControl(ParmBlkPtr pb, DCtlPtr dce)
{
    CDPatchInfo *info = &gShell->info;
    short        cs;
    long         slot;
    CDTraceEntry *e;
    int          i;

    (void)dce;

    if (gRing == NULL || pb == NULL) return;

    cs = ((CntrlParam *)pb)->csCode;

    info->callCount++;
    if (cs == kcsAudioPlay || cs == kcsAudioTrackSearch || cs == kcsAudioPause ||
        cs == kcsAudioStop || cs == kcsAudioStatus      || cs == kcsAudioScan  ||
        cs == kcsAudioControl || cs == kcsReadTheQSubcode ||
        cs == kcsReadAudioVolume)
        info->audioCallCount++;

    slot = info->writeCount & (kRingEntries - 1);
    e    = &gRing[slot];

    e->ticks  = TickCount();
    e->csCode = cs;
    e->ioTrap = (unsigned short)pb->ioParam.ioTrap;
    for (i = 0; i < 8; i++)
        e->csParam[i] = ((unsigned char *)((CntrlParam *)pb)->csParam)[i];

    /* Published last, so a reader never sees a slot count that outruns its
     * contents. */
    info->writeCount++;
}

/* ---- driver discovery ---------------------------------------------------- *
 * ★ REVISED after the first hardware boot. v1 walked the DRIVE QUEUE only and
 * asked each of its drivers, via DriverGestalt, whether it was a CD. On the real
 * machine that found nothing at INIT-parade time and the install correctly refused.
 * The drive queue lists DRIVES, and the CD drive is not necessarily enumerated that
 * early even when .AppleCD is already loaded.
 *
 * So the primary strategy is now a PASSIVE unit-table scan: read each DCE's DRVR
 * header name straight out of memory and look for ".AppleCD". No driver is called,
 * so this cannot hang the way the Phase-0 unit-table sweep did — that hang came from
 * issuing Status calls to arbitrary drivers, one of which never completed. Reading
 * bytes is not that.
 *
 * Name matching was something I argued against earlier, on the grounds that the
 * ATAPI-era driver's name varies across builds. It is the right call here anyway:
 * we measured the name on this hardware (".AppleCD", FINDINGS.md), a passive read is
 * enormously safer during the parade than calling into unknown drivers, and the one
 * candidate we find by name then gets verified with a single DriverGestalt call. */

#define kMaxUnits 256

/* Passively fetch a unit's DRVR name into a buffer. Returns false if anything about
 * the DCE or the pointer looks wrong — nothing here is worth a bus error at boot. */
static Boolean GetUnitDriverName(short refNum, unsigned char *out, short outMax)
{
    DCtlHandle     dceH;
    DCtlPtr        dce;
    unsigned char *base;
    DRVRHeaderPtr  hdr;
    short          len, i;

    dceH = GetDCtlEntry(refNum);
    if (dceH == NULL || *dceH == NULL) return false;
    dce = *dceH;

    if (dce->dCtlFlags & dRAMBasedMask) {
        Handle hh = (Handle)dce->dCtlDriver;
        if (hh == NULL || *hh == NULL) return false;
        base = (unsigned char *)(*hh);
    } else {
        base = (unsigned char *)dce->dCtlDriver;
    }
    if (base == NULL || ((unsigned long)base & 1) ||
        (unsigned long)base < 0x1000)
        return false;

    hdr = (DRVRHeaderPtr)base;
    len = hdr->drvrName[0];
    if (len < 1 || len > 31 || len >= outMax) return false;

    /* Every byte must be printable, or we are not looking at a name. */
    for (i = 1; i <= len; i++) {
        unsigned char c = hdr->drvrName[i];
        if (c < 0x20 || c > 0x7E) return false;
    }

    for (i = 0; i <= len; i++) out[i] = hdr->drvrName[i];
    return true;
}

static Boolean NameIsAppleCD(const unsigned char *p)
{
    /* Pascal ".AppleCD", case-insensitive, exact length. */
    static const char *want = ".APPLECD";
    short i;

    if (p[0] != 8) return false;
    for (i = 0; i < 8; i++) {
        unsigned char c = p[1 + i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        if (c != (unsigned char)want[i]) return false;
    }
    return true;
}

static Boolean VerifyIsCD(short refNum)
{
    DriverGestaltParam pb;
    short i;

    for (i = 0; i < (short)sizeof(pb); i++) ((char *)&pb)[i] = 0;
    pb.ioCRefNum             = refNum;
    pb.csCode                = kcsDriverGestalt;
    pb.driverGestaltSelector = kdgDeviceType;
    return (PBStatusSync((ParmBlkPtr)&pb) == noErr &&
            pb.driverGestaltResponse == kdgCDType);
}

static short FindCDRefNum(void)
{
    Ptr   utable = LM_UTableBase;
    short count  = LM_UnitEntryCount;
    short i;

    /* --- primary: passive name scan of the unit table --- */
    if (utable != NULL && count > 0 && count <= kMaxUnits) {
        for (i = 0; i < count; i++) {
            unsigned char name[36];
            short         refNum = (short)~i;

            if (((DCtlHandle *)utable)[i] == NULL) continue;
            if (!GetUnitDriverName(refNum, name, sizeof(name))) continue;
            if (!NameIsAppleCD(name)) continue;

            /* One call, to a driver we are now confident about. If it does not
             * answer 'cdrm' we still take it: the name is an exact match and the
             * shape checks in the installer are the real gate. */
            (void)VerifyIsCD(refNum);
            return refNum;
        }
    }

    /* --- fallback: the drive queue, as v1 did --- */
    {
        DrvQElPtr q = (DrvQElPtr)LM_DrvQHdr->qHead;
        short     seen[16];
        short     nSeen = 0;

        while (q != NULL && nSeen < 16) {
            short   refNum = q->dQRefNum;
            Boolean dup    = false;

            for (i = 0; i < nSeen; i++) if (seen[i] == refNum) dup = true;
            if (!dup) {
                seen[nSeen++] = refNum;
                if (VerifyIsCD(refNum)) return refNum;
            }
            q = (DrvQElPtr)q->qLink;
        }
    }

    return 0;
}

/* ---- shell construction -------------------------------------------------- */

static void PutJump(unsigned char *at, void *target)
{
    unsigned long t = (unsigned long)target;
    at[0] = 0x4E; at[1] = 0xF9;            /* JMP xxx.L */
    at[2] = (unsigned char)(t >> 24);
    at[3] = (unsigned char)(t >> 16);
    at[4] = (unsigned char)(t >> 8);
    at[5] = (unsigned char)(t);
}

/* ---- the installer: the blob's entry point, called once by the INIT -------- */

short _start(void)
{
    short          refNum;
    DCtlHandle     dceH;
    DCtlPtr        dce;
    DRVRHeaderPtr  orig;
    unsigned char *origBase;
    CDPatchShell  *shell;
    long           i;

    RETRO68_RELOCATE();
    Retro68CallConstructors();
    /* deliberately NOT Retro68FreeGlobals(): these globals must outlive us */

    refNum = FindCDRefNum();
    if (refNum == 0) return kInstallNoDriver;

    dceH = GetDCtlEntry(refNum);
    if (dceH == NULL || *dceH == NULL) return kInstallNoDCE;
    dce = *dceH;

    /* A Handle-based driver would move under us; .AppleCD is a Ptr (dRAMBased
     * clear, confirmed on hardware) so refuse rather than guess. */
    if (dce->dCtlFlags & dRAMBasedMask) return kInstallRAMBased;

    origBase = (unsigned char *)dce->dCtlDriver;
    if (origBase == NULL || ((unsigned long)origBase & 1) ||
        (unsigned long)origBase < 0x1000)
        return kInstallBadDriverPtr;

    orig = (DRVRHeaderPtr)origBase;

    /* Shape check before touching anything. All five entry offsets must be
     * non-zero, even, ordered and inside a sane range — a refused install is a
     * working machine. */
    if (orig->drvrOpen == 0 || orig->drvrPrime == 0 || orig->drvrCtl == 0 ||
        orig->drvrStatus == 0 || orig->drvrClose == 0)
        return kInstallNotDRVRShape;
    if ((orig->drvrOpen & 1) || (orig->drvrCtl & 1) || (orig->drvrClose & 1))
        return kInstallNotDRVRShape;
    if (orig->drvrCtl < 0x12 || (unsigned short)orig->drvrCtl > 0x4000)
        return kInstallNotDRVRShape;

    /* ★ Require the ATAPI driver we actually characterised, and refuse anything else.
     *
     * The first successful boot install taught this the hard way. During the extension
     * parade, unit 65's dCtlDriver pointed at 0x00FFE02E with entry offsets 0x30,
     * 0x40, 0x100, 0x106 — nothing like the ATAPI '.AppleCD' Phase 0 dumped
     * (0x114/0x134/0x154/0x174/0x194, every entry a 0xAAFE Mixed Mode descriptor
     * wrapping native PowerPC). So the INIT patched an EARLY, DIFFERENT incarnation of
     * the driver, not the one the whole design was built against.
     *
     * The signature of the right one is unmistakable: its Control entry begins with
     * 0xAAFE. Checking for that turns "did we happen to run late enough?" into a
     * decision the code makes for itself — at INIT time this fails and the install
     * cleanly declines, and post-boot it succeeds. */
    {
        unsigned char *ctl = origBase + (unsigned short)orig->drvrCtl;
        unsigned short op  = (unsigned short)((ctl[0] << 8) | ctl[1]);
        if (op != 0xAAFE) return kInstallNotATAPIDriver;
    }

    /* Already ours? Re-patching would lose the original pointer. */
    {
        CDPatchInfo *maybe = (CDPatchInfo *)(origBase + kInfoOffset);
        if (maybe->magic == kPatchMagic) return kInstallAlreadyPatched;
    }

    shell = (CDPatchShell *)NewPtrSysClear((Size)sizeof(CDPatchShell));
    if (shell == NULL) return kInstallNoMemory;

    gRing = (CDTraceEntry *)
            NewPtrSysClear((Size)(kRingEntries * sizeof(CDTraceEntry)));
    if (gRing == NULL) { DisposePtr((Ptr)shell); return kInstallNoMemory; }

    /* Header: copy the original's operating parameters verbatim so the Device
     * Manager treats our shell exactly as it treated the real one — including
     * dNeedTime, which is what keeps accRun arriving (2b's refill pump). */
    shell->drvrFlags  = orig->drvrFlags;
    shell->drvrDelay  = orig->drvrDelay;
    shell->drvrEMask  = orig->drvrEMask;
    shell->drvrMenu   = orig->drvrMenu;
    shell->drvrOpen   = kStubOpen;
    shell->drvrPrime  = kStubPrime;
    shell->drvrCtl    = kStubCtl;
    shell->drvrStatus = kStubStatus;
    shell->drvrClose  = kStubClose;

    /* ★ KEEP THE ORIGINAL NAME, byte for byte.
     * v1 gave the shell its own name, ".CDAudio". On hardware that showed up in the
     * unit table in place of ".AppleCD", and iTunes then could not recognise an audio
     * CD that it had read fine before. Anything that finds this driver by name —
     * OpenDriver, Audio CD Access, the mounting machinery — must keep finding it.
     * We are impersonating the driver, so we have to look exactly like it. Renaming
     * it was gratuitous. */
    {
        short nameLen = orig->drvrName[0];
        short k;
        if (nameLen < 1 || nameLen > (short)(sizeof(shell->name) - 1)) {
            DisposePtr((Ptr)gRing);
            DisposePtr((Ptr)shell);
            gRing = NULL;
            return kInstallNotDRVRShape;
        }
        for (k = 0; k <= nameLen; k++) shell->name[k] = orig->drvrName[k];
    }

    /* Four stubs tail-jump to the original's own entries; only Control is ours. */
    PutJump(&shell->stubs[kStubOpen   - kStubOpen], origBase + orig->drvrOpen);
    PutJump(&shell->stubs[kStubPrime  - kStubOpen], origBase + orig->drvrPrime);
    PutJump(&shell->stubs[kStubCtl    - kStubOpen], (void *)CDCtlEntry);
    PutJump(&shell->stubs[kStubStatus - kStubOpen], origBase + orig->drvrStatus);
    PutJump(&shell->stubs[kStubClose  - kStubOpen], origBase + orig->drvrClose);

    shell->info.magic       = kPatchMagic;
    shell->info.version     = kPatchVersion;
    shell->info.origDriver  = (Ptr)origBase;
    shell->info.ring        = (Ptr)gRing;
    shell->info.ringEntries = kRingEntries;
    shell->info.writeCount  = 0;
    shell->info.callCount   = 0;
    shell->info.audioCallCount = 0;

    gShell   = shell;
    gOrigCtl = (Ptr)(origBase + orig->drvrCtl);

    /* Everything is built and consistent before the DCE changes, so there is no
     * window in which a Control call could reach a half-finished shell. */
    for (i = 0; i < 4; i++) { }      /* no-op barrier for readability */
    dce->dCtlDriver = (Ptr)shell;

    return kInstallOK;
}

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
#define LM_DrvQHdr      ((QHdrPtr)0x0308)

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
 * Drive queue only, and by DriverGestalt 'devt' rather than by the name
 * ".AppleCD": the name varies across ATAPI-era builds, and the unit-table sweep
 * hung the Phase-0 probes on their first hardware run. This runs during the INIT
 * parade, where the fewer drivers we poke the better. */
static short FindCDRefNum(void)
{
    DrvQElPtr q = (DrvQElPtr)LM_DrvQHdr->qHead;
    short     seen[16];
    short     nSeen = 0;

    while (q != NULL && nSeen < 16) {
        short   refNum = q->dQRefNum;
        Boolean dup    = false;
        short   i;

        for (i = 0; i < nSeen; i++) if (seen[i] == refNum) dup = true;
        if (!dup) {
            DriverGestaltParam pb;
            seen[nSeen++] = refNum;

            for (i = 0; i < (short)sizeof(pb); i++) ((char *)&pb)[i] = 0;
            pb.ioCRefNum             = refNum;
            pb.csCode                = kcsDriverGestalt;
            pb.driverGestaltSelector = kdgDeviceType;
            if (PBStatusSync((ParmBlkPtr)&pb) == noErr &&
                pb.driverGestaltResponse == kdgCDType)
                return refNum;
        }
        q = (DrvQElPtr)q->qLink;
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
    shell->name[0]    = 8;
    shell->name[1] = '.'; shell->name[2] = 'C'; shell->name[3] = 'D';
    shell->name[4] = 'A'; shell->name[5] = 'u'; shell->name[6] = 'd';
    shell->name[7] = 'i'; shell->name[8] = 'o';

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

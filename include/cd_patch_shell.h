/*
 * cd_patch_shell.h — the shape of the DRVR shell our patch installs.
 *
 * Shared by the 68K patch blob that BUILDS it and the PowerPC app that READS it,
 * so the two can never disagree about the layout. Both compilers see the same
 * big-endian, naturally-aligned fields, and the array sizes are chosen so that no
 * implicit padding is needed anywhere: 9 shorts (18) + name[14] = 32, + stubs[32]
 * = 64, so `info` lands exactly at offset 0x40 on both sides.
 *
 * WHY A SHELL AT ALL
 * ------------------
 * A DRVR header's five entry points are 16-bit offsets FROM THE HEADER BASE, so we
 * cannot simply repoint the original driver's `drvrCtl` at our code — it is nowhere
 * near within 64 KB, and editing a shipping driver's header in place is needlessly
 * invasive. Instead we build our own header in the system heap, with its own small
 * offsets pointing at five jump stubs. Four of those tail-jump to the original's
 * absolute entries; only Control comes to us. Then we repoint the DCE's
 * `dCtlDriver` at our shell.
 *
 * The original's entries are Mixed Mode routine descriptors wrapping native
 * PowerPC (see FINDINGS.md), and a `JMP` to a descriptor works exactly as the
 * Device Manager's own call does: the 0xAAFE word traps into Mixed Mode, the PPC
 * routine runs, and it returns to whatever return address is on the stack. Since
 * we tail-jump rather than call, that return address is still the Device
 * Manager's, so passthrough is perfectly transparent.
 */

#ifndef CD_PATCH_SHELL_H
#define CD_PATCH_SHELL_H

#include <MacTypes.h>

#define kPatchMagic     FOUR_CHAR_CODE('CDAU')
#define kPatchVersion   1

/* Trace ring entry. 16 bytes, written by the Control handler with plain stores and
 * nothing else — it can be called at interrupt time, so no allocation, no File
 * Manager, no logging. */
typedef struct {
    unsigned long   ticks;      /* TickCount() when the call came through       */
    short           csCode;
    unsigned short  ioTrap;     /* bit 9 = noQueueBit: immediate vs queued.     */
                                /* This is the fact 2b needs: an immediate call */
                                /* ends in RTS, a queued one must end at        */
                                /* jIODone, and that decides whether 2b can     */
                                /* call the original and rewrite csParam.       */
    unsigned char   csParam[8]; /* first 8 bytes, enough to identify the request */
} CDTraceEntry;

/* Published at shell offset 0x40 so the dump app can find everything from the DCE
 * alone: read dCtlDriver, check the magic, follow the pointers. No Gestalt
 * selector needed, which avoids installing a 68K callback that the native Gestalt
 * Manager would have to call. */
typedef struct {
    OSType  magic;              /* kPatchMagic if this shell is ours            */
    short   version;
    short   reserved;
    Ptr     origDriver;         /* the original dCtlDriver, for uninstall       */
    Ptr     ring;               /* CDTraceEntry[ringEntries]                    */
    long    ringEntries;
    long    writeCount;         /* monotonic; & (ringEntries-1) = next slot     */
    long    callCount;          /* every Control call seen                      */
    long    audioCallCount;     /* just the audio-family csCodes                */
} CDPatchInfo;

typedef struct {
    /* --- DRVR header, 18 bytes --- */
    short           drvrFlags;
    short           drvrDelay;
    short           drvrEMask;
    short           drvrMenu;
    short           drvrOpen;       /* = 0x20 */
    short           drvrPrime;      /* = 0x26 */
    short           drvrCtl;        /* = 0x2C, the only one that comes to us */
    short           drvrStatus;     /* = 0x32 */
    short           drvrClose;      /* = 0x38 */
    unsigned char   name[14];       /* Pascal, padded so stubs start at 0x20 */

    /* --- five 6-byte JMP stubs (0x4EF9 + 32-bit absolute), 30 of 32 used --- */
    unsigned char   stubs[32];

    /* --- at exactly 0x40 --- */
    CDPatchInfo     info;
} CDPatchShell;

/* Stub offsets within the shell, matching the header fields above. */
#define kStubOpen     0x20
#define kStubPrime    0x26
#define kStubCtl      0x2C
#define kStubStatus   0x32
#define kStubClose    0x38
#define kInfoOffset   0x40

/* Status codes the blob's installer returns, so the INIT can log a reason rather
 * than a bare failure. */
enum {
    kInstallOK              = 0,
    kInstallNoDriver        = 1,   /* nothing reported devt == 'cdrm'          */
    kInstallNoDCE           = 2,
    kInstallBadDriverPtr    = 3,
    kInstallNotDRVRShape    = 4,   /* header did not look like a DRVR          */
    kInstallNoMemory        = 5,
    kInstallAlreadyPatched  = 6,   /* our magic is already there               */
    kInstallRAMBased        = 7,   /* dCtlDriver is a Handle; not handled yet  */
    kInstallNotATAPIDriver  = 8    /* the Control entry is not a 0xAAFE Mixed  */
                                   /* Mode descriptor, so this is NOT the      */
                                   /* ATAPI .AppleCD the design targets — seen */
                                   /* during the extension parade, where an    */
                                   /* earlier incarnation of the driver is in  */
                                   /* place. Refuse and let a later install    */
                                   /* catch the real one.                      */
};

#endif /* CD_PATCH_SHELL_H */

/*
 * cd_engine.h — the contract between the resident PowerPC engine and its installer.
 *
 * Shared by the PEF that fills this in and the app that prints it, so the two cannot
 * disagree. Both are PowerPC, so layout is trivially identical.
 *
 * WHY THE ENGINE IS POWERPC AND WHY THE PATCH IS ONE FIELD
 * -------------------------------------------------------
 * The 68K approach was abandoned on evidence, not taste. A 68K DRVR shell plus a
 * tail jump worked on the classic early driver (126 calls traced, machine stable) and
 * crashed the machine on all three attempts against the real ATAPI `.AppleCD`. The
 * reason is visible in the shape of the thing: unpatched, the Device Manager JSRs the
 * Control entry, which IS a Mixed Mode routine descriptor, and Mixed Mode performs
 * exactly one 68K→PPC transition. The 68K shell inserted a second boundary — DM JSRs
 * our stub, our 68K shim runs, a 68K C function is called, then an RTS into the
 * descriptor — inside a driver entry with a register-based calling convention.
 *
 * This design inserts NO new boundary. We change one field: the descriptor's
 * `procDescriptor`, at descriptor + 0x10, which holds the PowerPC TVector. The Device
 * Manager still JSRs the same descriptor, Mixed Mode still performs the same single
 * transition with the same `procInfo` and the same `ISA = kPowerPCISA`, and it lands
 * in our routine instead of Apple's. The DRVR header, the driver's name, its address
 * and `dCtlDriver` are all untouched, so the failure that broke iTunes — our shell
 * impersonating the driver — cannot recur.
 *
 * In CFM a function pointer IS the address of a TVector, so `(Ptr)&CDEngineControl`
 * is exactly the value to write, and chaining to the saved original is an ordinary
 * indirect call that returns normally. Being reached through our own TVector also
 * means r2/TOC is correct on entry, so globals work even at interrupt time — which
 * the 68K flat-code-resource route had to work around.
 *
 * ★ STEP 2 DOES NOT PATCH ANYTHING. It proves residency and validation in isolation:
 * the fragment is prepared, its memory is held, `kInitialize` finds and validates the
 * descriptor, saves the original TVector and reports everything here. Writing the
 * field is Step 3 and is a one-line change.
 */

#ifndef CD_ENGINE_H
#define CD_ENGINE_H

#include <MacTypes.h>

#define kEngineMagic    FOUR_CHAR_CODE('CDE1')
#define kEngineVersion  1

/* Offsets within a Mixed Mode routine descriptor, confirmed by dumping .AppleCD's
 * five entries on hardware (FINDINGS.md, Phase 2 pre-work):
 *   +0x00  0xAAFE          _MixedModeDispatch magic
 *   +0x02  version (0x07), flags
 *   +0x08  procInfo        0x00179822 = kRegisterBased, word result in D0
 *   +0x0D  ISA             0x01 = kPowerPCISA
 *   +0x10  procDescriptor  the TVector — THE ONE FIELD WE CHANGE
 */
#define kRDMagicOffset      0x00
#define kRDProcInfoOffset   0x08
#define kRDISAOffset        0x0D
#define kRDTVectorOffset    0x10
#define kRDMagic            0xAAFE
#define kRDISAPowerPC       0x01

/* Status from kInitialize. Distinct from the old 68K installer's codes so no log line
 * can be confused between the two generations. */
enum {
    kEngineOK               = 0,
    kEngineNoDriver         = 1,   /* no unit named .AppleCD                    */
    kEngineNoDCE            = 2,
    kEngineRAMBased         = 3,   /* dCtlDriver is a Handle; refuse            */
    kEngineBadDriverPtr     = 4,
    kEngineNotDRVRShape     = 5,
    kEngineNotDescriptor    = 6,   /* Control entry is not 0xAAFE               */
    kEngineNotPowerPCISA    = 7,   /* descriptor is not kPowerPCISA             */
    kEngineBadTVector       = 8,   /* saved TVector implausible                 */
    kEngineNoMemory         = 9,
    kEngineAlreadyPatched   = 10
};

/* Filled in by kInitialize, printed by the installer. Everything the Step-3 patch
 * will need is captured here first, so the decision to write is made against
 * recorded fact rather than an assumption. */
typedef struct {
    OSType   magic;
    short    version;
    short    status;            /* one of the kEngine* codes                    */

    short    cdRefNum;
    short    reserved0;
    Ptr      dCtlDriver;        /* the driver base, untouched                   */
    Ptr      ctlDescriptor;     /* dCtlDriver + drvrCtl                         */

    UInt32   procInfo;          /* from the live descriptor                     */
    UInt8    isa;
    UInt8    rdVersion;
    UInt16   reserved1;

    Ptr      origTVector;       /* what we would replace, saved                 */
    UInt32   origCode;          /* the TVector's two words, for sanity          */
    UInt32   origTOC;

    Ptr      ourTVector;        /* &CDEngineControl                             */
    UInt32   ourCode;
    UInt32   ourTOC;

    Ptr      ring;              /* trace ring, allocated but unused in Step 2   */
    long     ringEntries;

    short    patched;           /* 0 in Step 2, always                          */
    short    reserved2;
} CDEngineInfo;

/* Trace ring entry, same shape the 68K generation used — the field that mattered was
 * ioTrap, whose bit 9 (noQueueBit) told us AudioStatus and ReadQ arrive QUEUED. */
typedef struct {
    unsigned long   ticks;
    short           csCode;
    unsigned short  ioTrap;
    unsigned char   csParam[8];
} CDEngineTrace;

#define kEngineRingEntries  512

/* The command code we invoke DoDriverIO with. We call it ourselves rather than
 * letting the Device Manager do it — the fragment is never installed into the unit
 * table — so `contents` carries a CDEngineInfo* instead of a DriverInitInfo*. */
#define kEngineInitCommand      7    /* kInitializeCommand */
#define kEngineFinalizeCommand  8    /* kFinalizeCommand   */

#endif /* CD_ENGINE_H */

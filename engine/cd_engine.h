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
 * `procDescriptor`, at descriptor + 0x14, which holds the PowerPC TVector. The Device
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
/* 1 = single-slot request mailbox. 2 = the 16-entry request ring, which moved every
 * field after it in CDEnginePublic. 3 = pump liveness fields appended (pumpBeat and
 * the logger diagnostics). Readers must check before trusting the layout. */
#define kEngineVersion  3

/* ★ DO NOT HAND-COUNT OFFSETS INTO A ROUTINE DESCRIPTOR. Use MixedMode.h's real
 * `RoutineDescriptor` / `RoutineRecord` structs.
 *
 * Step 2's first hardware run failed on exactly this: I transcribed the offsets as
 * procInfo +0x08, ISA +0x0D, procDescriptor +0x10, when the true layout is +0x0C,
 * +0x11 and +0x14 — every one off by four. The run read ISA = 0x17, which is the
 * second byte of procInfo (0x00_17_9822), and procInfo = 0 from the reserved and
 * routineCount bytes. The validation guard caught it and refused, which is the only
 * reason nothing was corrupted: writing a TVector at +0x10 would have landed across
 * reserved1/ISA/routineFlags.
 *
 * The real layout, from MixedMode.h:198 and :177, is 32 bytes — which is why
 * .AppleCD's five entries sit exactly 0x20 apart:
 *   +0x00 goMixedModeTrap (0xAAFE)   +0x0C procInfo
 *   +0x02 version                    +0x10 reserved1
 *   +0x03 flags                      +0x11 ISA
 *   +0x04 reserved1                  +0x12 routineFlags
 *   +0x08 reserved2                  +0x14 procDescriptor  <- the one field we change
 *   +0x09 selectorInfo               +0x18 reserved2
 *   +0x0A routineCount               +0x1C selector
 */
#define kRDMagic            0xAAFE
#define kRDISAPowerPC       kPowerPCISA

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
    kEngineAlreadyPatched   = 10,
    kEngineCodeInAppHeap    = 11    /* our own code section is inside the        */
                                    /* application heap, so it would vanish when */
                                    /* the installer quits. Caught on hardware:  */
                                    /* CFM uses a PEF's code section IN PLACE,   */
                                    /* so the PEF must be copied to the system   */
                                    /* heap BEFORE GetDriverMemoryFragment.      */
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

    /* ★ Publication diagnostics. The first attempt wrote
     *   (void)SetGestaltValue(...)
     * and threw the error away, so when the reader could not find the selector there
     * was nothing to say why. Never discard a return value from a call that can fail
     * silently — every result gets recorded here now, and all three registration
     * entry points are tried in turn because which one OS 9 accepts for a brand-new
     * value selector is not something to guess at. */
    Ptr      pubBlock;
    OSErr    gestaltNewErr;
    OSErr    gestaltReplaceErr;
    OSErr    gestaltSetErr;
    short    gestaltPublished;  /* 1 if any of the three worked                 */

    /* Step 5a */
    short    driveNum;          /* for driver-level CD-DA reads                 */
    OSErr    audioInitErr;      /* ring/buffers/channel/TOC setup               */
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

/* ---- how a separate app finds the resident engine -------------------------- *
 * The engine's globals live in its own CFM data section, which no other process can
 * locate. So it allocates ONE system-heap block of this shape and publishes the
 * address through a value-based Gestalt selector — `SetGestaltValue`, which is
 * create-or-replace, so repeated installs are idempotent.
 *
 * Value-based deliberately: `NewGestalt` takes a selector FUNCTION, which would mean
 * handing the native Gestalt Manager a callback to invoke. A plain long needs no UPP
 * and no callback, so there is nothing for it to get wrong.
 *
 * The counters live HERE rather than in the engine's statics so a reader sees live
 * values, and the handler updates them with plain stores — it may be at interrupt
 * time. */
#define kEnginePublicSelector   FOUR_CHAR_CODE('CDau')

/* Fallback publication, for when the Gestalt route does not take. The installer writes
 * this tiny file into the System Folder and the reader falls back to it: 4 bytes of
 * magic then the 4-byte address of the CDEnginePublic block. Crude on purpose — no OS
 * mechanism to misunderstand. A stale file after a reboot is harmless because the
 * reader validates the block's own magic before trusting it, and on OS 9 all RAM is
 * readable so a wrong address cannot fault. */
#define kEngineStateFileName    "\pCD Engine State"

/* One posted request. 16 bytes of csParam is what every audio csCode this engine
 * services fits in — the same 16 the single-slot mailbox carried. */
typedef struct {
    volatile short         csCode;
    volatile short         reserved;
    volatile unsigned char param[16];
} CDEngineRequest;

/* Power of two, so the index masks. 16 entries against a pump that polls every tick is
 * enormous headroom: the deepest backlog ever observed is 3. It is sized for the case
 * that has never happened rather than the one that has, because the cost is 320 bytes
 * and the alternative failure is silent. */
/* ★ Overridable, because the SIZE of this block is now the prime suspect.
 *
 * bisectA settled it: v1's BEHAVIOUR on v4's MEMORY still froze the machine, at poll 7,
 * the earliest yet. Reverting the drain loop and the variable-offset write changed
 * nothing. What did not change was the block: 456 bytes instead of v1's ~148, and every
 * byte of that difference is this ring.
 *
 * So the ring's ENTRY COUNT is the dial. Each entry costs 20 bytes:
 *
 *   16 entries -> 456 bytes   froze, 4 runs out of 4 (v2, v3, v4, bisectA)
 *    2 entries -> 176 bytes   the candidate: closest to v1 while still fixing the
 *                             realistic Stop-then-Play drop
 *    1 entry   -> 156 bytes   effectively v1's footprint, for finding the threshold
 */
#ifndef kEngineReqRingEntries
#define kEngineReqRingEntries   16
#endif

/* ★ BISECT SWITCH — separates the two things that changed together in v2.
 *
 * v1 (clean, 4 runs) and v2/v3/v4 (froze, 3 runs) differ in two ways at once, and the
 * instrumentation has ruled out everything else: the pump, the drain loop, the logger
 * and the ring bookkeeping were all measured healthy at the last sample before a
 * freeze, with the ring drained and idle.
 *
 *   BEHAVIOUR  the consumer drains every pending request instead of only the newest,
 *              and the producer writes at a VARIABLE offset (reqRing[w & 15]) rather
 *              than a fixed one.
 *   MEMORY     CDEnginePublic grew from ~148 bytes to 456, so the engine's
 *              NewPtrSysClear takes a three-times-larger bite out of the SYSTEM heap
 *              — the same heap that holds this machine's experimental USB 2.0 EHCI
 *              driver. A different allocation size moves everything allocated after it.
 *
 * CD_RING_MODE 0 keeps the STRUCTURE exactly as it is — same size, same offsets, same
 * allocation — while restoring v1's single-slot BEHAVIOUR. So:
 *
 *   0 freezes  ⇒ the behaviour is innocent and the ALLOCATION SIZE is the trigger,
 *                which means the real fault is somewhere else in the system heap and
 *                this project has merely been perturbing it;
 *   0 is clean ⇒ the behaviour is the cause after all, and it is the producer's
 *                variable-offset write or the drain loop, not the memory layout.
 *
 * Either answer is worth the reboot; neither is reachable by reading the code, which
 * three builds have now demonstrated. */
#ifndef CD_RING_MODE
#define CD_RING_MODE 1
#endif

typedef struct {
    OSType          magic;          /* kEngineMagic                             */
    /* ★ 2 = the request ring replaced the single-slot mailbox, which moved every
     * field below it. A reader built against version 1 will misread this block, so
     * readers must check. */
    short           version;
    short           patched;        /* 1 while our TVector is in the descriptor */

    short           cdRefNum;
    short           reserved0;

    Ptr             origTVector;    /* saved, for reference                     */
    Ptr             ourTVector;

    Ptr             ring;           /* CDEngineTrace[ringEntries]               */
    long            ringEntries;

    volatile long   writeCount;     /* monotonic; & (ringEntries-1) = next slot */
    volatile long   callCount;      /* every Control call seen                  */
    volatile long   audioCallCount; /* just the audio-family csCodes            */

    /* ---- request mailbox -------------------------------------------------- *
     * ★ This is what the deadlock forced. The handler used to read the disc and start
     * playback itself, from inside the driver's own Control entry — and a synchronous
     * PBRead to a driver cannot begin until the Control call it is nested in returns.
     * Self-deadlock; AudioPlay froze on the first nested call.
     *
     * So the handler now does nothing but record the request here and chain. All I/O
     * happens in the pump application, at its own task level, outside any Control call
     * — which is exactly where Phase 1 measured 30 s of streaming with zero underruns.
     *
     * ★ A RING, NOT ONE SLOT — and it is a ring because one slot demonstrably lost
     * requests on hardware. The 2026-08-06d log skips request numbers (1, 4, 5, 6, 7),
     * and every earlier run does the same: the pump read whichever request happened to
     * be in the slot when it next looked, then set `lastSeq` to that sequence number,
     * stepping over everything that had arrived in between. Silently.
     *
     * It stayed harmless only by luck of ordering — the calls being lost were
     * AudioControl and AudioTrackSearch-with-hold, which the pump ignores anyway.
     * Nothing protected an AudioPlay, and AudioStop immediately followed by AudioPlay
     * is precisely how a game restarts a music loop. Losing that one is silence with no
     * error recorded anywhere, on someone else's machine.
     *
     * Single producer (the handler, at any interrupt level), single consumer (the pump,
     * at task level), so no lock is needed — only ordering:
     *
     *   producer  fills reqRing[reqWrite & mask], THEN publishes reqWrite + 1
     *   consumer  reads while reqRead != reqWrite, THEN publishes reqRead + 1
     *
     * Both indices are monotonic longs, never wrapped, so `reqWrite - reqRead` is the
     * exact backlog and cannot be confused with an empty ring. Aligned long loads and
     * stores are atomic on PowerPC, which is what keeps the handler safe at interrupt
     * level: it still does nothing but plain stores.
     *
     * If the producer ever laps the consumer it overwrites the OLDEST unread entries.
     * The consumer detects that (backlog > entries) and counts what it lost in
     * `reqDropped`, so an overflow can never be the silent thing this replaced. */
    volatile long   reqWrite;       /* monotonic count of requests POSTED       */
    volatile long   reqRead;        /* monotonic count CONSUMED (pump writes)   */
    volatile long   reqDropped;     /* entries overwritten before the pump saw  */
    CDEngineRequest reqRing[kEngineReqRingEntries];

    /* Pump -> engine, purely informational so the trace can show it. */
    volatile short  pumpAlive;
    volatile short  pumpPlaying;
    volatile long   pumpUnderruns;

    /* ★ PUMP LIVENESS, published rather than logged.
     *
     * The v3 run put a heartbeat in the pump's own log and learned nothing, because
     * that log had already stopped receiving lines — while the pump was provably still
     * playing, its synthesised position advancing smoothly to 3.00 s. Meanwhile the
     * probe wrote 359 lines to its own log in the same window, so the File Manager and
     * the volume were fine. The pump's logging channel was the thing that was broken,
     * and instrumentation that travels down a broken channel measures nothing.
     *
     * So the pump publishes here instead, and the PROBE reports it — through the
     * channel that demonstrably survives. `pumpBeat` rises once per pump loop pass: if
     * it is still climbing when the probe stops, the pump was alive to the end.
     * logQuietDepth and logLastErr say why the pump's own log went silent: a stuck
     * quiet region reads as depth > 0, a failing write as a non-zero error. */
    volatile long   pumpBeat;        /* ++ every pump event-loop pass            */
    volatile long   pumpReqSeen;     /* requests actually drained                */
    volatile short  logQuietDepth;   /* > 0 means CDLogf is discarding lines     */
    volatile short  logLastErr;      /* last FSWrite result in the pump          */
    volatile long   logWrites;       /* lines the pump handed to FSWrite         */

    /* ★ How much room is left in the SYSTEM HEAP, and how big this block is.
     *
     * The freeze now tracks the size of this allocation and nothing else, which points
     * away from our logic and at the heap itself: this machine also carries an
     * experimental USB 2.0 EHCI driver, whose buffers live in the same heap. If the
     * system heap is nearly exhausted, 308 extra bytes is exactly the sort of nudge
     * that makes someone else's unchecked allocation fail.
     *
     * Recorded at install time so the number is on the record before anything can go
     * wrong, and published so the probe can report it even if the pump's log dies. */
    volatile long   sysFreeAtInit;   /* FreeMemSys() after our allocations       */
    volatile long   sysLargestAtInit;/* MaxMemSys() - the biggest single block    */
    volatile long   pubBlockBytes;   /* sizeof(CDEnginePublic), as the ENGINE saw it */

    /* ---- the mailbox in reverse: the playback cursor ---------------------- *
     * Phase 0 proved this driver answers AudioStatus and ReadQ with a position that
     * never moves, because it never actually transports the disc. A game that polls
     * for track end therefore never sees one, which is exactly the community's
     * "music never loops" symptom.
     *
     * So the pump publishes where playback has actually reached — derived from bytes
     * the Sound Manager has consumed, not from what we asked for — and the handler
     * rewrites the driver's stale answer with these values.
     *
     * The handler only reads these, with plain aligned loads, so it stays safe at
     * any interrupt level. `posSeq` is bumped last by the pump; the handler does not
     * need it, but a reader can use it to tell a live cursor from a stale one. */
    volatile short  playState;      /* 0 stopped, 1 playing, 2 paused,           */
                                    /* 3 completed (hold the final position)     */
    volatile short  curTrack;       /* binary track number                      */
    volatile long   curAbsFrame;    /* absolute frame = LBA + 150               */
    volatile long   curRelFrame;    /* frames since the start of the track      */
    volatile long   posSeq;

    /* What the ORIGINAL driver answered, captured once so the pump can log it. The
     * MSF bytes we can decode and rewrite with confidence; bytes 0..2 of AudioStatus
     * were 0x00 in every Phase-0 sample and their encoding is still unconfirmed, so
     * seeing the real thing matters before trusting a guess. */
    volatile short  origStatusCaptured;
    volatile unsigned char origStatusParam[16];
    volatile unsigned char origReadQParam[16];
    volatile short  origReadQCaptured;

    volatile long   synthStatusCount;   /* how many answers we have rewritten   */
    volatile long   synthReadQCount;
} CDEnginePublic;

/* AudioStatus byte 0 while we are playing.
 *
 * ⚠ A GUESS, and flagged as one. The MMC audio-status codes are 0x11 play-in-progress,
 * 0x12 paused, 0x13 completed, 0x14 error, 0x15 no-status, and this driver returned
 * 0x00 in every Phase-0 sample — which is not a valid MMC code, so either it passes
 * something else through or 0x00 simply means "nothing playing". Reporting
 * play-in-progress is the reading most likely to make a polling game behave, and it is
 * a single constant to change if a game disagrees. The pump logs what the original
 * actually returned so this can be settled with evidence. */
#define kSynthStatusPlaying    0x11
#define kSynthStatusPaused     0x12
#define kSynthStatusCompleted  0x13

/* The command code we invoke DoDriverIO with. We call it ourselves rather than
 * letting the Device Manager do it — the fragment is never installed into the unit
 * table — so `contents` carries a CDEngineInfo* instead of a DriverInitInfo*. */
#define kEngineInitCommand      7    /* kInitializeCommand: validate, change nothing */
#define kEngineFinalizeCommand  8    /* kFinalizeCommand                             */

/* Step 3. Deliberately separate commands so that "validate" and "modify" can never be
 * the same call by accident — every hardware failure in this project so far came from
 * modifying the driver, and two of them were caught only because validation was a
 * distinct, harmless step. */
#define kEnginePatchCommand     0x4344      /* 'CD' : write our TVector in     */
#define kEngineUnpatchCommand   0x4355      /* 'CU' : write the saved one back */

#endif /* CD_ENGINE_H */

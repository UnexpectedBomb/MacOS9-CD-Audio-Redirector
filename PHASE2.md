# Phase 2 — the interception INIT: design

Written 2026-08-05, after Phase 1 succeeded.

> **⚠ UPDATED after `CDCtlDump_v1` ran (see FINDINGS.md, Phase 2 pre-work).**
> §3's blocking question is **answered and dissolved**: the Control entry is a Mixed
> Mode routine descriptor wrapping native PowerPC, so it is a callable routine that
> returns normally, and a patch can chain to it and then rewrite `csParam`. Good news.
>
> But §2's **all-68K decision is dead**: its premise (that the entry is 68K code) was
> false, and this Retro68 installation has **no m68k toolchain at all**, so no INIT of
> any kind can currently be built. **§7 is the open decision that must be settled
> before the vehicle is written.** Read §7 before §2.

## 1. What Phase 0 and 1 already settled

Nothing here is speculation any more:

| Fact | Consequence for Phase 2 |
|---|---|
| `.AppleCD` v1.4.8 is a **classic DRVR**, refNum −66, drive 4 | Patch the DRVR Control entry, not a native dispatch |
| Entry offsets: open 0x114, prime 0x134, **ctl 0x154**, status 0x174, close 0x194 | Control entry = `dCtlDriver + 0x0154` |
| `dRAMBased` clear ⇒ `dCtlDriver` is a **Ptr** | No handle dereference, no relocation to worry about |
| All audio + TOC calls are **Control**, not Status | One entry to patch, not two |
| Driver **accepts and ignores** transport commands, reports a frozen position | We supply both the audio *and* the status |
| DAE works: `ChangeBlockSize(2352)` + driver-level `PBRead` | The read path is settled |
| `'sowt'` honoured, `compressionID = notCompressed` | No byte swap at interrupt time |
| 2 s ring + 32-sector refills = 0 underruns over 30 s | Ring sizing is settled |

## 2. Architecture: all-68K, and why

The engine is going to be **68K code**, not a PPC fragment.

The Control entry we are patching *is* 68K code, so the patch has to be 68K
whatever else happens. The question is only where the engine lives. A PPC engine
would mean the proven-but-involved residency route (68K INIT →
`InstallDriverFromMemory` of a native-driver PEF) plus a Mixed Mode boundary at the
patch. An all-68K engine has neither: no CFM, no `InstallDriverFromMemory`, no
TOC/RTOC concerns at interrupt time, and one less place for the
`NewRoutineDescriptorTrap` trap to bite.

Cost: the mixing work runs emulated. It is a `BlockMoveData` of 44 KB per 0.25 s —
176 KB/s of memory copy. Trivial even under emulation, and Phase 1 measured zero
underruns with far more headroom than that.

**The one unavoidable Mixed Mode boundary** is the `SndDoubleBackUPP`. On OS 9 the
Sound Manager is native, so handing it a bare 68K procedure pointer gets that
address executed as PowerPC. From 68K, Retro68 `#define`s `NewSndDoubleBackUPP` to
a no-op cast, so it must be built by hand with **`NewRoutineDescriptorTrap`** —
exactly the gotcha `reference_os9_init_resident_driver` records costing a reboot
cycle on the USB2 work. Verify `upp != proc` before use.

### Residency

INIT code is discarded when the INIT returns, so the resident part cannot live in
it. The engine goes in a **separate 68K code resource** inside the INIT file,
loaded with `SetZone(SystemZone())` so it lands in the system heap, then
`DetachResource` + `HLock`. Its entry points are fixed offsets from the resource
base, which is also how the DRVR-shaped stub below gets built.

### Interrupt-time state

The doubleback proc needs no globals: state arrives through `dbUserInfo[0]`, the
same idiom Phase 1 used and the same discipline the VBL probe used for its
interrupt tasks. That sidesteps A4-relative globals in a detached code resource
entirely.

### The refill pump — `accRun`, already running

Phase 1 refilled the ring from its own task-level loop. An INIT has no such loop,
and the doubleback proc cannot read from the drive because it runs below task
level.

But the driver already asks for periodic task time: `dCtlFlags` = 0x7D24 has
**`dNeedTimeMask` (0x2000) set**, so `SystemTask` delivers **`accRun`, csCode 65,
as a Control call at task level**. Our patch sees those go past. That is the pump,
already installed, no new machinery.

Two caveats, both handled:

- **Period.** `dCtlDelay` was observed as 120 ticks on one run and 480 on another —
  2 to 8 seconds, far too slow and not stable. Phase 2 shortens `dCtlDelay` while
  playing (≈6 ticks ⇒ ~10 refills/s ⇒ ~17.6 KB each, comfortable against
  32-sector reads) and restores it on stop.
- **Starvation.** `accRun` only arrives when the foreground app yields. A game that
  never calls `WaitNextEvent` starves the ring. A background application would be
  no better — OS 9 multitasking is cooperative, so it gets time on the same
  condition. Mitigation is a larger ring (4 s) plus the underrun counter to
  measure it, and the rip-ahead-to-disk design in REVIEW.md §4 as the fallback if
  a real game turns out to be that hostile.

## 3. The one open question, and why the INIT waits on it

**How does `.AppleCD`'s Control entry return?** A classic DRVR either RTSes with
D0 = result, or jumps to `jIODone`. The patch shape differs completely:

- **RTS** ⇒ our handler can `JSR` the original, then inspect and rewrite `csParam`
  before returning. That is what synthesising `AudioStatus` and `ReadQ` needs, and
  Phase 0 proved that synthesis is mandatory.
- **jIODone** ⇒ calling the original never comes back to us. The patch must then do
  its side effects and **tail-jump**, with no chance to post-process results — so
  the status codes have to be answered entirely by us, without chaining, which
  means getting the completion convention exactly right by hand.

There is a second, related unknown: the five entry offsets are evenly spaced 0x20
apart, which is the signature of a **jump table**, not five routines. If 0x154 is a
stub, the patch still goes there, but knowing what it jumps to is needed to reason
about the return.

Guessing either of these in boot code produces a machine that hangs at startup with
no debugger and no breadcrumb. The project's own rule is explicit about this
situation: two speculative ROMs were shipped on the USB2 work and both theories
were wrong, while a probe answered it in one run.

**So `CDCtlDump_v1` goes first** — an ordinary app, one double-click, read-only, no
boot risk. It dumps the DRVR header, the Control entry, and follows one level of
jump if the entry is a stub, so a jump table costs no second run. Then the patch
gets written against what the code actually does.

## 4. Patch mechanics (independent of §3)

Entry points in a DRVR header are **16-bit offsets from the header base**, so the
patch cannot simply be pointed at by editing `drvrCtl` — our code will not be within
64 KB of the original header, and even if it were, editing a shipping driver's
header in place is needlessly invasive.

The classic technique instead:

1. Allocate a block in the **system heap** holding a **DRVR-shaped header followed
   by our code**, so our own `drvrCtl` offset is small and correct by construction.
2. Copy the original header's flags, delay, emask and menu into ours, and point our
   `drvrOpen`/`drvrPrime`/`drvrStatus`/`drvrClose` offsets at thin stubs that
   tail-jump to the original's absolute entries. Only `drvrCtl` goes to our handler.
3. Save the original `dCtlDriver`, then repoint the DCE's `dCtlDriver` at our block.
4. Our Control handler chains to `originalBase + 0x154` for everything it does not
   handle — an absolute address, computed once at install time.

Uninstall is the reverse: restore `dCtlDriver`. Worth having, because it makes the
shift-key escape hatch and a clean failure path possible.

### Registers and ABI

A DRVR Control entry is entered with **A0 = ParmBlkPtr, A1 = DCtlPtr**, which C
cannot express, so the entry is a small assembly shim that marshals A0/A1 into a C
call and preserves everything the convention requires. Passthrough is a tail jump
with A0/A1 untouched, which preserves the original's return convention exactly
whatever it turns out to be — the one part of the design that is safe under either
answer to §3.

## 5. Which csCodes we take

| csCode | Handling |
|---|---|
| 104 AudioPlay | Start the engine at the requested track/MSF |
| 103 AudioTrackSearch | Seek: set the cursor, hold |
| 105 AudioPause | Pause/resume the engine |
| 106 AudioStop | Stop, free the channel, restore `dCtlDelay` |
| 108 AudioScan | Map to a cursor move, or ignore |
| 109 AudioControl | Software volume: scale samples |
| 107 AudioStatus | **Synthesise** from our cursor |
| 101 ReadQ | **Synthesise** from our cursor |
| 100 ReadTOC | Pass through — the driver's TOC is correct |
| 65 accRun | Refill the ring, then pass through |
| everything else | Pass through untouched |

Synthesis uses the byte layouts captured from the real driver in FINDINGS.md:
`ReadQ` = `[ctrl/adr, track BCD, index BCD, rel M S F BCD, abs M S F BCD]`, and
`AudioStatus` bytes 3..5 = absolute MSF in BCD.

## 6. Staging, and safety

**2a — patch with no audio.** The INIT installs the patch, records every
intercepted csCode into an interrupt-safe ring in the system heap, passes
everything through unchanged, and a companion app dumps the ring. This proves the
risky half — residency, the patch, passthrough — with a payload that cannot
misbehave. It also delivers the game call trace that PHASE0.md §P5b deferred,
essentially for free, and answers the `accRun` period question with real numbers.

**2b — add the engine.** Once 2a boots reliably and the trace shows the real call
pattern, wire in the Phase-1 engine.

Non-negotiables for both:

- **Shift at boot skips installation entirely** — the standard INIT convention, and
  the only recovery path if a patched boot goes bad.
- **Test on the expendable OS9 LAB volume, not the primary**
  (`feedback_os9_bootcode_testing_safety`).
- **Guard the install**: if the driver is not found, `devt` is not `'cdrm'`, the
  header does not look like a DRVR, or `dCtlDriver` is not a plausible pointer,
  do not patch. A refused install is a working machine.
- **Capability check**: on a Mac whose analog CD-audio path works, do not intercept
  at all — the same discipline the VBL fix landed on. Detect and no-op rather than
  double-play.

---

## 7. OPEN DECISION: what vehicle carries the patch?

`CDCtlDump_v1` settled the ABI question and simultaneously invalidated §2's toolchain
choice. A classic INIT is a **68K code resource**, Retro68's own sample says
PowerPC is not supported for code resources, and **this installation has no m68k
toolchain** — no `m68k-apple-macos` directory, no m68k gcc, no 68K CMake file.

So FEASIBILITY §2's stated goal, "one system extension (an INIT)", is not currently
buildable. Two ways forward.

### Option A — build the m68k toolchain, keep the INIT

Retro68 can build both targets; the sources are in `~/Retro68/` and the build trees
in `~/Retro68-build/`. Adding the m68k target is a multi-hour build.

- **For:** delivers exactly the intended artifact — drop one file into Extensions,
  it works from boot, nothing visible to the user, no Startup Items entry.
- **Against:** hours of toolchain work, and a non-zero risk of disturbing a
  PowerPC toolchain that currently builds everything in this project correctly.
  `feedback_toolchain_snapshots` says snapshot before toolchain work, so that is a
  prerequisite, not an optional extra.
- Note the INIT would still only be the *installer*; the engine could stay PowerPC.

### Option B — PowerPC only: faceless Startup-Items app + resident native driver

Recommended, and it is the arrangement this project has already validated once.
`reference_os9_init_resident_driver` records the USB2 conclusion verbatim: a resident
INIT cannot give post-boot work top-level task context, so **the shippable auto-run
vehicle is a top-level process, not an INIT** — a faceless background app hidden via
SIZE flags (`modeCanBackground|modeOnlyBackground|modeHighLevelEventAware` = 0x14C0).

Shape:

1. **The engine** is a native driver PEF, made resident with `InstallDriverFromMemory`
   so it is owned by the system Unit Table and survives whatever launched it. This is
   the pattern proven on hardware for USB2 (R2b-3).
2. **A faceless Startup-Items app** installs the DCE patch and then stays running as
   the task-level pump for ring refills.
3. **Our patch entry** is our own Mixed Mode routine descriptor with
   `ISA = kPowerPCISA` and `procInfo = 0x00179822` — *exactly mirroring what
   `.AppleCD` itself does at all five of its entries*. We are not inventing a shape;
   we are imitating the driver's own.

- **For:** builds today with the toolchain in hand; no 68K anywhere; the background
  app is a *better* pump than `accRun` (it can allocate, log, and use the File
  Manager, none of which `accRun`-time code should assume); proven vehicle on this
  project; and the engine code is reusable under Option A anyway.
- **Against:** the deliverable is an app in Startup Items rather than a single
  extension, which is a visible change from FEASIBILITY §2's goal. Also the patch
  must be **unpatched on quit** — if the app quits while `dCtlDriver` points into its
  fragment, the next CD Control call jumps into freed code. Making the engine a
  resident driver (step 1) is what contains that risk, and a quit handler that
  restores `dCtlDriver` closes it.

### Why this is the user's call

Both produce working software and the engine work transfers either way, so nothing
is wasted by choosing B now and revisiting. But Option A costs hours of toolchain
build plus a snapshot, and Option B changes the shape of what ships. That is a
product-and-time tradeoff, not a technical one, so it is not mine to make silently.

### What does not change either way

Everything in §1, §4 (patch mechanics), §5 (which csCodes we take) and §6 (staging
and safety) stands, with two refinements from the dump:

- Chaining is a **call**, not a tail jump: the original returns, so `csParam` can be
  rewritten afterwards. The tail-jump fallback is no longer needed.
- We reuse the original's descriptor **by address** and never have to construct a
  procInfo to chain. Only our own entry descriptor needs building, and its shape is
  copied from the driver's.

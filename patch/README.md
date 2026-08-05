# Phase 2a — the Control-entry patch, trace only

`CDPatch2a.bin` (extension) + `CDTraceDump_v1` (reader). **No audio.** 2a installs the
patch, records every Control call, and hands every one to the original driver
unchanged. The engine arrives in 2b.

## ⚠ Read this before the first boot

This is the project's first boot code. A bad extension means a machine that hangs at
startup.

1. **Install on the expendable OS9 LAB volume, not the primary** —
   `feedback_os9_bootcode_testing_safety`.
2. **Hold SHIFT at boot to skip installation.** Checked before anything else happens;
   it is the recovery path.
3. **Name the file so it loads late.** Extensions load in an undefined order, and if
   `.AppleCD` has not loaded yet there is nothing to patch. Rename the file to begin
   with a tilde — e.g. `~CD Audio Patch 2a` — because `~` is 0x7E and sorts after
   every letter. If it still loses the race, the INIT logs `no CD driver yet` and does
   nothing, which is a clean no-op rather than a broken boot.
4. Every outcome is one line in **`CD Patch Log`** in the System Folder, flushed, so
   even a hang after that point leaves the reason on disc.

## What to do — start with the app, not the boot

The first hardware attempt (2026-08-05) refused to patch at boot: the INIT ran during
the extension parade, found no CD driver, and logged
`no CD driver yet (load order - rename to sort later)`. That is the safe path working,
but it meant the patch's mechanics could not be tested at all. So test from the app
first, where boot timing is irrelevant.

### 1. Prove the patch works (no reboot, no risk)

1. Run **`CDPatchInstall`**. It performs the *identical* install from a normal
   application — same `'CDpt'` blob, same system-heap residency — long after every
   extension has loaded and the drive has been enumerated. It also dumps the whole
   unit table by name and the drive queue into `CD Patch Log`, which is what tells us
   why the boot attempt failed.
2. Touch the drive: insert an audio CD, play a track in iTunes, or run `CDRecon_v2`.
3. Run **`CDTraceDump_v1`** and send back `CD Trace Log` + `CD Patch Log`.

The patch stays resident after `CDPatchInstall` quits because the blob is **copied into
a `NewPtrSys` block** and the copy is what runs — the resident code is in memory
allocated in the system heap explicitly.

> **⚠ The first attempt at this crashed into MacsBug.** The `'CDpt'` resource was marked
> `preload`, so it was read into the *application's* heap at launch, before
> `SetZone(SystemZone())` could matter. `DetachResource` and `HLockHi` faithfully kept it
> there, the patch pointed at app-heap code, and the moment the app quit that memory was
> freed with `dCtlDriver` still pointing into it — so the next Control call jumped into
> freed memory. `preload` is gone and the loader no longer depends on which heap happens
> to be current. If you are recovering from that crash: just restart. The patch was never
> persistent, and a fresh boot rebuilds `dCtlDriver` from scratch.

### 2. Then fix the boot path

`CD Patch Log`'s unit-table dump answers which fix is needed:

- **`.AppleCD` is absent from the unit table** ⇒ the driver's own extension had not
  loaded yet. Rename this extension to sort later (`~CD Audio Patch 2a`; `~` is 0x7E
  and sorts after every letter).
- **`.AppleCD` is present but the drive queue was empty** ⇒ load order was fine and v1
  simply looked in the wrong place. The revised blob now scans the unit table by name
  first, so a reboot should just work.

### Removing it

Hold **option** when launching `CDTraceDump_v1` to restore the original `dCtlDriver`
live, no reboot. Or take the extension out of Extensions and restart.

## What 2a is actually proving

| Question | How it answers |
|---|---|
| Does the residency mechanism work? | The blob is loaded into the system heap, detached, locked and self-relocated; if `dCtlDriver` still points at our shell after boot, it survived the INIT being discarded |
| Does the patch get called? | A rising `callCount` |
| Is passthrough transparent? | Data reads, iTunes and `CDRecon` all keep working |
| **Are the audio calls immediate or queued?** | Every trace entry records `ioTrap`; bit 9 is `noQueueBit` |
| What does a game actually call? | The ring — this is the trace PHASE0.md §P5b deferred |

That fourth one is the one 2b hinges on. Retro68's own `libDRVRRuntime` shows a
Control routine ending in a plain `RTS` for an immediate call but jumping to `jIODone`
for a queued one. If the audio calls are immediate, 2b can call the original and
rewrite `csParam` afterwards — which is exactly what synthesising `AudioStatus` and
`ReadQ` requires. If they are queued, 2b has to complete those requests itself.

## Design notes

- **Passthrough is a tail jump, not a call.** Our Control shim preserves A0/A1 across
  the C tracer, then pushes the original's entry address and `RTS`es into it, leaving
  the Device Manager's own return address on the stack. Whatever the original does —
  `RTS` or `jIODone` — happens exactly as it would have without us. That is safe under
  either ending, which is why 2a can be trusted before the ending is known.
- **The tracer is interrupt-safe.** Control can be issued asynchronously, so the
  handler may run below task level. It does nothing but plain stores into a
  pre-allocated ring: no allocation, no File Manager, no logging, no waiting.
- **Discovery is a passive unit-table name scan**, revised after the first boot. v1
  walked the drive queue and asked each of its drivers via DriverGestalt; that found
  nothing during the parade, because the drive queue lists *drives* and the CD drive
  may not be enumerated that early even when `.AppleCD` is loaded. The scan now reads
  each DCE's DRVR header name straight out of memory and looks for `.AppleCD`, calling
  no driver at all — so it cannot hang the way the Phase-0 unit-table sweep did, since
  that hang came from issuing Status calls to arbitrary drivers. The single candidate
  found by name is then verified with one DriverGestalt call, and the drive-queue path
  remains as a fallback.
- **Only `drvrCtl` is ours.** The other four entries are 6-byte `JMP` stubs to the
  original's absolute entries. Those entries are Mixed Mode routine descriptors
  wrapping native PowerPC, and a `JMP` to a descriptor behaves exactly like the
  Device Manager's own call.
- **The header is copied verbatim** — flags, delay, emask, menu — so the Device
  Manager treats our shell exactly as it treated the real driver, including
  `dNeedTime`, which is what keeps `accRun` arriving. That is 2b's refill pump.
- **The install refuses rather than guesses.** No CD driver, no DCE, an implausible
  `dCtlDriver`, a Handle-based driver, a header that is not DRVR-shaped, our magic
  already present — every one of those returns a reason and patches nothing. A
  refused install is a working machine.
- **Uninstall never frees the shell or ring.** A Control call could still be inside
  our handler; leaking two small system-heap blocks until restart is far cheaper than
  freeing memory something might still be using.
- **`Retro68FreeGlobals` is deliberately not called in the blob.** Its globals have to
  outlive the installer call. The INIT does call it, because its own do not.

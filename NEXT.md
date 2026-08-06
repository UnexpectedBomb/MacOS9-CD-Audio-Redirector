# Where this project stands, and what to do next

Written 2026-08-06 for a cold start. Read this first, then `FINDINGS.md` for evidence.

## It works

On the G4 mini, with a pressed audio CD: a legacy `AudioPlay` — the call a mixed-mode game
issues — produces **audible music**, plus a truthful advancing position, on a machine with no
analog CD-audio path. Verified end to end, twice, with the numbers checking out to the frame.

The mechanism is **complete**. What remains is packaging and validation against a real game.

## Current artifacts (staged on the Pi at `/home/csell/shared/`)

| Artifact | What it is |
|---|---|
| **`CDPump_v4`** | The whole fix: patches the driver, then stays running as the audio pump. Re-reads the TOC per play, so a disc inserted after launch works |
| `CDPump_v3` | The previous build, kept on the share as the known-good control |
| `CDPlayProbe_v3` | Stands in for a game — issues the legacy audio calls, asks if you heard music |
| `CDTraceRead_v1` | Reads the engine's trace ring via `Gestalt('CDau')` |
| `CDRecon_v2` | Phase-0 recon: driver identity, TOC, DAE gate, mounted volumes |
| `CDCtlDump_v1` | Dumps the driver's Control entry (read-only) |
| `CDAudioSpike_v1` | The standalone Phase-1 DAE → Sound Manager spike |

## How to run it

1. Reboot **with the drive empty** — that is the faceless-boot case the TOC re-read exists for.
2. Launch **`CDPump_v4` holding option** — patches, then stays running. **Leave it open.**
   Expect `no TOC at startup (empty drive?) - will re-read on the first play request`.
3. **Now** insert an audio CD.
4. Run **`CDPlayProbe_v3`**. Expect music, `pump: TOC generation 1` with the full track table,
   `the CD Audio Redirector IS resident`, and `reported status CHANGED`.
5. Click in the pump window to stop it. `CDTraceRead_v1` shows the trace.

Logs land in the System Folder: `CD Engine Log`, `CD Play Probe Log`, `CD Trace Log`.
They **append** — read from the last banner.

**Nothing persists across a restart.** Recovery from anything is always a reboot.

## Next steps, in order

### 0. ✅ Re-read the TOC on demand — DONE in `CDPump_v4`, **PASSED on hardware 2026-08-06c**
`CDPumpInit` used to read the TOC once and never again, so a faceless Startup-Items app
launching at boot with an **empty drive** would have had `gTOC.valid` false for the whole
session and could never have resolved a request.

`CDPumpPlay` now calls `EnsureTOC()` before resolving anything. It re-reads on **every** play
request rather than trying to detect a disc change: the read is three Control calls and
measured one tick, against a play path costing 1.5 s, so a staleness heuristic would be more
code and more ways to be wrong to save 0.7% of the budget. The read runs under
`CDLogSetQuiet` (new, in `cd_probe_common`) so it does not append ~40 lines and as many
flushed `FSWrite`s to the start of every piece of music; the full track table is logged only
when the disc actually **changes**, tagged `TOC generation N`.

Two deliberate choices, both to avoid making things worse than they were:
- a failed re-read **keeps** a TOC already in hand, so a transient glitch cannot turn a
  playing disc into an unresolvable request;
- the block size is restored around the read and retaken afterwards. `ReadTOC` is a Control
  call and ought not to care, but every TOC read that has succeeded on this hardware happened
  at 512, and two Control calls are cheap insurance. On the first play nothing is taken, so
  it costs nothing.

**Validated.** Booted with an empty tray, launched the pump, *then* inserted the disc: the
startup TOC read failed cleanly with `err=-65` (no hang), the pump ran anyway, and the first
play request logged `TOC generation 1` with the full table and played. Two plays produced
exactly one generation line, so the change suppression works.

**Fold into the packaging build (step 1), both found by that run:**
- `EnsureTOC` should also refresh **`gDriveNum`**. With an empty tray the CD is absent from
  the drive queue, so the pump starts with `drive=0` and uses it as `ioVRefNum` on every
  `PBRead`. It worked here because the read is driver-level and `ioRefNum` selects the driver,
  but that is now the *default* shipping configuration and it assumes the driver ignores
  `ioVRefNum`. Another drive may not.
- The `discovery stage 2 SKIPPED (shift held)` message is wrong — the call site hardcodes
  `allowFullSweep = false` (`cd_engine_install.c:474`). Say what the code did.
- Do not assume the CD driver's refNum: it was −66 one boot and −56 the next.

### 1. Faceless Startup-Items packaging  *(packaging, no new mechanism)*
Ship as one file the user drops into Startup Items. Hide it from the Application menu with
SIZE flags `modeCanBackground|modeOnlyBackground|modeHighLevelEventAware` = **0x14C0**, the
shape the USB2 work settled on. It must patch without the option key in that mode, and it
must not open a window.

⚠ It **cannot** become a real INIT. Three independent reasons, all established:
an INIT has no ongoing task-level context; the audio must run outside any driver Control
call (the deadlock); and an INIT patches too early to find the real ATAPI driver. A faceless
app gives the identical user experience — one file, one restart, invisible.

### 2. Multi-track and looping behaviour
**Repeat play is DONE and PASSED** (2026-08-06b, three plays against one live pump: cursor
restarts from zero every time, 0 underruns, block size taken and given back three times each
— see FINDINGS). Still open, and both need a probe change because `CDPlayProbe` always
targets the *first* audio track and always stops after ~12 s:

- a second `AudioPlay` for a **different** track (new `gTrackStartLBA`, new range);
- a track played to its **natural end**, confirming the position holds with status `0x13`
  rather than reverting.

### 3. ⚠ A REAL MIXED-MODE GAME DISC — the biggest untested gap
Everything so far used a pressed *audio* CD. Untested and materially different:
- track 1 is **data**, so `DecodePos` must resolve positions on a disc whose first track is
  not audio;
- the **game** issues the calls, not our probe, and may use the track-number form rather
  than MSF, or poll differently;
- whether an audio session mounts alongside the HFS data volume (Phase 0's P3 left this
  open);
- **the game reads level data from track 1 while music plays** — the block-size race in
  `cd_pump_audio.c` is documented but unsolved, and this is where it would surface. If it
  does, the fix is the ATA route via the `'dvrf'` handle.

This is Jubadub's run (macos9lives topic 7829); the fix is primarily for him.

### 4. Third-party README, then the handoff
Install steps, what to expect, how to remove it, and what to report back. Then the forum
post to topic 7829. **Both the GitHub push and the forum post need explicit permission** —
ask every time. No em-dashes in anything published externally.

## Traps already paid for — do not rediscover these

- **No synchronous driver I/O from inside that driver's own Control entry.** It deadlocks:
  the read cannot start until the Control call returns, and the call is waiting for the read.
  `accRun` is no escape, being a Control call itself. This is why the pump exists.
- **`preload` on a resource defeats `SetZone(SystemZone())`** — it is read when the *file*
  opens, into the app heap. And **CFM uses a PEF's code section in place**, so a PEF must be
  `NewPtrSys`-copied *before* `GetDriverMemoryFragment` or the resident code lives in the app
  heap and dies with it. Both crashed the machine.
- **Never hand-count offsets into a `RoutineDescriptor`** — use `MixedMode.h`'s structs.
  procInfo +0x0C, ISA +0x11, procDescriptor +0x14, 32 bytes total.
- **`NewGestaltValue`** installs a new value selector; `SetGestaltValue` does not. Try all
  three and log every result.
- **Never discard an error from a call that can fail silently.** Two separate failures here
  were undiagnosable because of a `(void)` cast or a re-opened log.
- **Version-stamp every artifact.** An unversioned build cost a whole debugging cycle chasing
  a stale copy.
- **A diagnostic's interpretation text goes stale.** `CDPlayProbe`'s "therefore" line was
  wrong twice in three runs as the system gained capabilities. Re-check the tooling's
  conclusions whenever behaviour changes.
- **A build script that hardcodes the artifact version will silently ship a stale binary.**
  `engine/scripts/push-to-pi.sh` had `BASES="CDPump_v3"`; when the target became v4 it
  re-pushed the v3 still sitting in the build directory and printed a success line. It now
  takes the name from CMake and fails loudly if nothing was copied. **The four probe push
  scripts still hardcode their names** — latent, since those versions have not moved, but the
  same trap is armed. Fix them when any of those artifacts next changes version.

## Numbers worth knowing

Measured 2026-08-06b, G4 mini, pressed audio CD:

- **Warm CD-DA read throughput ≈ 330 KB/s, 1.9x real time.** Cold it is 153 KB/s, which is
  *below* real time. This is the number that decides whether a game can read level data while
  music plays; the spare bandwidth is about 0.9x real time, which is not generous.
- **Start latency: 1.5 s warm, 2.7 s cold**, from the game's `AudioPlay` to first sound
  (~0.37 s mailbox latency plus the 2.0 s pre-roll read). Reducible by shrinking the pre-roll,
  but the pre-roll is also the underrun cushion — do not shrink it blind.
- `accRun` arrives every 120 ticks = 2.0 s; the pump does not rely on it, it refills from its
  own event loop.

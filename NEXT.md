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
| **`CDPump_v3`** | The whole fix: patches the driver, then stays running as the audio pump |
| `CDPlayProbe_v3` | Stands in for a game — issues the legacy audio calls, asks if you heard music |
| `CDTraceRead_v1` | Reads the engine's trace ring via `Gestalt('CDau')` |
| `CDRecon_v2` | Phase-0 recon: driver identity, TOC, DAE gate, mounted volumes |
| `CDCtlDump_v1` | Dumps the driver's Control entry (read-only) |
| `CDAudioSpike_v1` | The standalone Phase-1 DAE → Sound Manager spike |

## How to run it

1. Reboot. Confirm iTunes reads an audio CD (the baseline).
2. Launch **`CDPump_v3` holding option** — patches, then stays running. **Leave it open.**
3. Run **`CDPlayProbe_v3`**. Expect music, and a log saying
   `the CD Audio Redirector IS resident` plus `reported status CHANGED`.
4. Click in the pump window to stop it. `CDTraceRead_v1` shows the trace.

Logs land in the System Folder: `CD Engine Log`, `CD Play Probe Log`, `CD Trace Log`.
They **append** — read from the last banner.

**Nothing persists across a restart.** Recovery from anything is always a reboot.

## Next steps, in order

### 0. ⚠ Re-read the TOC on demand  *(blocks step 1 — do this first)*
`CDPumpInit` reads the TOC **once**, at launch, and nothing re-reads it
([cd_pump_audio.c:189](engine/cd_pump_audio.c:189)); `DecodePos` resolves every request
against that snapshot. A faceless Startup-Items app launches at boot with an **empty drive**,
so `gTOC.valid` is false for the whole session and no request can ever resolve. Re-read
inside `CDPumpPlay` when the TOC is invalid or the disc may have changed.

Until this is fixed, every hardware run must put the disc in the drive **before** launching
the pump, and must not swap discs afterwards: one disc per boot.

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
- **The pump's TOC is a snapshot taken at launch.** Disc in the drive first, no swaps.
  See step 0 — this is also a shipping blocker.

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

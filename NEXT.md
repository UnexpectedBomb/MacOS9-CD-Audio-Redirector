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
Play a track to its natural end and confirm the position holds at the boundary with status
`0x13` rather than reverting. Then a second `AudioPlay` for a different track.

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

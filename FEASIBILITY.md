# OS 9 CD Audio Redirector — Feasibility Scope

**Status:** SCOPED, NOT STARTED. This document is the entire briefing — a fresh session can pick it
up cold. Nothing has been built. Written 2026-08-04 as an offshoot of the Mac Mini G4 audio work;
it is a **separate project** from the amp Control-Strip module and the software-volume driver.

---

## 1. One-paragraph problem statement

Early "mixed-mode" (data track + Red Book audio tracks) Mac CD games — *Warcraft: Orcs & Humans*,
*Quake* before the 1.09 patch, and a shelf of others — play their music by telling the CD-ROM drive
to **play an audio track over the old analog CD-audio path**. Late Macs (the **Mac Mini G4** especially,
but also other post-~2000 models) **have no analog CD-audio wire** to carry the drive's audio to the
sound chip, and OS 8.1+/ATAPI changed the CD driver behavior these games assume. Net result: the
in-game "play track N" command succeeds or silently no-ops, and **no music comes out** — while in-game
digital sound effects are fine. This is a *broad* late-Mac problem, not Mini-specific, and the community
(macos9lives topic 7829: Jubadub/dtekle; Macintosh Garden *Warcraft* comments: Jatoba/MSX) has
identified it but not fixed it — only worked around it (loop the tracks in iTunes by hand).

**Proven fact that makes a fix possible:** iTunes / Apple CD Audio Player *can* play those same tracks
on the Mini, digitally (Digital Audio Extraction → PCM through the Sound Manager). So the DAC, the
amps, and the digital output path all work. The only broken link is the *legacy game → CD-audio route*.

## 2. Goal, and explicit non-goals

**Goal:** one system extension (an INIT) that makes the legacy CD-audio *control calls* audible again,
transparently and game-agnostically — by intercepting them and servicing them with DAE + Sound Manager
streaming. Fix the whole class of games at once, no per-title patching.

**Non-goals (keep the scope honest):**
- NOT a per-game binary patch (that is the fallback if this proves infeasible — see §10).
- NOT the Sound control-panel "CD input" crash. That is a *different* bug — the MacOS9Lives spoof
  advertising a CD-audio input the Toonie DAC doesn't have (the same phantom-hardware pathology as the
  dead hardware volume). Out of scope here; note it, don't chase it.
- NOT related to the Toonie/`awgc`/software-volume mess. This project rides on the **already-working**
  digital PCM output; it does not need any of that solved. (Big de-risk: independent of that quagmire.)
- NOT a CD *ripper*, not a general media player, not analog line-in monitoring.

## 3. How CD audio actually works on classic Mac OS (the mechanism we're bridging)

- **The legacy API.** Apps drive the Apple CD-ROM driver (`.AppleCD`) through the **Device Manager**
  (`PBControl`/`PBStatus`) using a family of audio **csCodes**: *AudioTrackSearch, AudioPlay, AudioPause,
  AudioStop, AudioStatus, AudioControl (volume), ReadTOC, ReadQ (sub-channel/position)*. The app reads
  the TOC, then issues AudioPlay over a track/MSF range and (often) polls AudioStatus to know when a
  track ends so it can loop or advance. **Exact csCode numbers must be pulled from *Inside Macintosh:
  Devices* (SCSI / CD-ROM driver chapter) + the Apple CD-ROM driver docs during Phase 0 — do not trust
  memory for the numbers.**
- **Analog model (dead on the Mini).** Historically AudioPlay made the drive decode the track to analog
  and push it down a physical cable into the sound hardware's CD-audio input mixer. No such cable/mixer
  on the Mini ⇒ silence.
- **Digital model (works on the Mini).** DAE = read the CD-DA sectors as *data* off the bus and play
  them as PCM through the Sound Manager. This is what iTunes/AppleCD Audio Player do. **Our extension
  makes the legacy commands take the digital road.**
- **CD-DA data format (concrete detail that matters):** a CD-DA sector is **2352 bytes = 588 stereo
  frames of 16-bit signed *little-endian* PCM** at 44100 Hz; 75 sectors/second = 176,400 B/s. The Sound
  Manager's 16-bit format is **big-endian** (`k16BitBigEndianFormat`/`twos`). **⇒ every sample must be
  byte-swapped LE→BE before playback.** Cheap, but mandatory.

## 4. Root-cause hypotheses to confirm in Phase 0 (the two that matter)

- **H1 — the game's AudioPlay reaches the driver but produces no output** (analog path gone / driver
  accepts-but-no-route). Fix = intercept and DAE.
- **H2 — the driver rejects the audio csCodes outright** (the ATAPI-era `.AppleCD` doesn't implement
  them, so the game gets an error and gives up). Fix = still intercept, but we must also *return
  success-shaped results* so the game proceeds; and we may need to synthesize TOC/status the game trusts.

Either way the interception approach applies; H2 just means we must emulate more of the driver's
audio surface convincingly. A **call trace (Phase 0)** tells us which, plus the exact csCodes/params.

## 5. Proposed architecture

```
   game ──PBControl(AudioPlay,…)──▶  [ our patched CD-driver Control entry ]
                                        │  audio csCode?
                              yes ◀─────┤─────▶ no ──▶ original driver Control (passthrough)
                                        ▼
                              [ CD Audio Engine ]
                                 • TOC → LBA range for the track
                                 • async DAE reads (READ CD) → task-level RING buffer
                                 • SndPlayDoubleBuffer: interrupt-time doubleback proc
                                   pulls from ring, byte-swaps LE→BE, fills SM buffers
                                 • playback cursor → answers AudioStatus/ReadQ polls
                                 • AudioStop/Pause/Resume/TrackSearch handlers
```

- **Interception point (recommended): patch the CD driver's DCE `Control` routine.** Find the CD
  driver in the unit table (by name `.AppleCD` / by driver identity), save its original `Control`
  (and `Status`) entry, install ours via a Mixed Mode routine descriptor. Our handler switches on
  csCode: audio-family → engine; everything else → call the saved original unchanged.
  - *Alternative:* patch the `_Control`/`_Status` **traps** (Get/SetOSTrapAddress) and filter by the
    CD driver's `ioRefNum`. Easier to install, but catches every Control call system-wide (must filter
    tightly). **Prefer the DCE patch — surgical.** Document both; decide in Phase 2.
- **When to install the patch.** The CD driver loads with the volume/driver, potentially after the INIT
  runs. Options: (a) patch at boot and re-verify/re-apply lazily on first CD access; (b) hook the CD
  driver's open. **Must be present before a game issues AudioPlay.** Design this deliberately in Phase 2.
- **Two-stage buffering (this is the crux of correctness):**
  - *Task level:* async `READ CD` completion routines refill a large pre-allocated **ring buffer**
    (read-ahead, e.g. 1–2 seconds, to ride out seeks/retries).
  - *Interrupt level:* the `SndPlayDoubleBuffer` **doubleback proc** copies+byte-swaps from the ring
    into the SM's double buffers. **It must never block, allocate, or touch the File Manager** — if the
    ring is momentarily empty, it outputs silence and flags an underrun, never waits.

## 6. Hard technical risks & the OS 9 lessons that apply

- **★ Interrupt-safety (the recurring landmine).** The doubleback proc and the READ-completion routines
  run below task level. **No File Manager, no synchronous PB calls, no allocation there.** See the memory
  `reference_os9_no_filemgr_at_interrupt` (bit the USB/EHCI work three times) and
  `reference_os9_stage_fm_buffer_dma`. Pre-allocate everything; use interrupt-safe flags + a task-level
  drain, exactly like the EHCI ring pattern.
- **DAE availability = the make-or-break unknown.** Does the Mini's OS 9 CD driver expose raw audio-sector
  reads, and via what — a driver read call, or must we go to the **ATA Manager / SCSI Manager and issue
  `READ CD (0xBE)`** ourselves? iTunes proves the *hardware+OS can*; the *API surface we get to use* is
  unconfirmed. **This is the decisive gate — Phase 0.**
- **Driver-ABI / Mixed Mode from driver context.** Patching a driver's Control entry and being called
  back through Mixed Mode with the right PB conventions is delicate; get the RoutineDescriptor/ProcInfo
  right or it crashes at I/O time. (We have the residency/Mixed-Mode playbook from the amp INIT and the
  resident driver-loader work — `reference_os9_init_resident_driver`.)
- **Status/position fidelity.** Games poll AudioStatus/ReadQ in different unit conventions (absolute vs
  track-relative, MSF vs frames). Report a cursor derived from bytes streamed so **loops and level-sync
  behave**. Getting units wrong = music that never loops, or loops instantly.
- **Coexistence — must not break anything that works.** Passthrough for all non-audio csCodes must be
  perfect (games read their *data* from track 1 through the same driver). Don't break iTunes/AppleCD
  Player (they may use DAE directly — ensure we only intercept the legacy control path). Handle eject,
  disc-swap, no-disc, and multiple audio streams gracefully. Follow `feedback_dont_break_coexisting_drivers`.
- **Underrun / throughput.** 176 KB/s is trivial for a G4, but CD **seek latency** and read retries can
  starve the ring. Mitigate with generous read-ahead and by not seeking mid-track. Test on a scratched/
  slow disc.
- **Mixed-mode TOC.** Track 1 = data (skip it); audio tracks follow. Parse the TOC and map the game's
  track numbers correctly.

## 7. Phased plan (front-load the kill switch)

- **Phase 0 — Recon + the decisive DAE probe (cheap, do first).**
  1. *Trace:* a tiny logger (app or extension) that records every `PBControl`/`PBStatus` to the CD driver
     while a broken game (Warcraft) runs — csCodes + params + return values. Resolves H1 vs H2 and gives
     the exact call surface. (Interrupt-safe logging or task-level ring, per our discipline.)
  2. *DAE probe:* a standalone test that reads a known audio track's sectors on the **Mini** under OS 9
     (via the driver read path or `READ CD 0xBE` through ATA/SCSI Manager) and checksums/saves them.
  **GO/NO-GO GATE:** if we cannot DAE-read audio sectors on the Mini's drive under OS 9, the extension
  approach is **blocked** → fall back to per-game patching or declare infeasible. iTunes strongly implies
  GO, but *this probe is the whole ballgame* — run it before writing any engine code.
- **Phase 1 — DAE→Sound Manager spike.** Standalone app: read one audio track via DAE and play it through
  the Sound Manager (byte-swap, double-buffer, no interception). Proves the playback engine + timing on
  real hardware. Deliver an audible track.
- **Phase 2 — Interception.** The INIT that patches the CD driver Control entry, catches AudioPlay, and
  routes to the Phase-1 engine. First success = Warcraft plays its music through us.
- **Phase 3 — Status/loop/transport fidelity.** AudioStatus/ReadQ position, AudioStop/Pause/Resume,
  AudioTrackSearch. Test the games that loop and sync to track boundaries.
- **Phase 4 — Hardening & coexistence.** Passthrough correctness, several games, don't-break iTunes/data
  reads, eject/swap/no-disc, error paths. Ship.

## 8. Toolchain, residency, build

- **Residency ⇒ almost certainly CodeWarrior, not Retro68.** This is a resident boot INIT that patches a
  driver — and `reference_retro68_no_ppc_init` says Retro68 can't build a resident PPC INIT (the amp INIT
  needed CodeWarrior for exactly this reason). Plan on CodeWarrior for the INIT shell; the engine can be
  PPC/CFM. **Reconfirm** whether any part can be Retro68 (e.g. the Phase-0/1 spikes as plain apps — yes,
  those can be Retro68).
- Spikes (Phase 0 trace, Phase 1 player) are **ordinary apps** → Retro68 is fine and faster to iterate.
- Reuse the project's build/staging pipeline conventions (`stage-artifacts.sh`, APM-wrap for mountable images,
  version-stamped artifacts).

## 9. Prior art / references to pull in the dedicated session

- *Inside Macintosh: Devices* — SCSI Manager + the CD-ROM (`.AppleCD`) driver: the audio csCodes, TOC,
  Q sub-channel, and read calls (authoritative for the exact numbers/structs).
- The Apple CD-ROM driver ("AppleCD") + **Apple CD Audio Player** (digital audio behavior).
- QuickTime's CD-audio import (another digital path, for reference).
- ATAPI **MMC `READ CD` (0xBE)** command spec (for direct DAE if the driver won't expose it).
- Linux `cdda2wav`/`cdparanoia` and BSD DAE code — battle-tested READ-CD quirk handling.
- Community context: macos9lives **topic 7829** (Jubadub, dtekle) and **Macintosh Garden** *Warcraft:
  Orcs & Humans* comments (Jatoba, MSX) — see `reference_macos9lives_forum`.
- Our own lessons: `reference_os9_no_filemgr_at_interrupt`, `reference_os9_init_resident_driver`,
  `reference_retro68_no_ppc_init`, `feedback_os9_bootcode_testing_safety`, `feedback_dont_break_coexisting_drivers`,
  and the audio findings in `project_mini_g4_audio` / `project_g4_audio_driver`.

## 10. Fallback if the extension proves infeasible

Per-game binary patch: reverse-engineer the title's CD-audio routine and replace the AudioPlay path with
an embedded DAE→Sound Manager player. Real work per game; only sensible for a couple of marquee titles.
Same engine as Phase 1, injected into the game instead of the driver. Keep as Plan B.

## 11. Effort & honest reality check

- **Phase 0 is small and decisive** — a day or two of spikes answers whether the whole thing is possible.
- **Phases 1–4 are a medium driver project** — the streaming/interrupt/status work is the meat, and it
  needs **several hardware-test cycles** (reboot per test; no debugger). Follow the hardware-test
  discipline (state the question + discriminator before each run; expendable test volume —
  `feedback_os9_bootcode_testing_safety`).
- **Blocker to weigh:** the primary test Mini is currently showing a suspected heat/GPU fault (see
  `project_mini_g4_audio`). This project needs a healthy Mini (or the user's second one) for its many
  test cycles. Don't start Phase 1+ until there's a reliable machine.
- **Payoff:** one extension revives Red Book audio across a whole shelf of classic games on late Macs —
  high community value, and it rides on the already-working digital path rather than the Toonie quagmire.

## 12. First actions when this is picked up

1. Read this doc + the memories named in §9.
2. Confirm a healthy test Mini is available.
3. Build the **Phase-0 trace** (Retro68 app/extension) → run Warcraft → capture the exact CD-driver
   audio calls (H1 vs H2, csCodes, params).
4. Build the **Phase-0 DAE probe** → confirm audio-sector reads work on the Mini under OS 9. **This gate
   decides the project.**
5. Only then start Phase 1.

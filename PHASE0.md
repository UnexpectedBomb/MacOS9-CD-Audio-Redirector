# Phase 0 — run protocol

Built 2026-08-04. Everything here is an ordinary double-clickable PowerPC app: no
boot code, no INIT, no reboot per iteration. Each probe states its question and its
discriminator **before** the run, so no result can come back ambiguous.

Read `REVIEW.md` for why Phase 0 has this shape rather than the one in
`FEASIBILITY.md` §7.

## What to run, in order

| # | Probe | Artifact | Machine | Touches |
|---|-------|----------|---------|---------|
| P1 | iTunes vs AppleCD Audio Player | none — manual | mini | nothing |
| P2 | driver classification | `CDRecon_v1` | mini (then MDD) | read-only |
| P3 | audio tracks as files | `CDRecon_v1` | mini | read-only |
| P4 | **the DAE gate** | `CDRecon_v1` | mini | block size, restored |
| P5a | legacy audio API | `CDPlayProbe_v1` | **MDD first**, then mini | playback + CD volume, restored |

P2–P4 are one run of one app.

**What disc to use.** P4 and P5a only need Red Book audio tracks, so an ordinary
**pressed music CD** runs both. Prefer a pressed disc over a CD-R for P4
specifically: if the gate failed on a burn, media would be a live confound and the
run would have to be repeated on a pressed disc anyway, and the gate is the one
result that must not come back ambiguous.

Only **P3** needs a real mixed-mode disc (data track 1 + audio tracks) — and a
music CD still answers half of it: if the tracks appear as AIFF-ish files, Audio CD
Access is present and working on this system, leaving only "does it do the same for
the audio session of a mixed-mode disc?". That first half is the part that would
simplify the design, so it is worth having early.

A mixed-mode game disc (Warcraft: Orcs & Humans is the reference title) is needed
for the full P3 answer and for Phase-2 validation against a real game. Note that
**"hybrid" is not "mixed mode"**: most Mac game discs of the era are hybrid
(Mac + PC *data*), which is unrelated. Enhanced CD / CD-Extra music albums are
audio + data but *multisession*, a different layout again — an interesting extra
data point, not a substitute.

Artifacts are staged on the Pi share as `CDRecon_v2.bin` / `.img` and
`CDPlayProbe_v2.bin` / `.img` in `/home/csell/shared/`. **Use v2 and trash v1** —
v1 hung on its first hardware run (see below).

**Modifier keys at launch:**

- **shift** — safe mode: skip the full unit-table sweep entirely. Only the drive
  queue's own drivers get asked anything.
- **option** (CDRecon only) — skip P4, keeping the run 100% read-only.

### What v1 got wrong, and what v2 does about it

v1 hung on both apps' first run: blank menu bar, live cursor, no window. Both logs
end at the same line, the unit-table sweep header. The sweep sent a DriverGestalt
Status call to every populated entry in a 96-entry unit table, and one of those
drivers accepted the call and never completed it. `PBStatusSync` spins on
`ioResult`, so that is a permanent hang — waiting longer would not have helped.
Poking arbitrary non-disk drivers to find the CD was careless.

Worse, the sweep had no surviving breadcrumb: `CDDriverGestalt` bypassed the
logging wrappers, and `CDLogf` only flushed inside `CDLogStep`, so the per-entry
lines that would have named the guilty driver never reached disc.

v2:

1. **Drive-queue-first discovery.** Stage 1 asks only the drivers listed in the
   drive queue, which are block drivers by definition and well behaved. The CD
   driver is in there whenever it is loaded, disc or no disc, so stage 1 is
   normally the whole job and the sweep never runs at all.
2. **The sweep is a fallback**, only if stage 1 finds no CD, and skippable with
   shift.
3. **Every driver call announces itself** in a flushed log line *and* on screen, in
   a progress window opened before the first driver call. A hang is now visible,
   attributable, and distinguishable from merely slow.
4. **Pointer sanity check** before dereferencing `dCtlDriver` for a hex dump, so a
   bogus DCE gives a log line instead of a bus error.

---

## P1 — iTunes vs AppleCD Audio Player (manual, do this first)

**Question:** is the project's premise actually true?

`FEASIBILITY.md` §1 cites iTunes *and* AppleCD Audio Player as evidence that the
tracks play digitally on the mini. Those are not equivalent evidence. AppleCD Audio
Player is a transport controller: it issues the same legacy audio csCodes to the
drive that the games do, over the analog path. iTunes is the digital case.

**Do:** with the same mixed-mode disc in the drive, try to play an audio track in
iTunes, then in AppleCD Audio Player.

**Discriminator:**

- iTunes audible, AppleCD Audio Player silent → the §3 model is confirmed. Proceed.
- **Both audible** → the diagnosis is wrong. The analog route is not the broken
  link, and the whole design needs re-scoping before anything else happens. Stop
  and say so.
- Both silent → something more basic is wrong (disc, drive, output selection).
  Sort that out before trusting any later probe.

## P2 — which driver, and is it classic or native? (`CDRecon_v1`)

**Question:** does the optical drive belong to a classic `DRVR` or a native
`ndrv`? Everything about the interception design follows from this, and the eSATA
work already established that a mis-shaped native dispatch field fails as a
garbage-UPP branch rather than a clean error.

The app finds the driver by walking the unit table and asking each entry what it
is via DriverGestalt `'devt' == 'cdrm'` — deliberately not by matching the name
`.AppleCD`, which varies across ATAPI-era builds.

**Discriminator:** in the log,

- `GetDriverInformation: NATIVE ndrv` → interception patches the native dispatch;
  our shim must be resident PPC code (the proven 68K INIT +
  `InstallDriverFromMemory` route, not CodeWarrior).
- `GetDriverInformation: err=… ⇒ classic 'DRVR'` → interception patches the
  `drvrCtl` / `drvrStatus` offsets, which the log prints.

Also captured, because the later phases need them: `'intf'` (expect `ata `),
`'dvrf'` (the ATA device handle route-B DAE would need), `'nmrg'`, `'vers'`, the
full DCE and AuxDCE, and the drive number.

Worth running on the MDD too, for comparison — if the two machines differ, the
extension has to handle both.

## P3 — do the audio tracks show up as files? (`CDRecon_v1`)

**Question:** under classic Mac OS with QuickTime, an audio CD mounts as a volume
of AIFF-readable track files. Does that happen for the audio session of a
**mixed-mode** disc?

**Discriminator:** the log lists every mounted volume and, for any volume on the CD
driver, its whole root directory with types and creators.

- A file of type `AIFF` / `AIFC` / `cdda` / `trak` → **P3 GO**: DAE is a plain
  `FSRead`, and the ATA Manager branch of the design is unnecessary. Big
  simplification, and it also makes rip-ahead-to-disk trivial.
- Nothing → expected; the audio session does not mount, and DAE has to come from
  P4's route A or route B.

## P4 — THE GATE: can we read CD-DA sectors at all? (`CDRecon_v1`)

**Question:** the decisive one. If audio sectors cannot be read on this drive under
OS 9, the extension approach is blocked and the fallback is per-game patching.

Route A: ask the driver for a 2352-byte block size, then do a driver-level
`PBRead` at the first audio track's LBA + 10 seconds (offset in, so a silent
pregap cannot masquerade as a failed read). The block size is restored on every
exit path.

**Discriminator:**

- `⇒ P4 GO` (read succeeded, >25% non-zero bytes, sane peak amplitudes) → the
  project is viable and route B is never needed.
- `read SUCCEEDED but the data is at or near silence` → inconclusive. Re-run
  against a different track before drawing any conclusion.
- `2352 refused` or `read FAILED` → route A is out. Next step is route B: ATAPI
  `READ CD (0xBE)` through the ATA Manager, using the `'dvrf'` handle P2 logged.
  Only if that also fails is the extension approach blocked.

Hold **option** at launch to skip P4 and keep the run entirely read-only.

## P5a — the legacy audio API, driven directly (`CDPlayProbe_v1`)

**Question:** H1 (driver accepts AudioPlay, nothing audible because there is no
analog route) or H2 (driver rejects the audio csCodes and games give up)?

`FEASIBILITY.md` §7 proposed answering this by tracing a running game, which needs
resident boot code. But H1 vs H2 is a question about the *driver*, not the game, so
this app asks the driver directly: it issues the same sequence a mixed-mode game
issues — read the TOC, position to the track, play, poll status, pause, resume,
stop — and logs every csCode, parameter and result.

Three things it settles beyond H1/H2:

- **The address encoding.** The audio calls take a position-type byte plus a
  position, and the available source material does not pin down the values. The app
  tries the plausible types in both MSF and track-number form and records which one
  the driver accepts, instead of the engine guessing later.
- **The position units** reported by AudioStatus and ReadQ during ten seconds of
  playback — absolute vs track-relative, MSF vs frames. `FEASIBILITY.md` §6 flags
  getting this wrong as "music that never loops, or loops instantly". Look for the
  field advancing 75 per second.
- **The volume confound.** It reads the drive's CD-audio volume, sets it to full
  before playing, and restores it. Without that, "silent" could just mean the
  volume was zero, and H1 would be a false positive.

**Run it on the MDD first** — that machine still has the analog CD-audio wire, so
it is the known-good control. Running the control before the suspect is the house
rule that has repeatedly reframed ambiguous results.

**Discriminator** (the app asks you whether you heard anything and records the
answer in the log):

| MDD | mini | Meaning |
|-----|------|---------|
| AudioPlay noErr, audible | AudioPlay noErr, silent | **H1 confirmed.** Interception + DAE is the right design. |
| AudioPlay noErr, audible | AudioPlay refused | The mini's driver does not implement the audio surface: **H2**. The extension must synthesise convincing replies, not merely add sound. |
| AudioPlay refused | AudioPlay refused | H2 everywhere; re-check the csCode numbers in `include/cd_cscodes.h` before believing it. |
| AudioPlay noErr, silent | — | The control case failed. Suspect the analog input is muted in the Sound control panel, or the disc. Fix before testing the mini. |

⚠ This probe is **active**: it starts and stops playback and changes the drive's
audio volume. It always issues AudioStop before exiting.

## P5b — the resident trace INIT: deliberately not built yet

The INIT that patches the driver and logs a real game's calls is **deferred**, for
three reasons:

1. **It only catches 68K callers.** A 68K `WaitNextEvent` trap patch on this very
   hardware installed but never fired, because the PPC-native Finder goes through
   InterfaceLib and bypasses the 68K trap table (`reference_os9_init_resident_driver`).
   `_Control` is no different. So the trace would be blind to PPC-native games.
2. **P5a answers the question it was for.** H1 vs H2 is a driver property, and an
   app can ask the driver directly for the cost of a double-click.
3. **Its design depends on P3 and P4.** If audio tracks turn out to be files, or
   if route A is refused, both the engine and what the trace needs to capture
   change. Building boot code before the cheap read-only answers come back is
   exactly the "two speculative ROMs, both theories wrong" mistake.

It becomes worth building if a specific game turns out not to use `PBControl` on
the CD driver at all — e.g. if it goes through SCSI Manager pass-through, which a
DCE patch would never see. That is the one question P5a cannot answer.

---

## What to capture and send back

1. `CD Recon Log` from the System Folder — **read from the last banner**, the file
   appends across runs.
2. `CD Play Probe Log`, same.
3. The P1 answer (which of iTunes / AppleCD Audio Player produced sound).
4. Which machine each run was on.

Both logs are flushed before every driver call, so if a run hangs the machine, the
last `STEP` line in the log names the call that did it — capture the log anyway,
that is the most valuable single line in it.

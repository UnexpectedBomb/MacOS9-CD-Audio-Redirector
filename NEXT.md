# Where this project stands, and what to do next

Written 2026-08-06 for a cold start. Read this first, then `FINDINGS.md` for evidence.

## ★ THE SHIP GATE — read this before proposing anything as finished

**The music must start every single time it is supposed to. Any known path where it might not
means this is not ready to ship. There is no acceptable rate of "sometimes the music doesn't
play."**

Stated by the user 2026-08-07, and it is not a preference — it is the definition of done for
this project. The entire purpose of the extension is that a legacy game's `AudioPlay` produces
music. A build that mostly does that has not fixed the problem, it has *reproduced* it: the
user's original symptom is silent music, and an intermittent failure to start is the same
experience with a different cause. Jubadub would have no way to tell the two apart, and
neither would we.

Concretely, this rules out shipping:

- **the single-slot mailbox as it stands** (`CDAudioRedirector_v1`). It drops a request
  whenever two arrive inside one pump pass (~170 ms). Most sequences survive because the pump
  keeps the *newest* request, but `AudioPlay` followed quickly by `AudioPause` loses the Play,
  and then nothing recovers until the game issues a fresh Play. Silence, no error, no counter.
- **any variant whose failure mode is silent.** Whatever ships must be able to say that it
  dropped something — the ring's `reqDropped` is the model. A failure we cannot see in a log
  is one we cannot answer a bug report about.

The user has explicitly chosen to keep hardening this until it works every time rather than
ship something with a known hole. Do not offer "ship it with a caveat in the README" as a way
out of a diagnosis that has become tedious. Finish the diagnosis.

⚠ This also means the two-slot fallback is **not** automatically acceptable. It narrows the
window; it does not close it. Three requests inside one pump pass would still lose one. It is
only a candidate if it can be shown to make the loss impossible for the call patterns a real
game issues, or if it carries a counter that makes any loss visible.

## ⏸ PAUSED 2026-08-07, waiting on a disc

Work stopped deliberately here. A **Warcraft: Orcs & Humans** disc was bought and is in the
post; there is nothing worth doing until it arrives, because every remaining question needs a
mixed-mode disc to answer and this machine does not have one.

**Pick up here:**

1. Disc arrives. Check it in a modern Mac: the Finder should mount a data volume **while** the
   Music app shows audio tracks.
2. On the mini, run **`CDRecon_v2`** and confirm the log now says
   `track 1: raw=0x04 ctrl=0x4 DATA`. That single line re-verifies the TOC fix on real media
   before anything else is attempted.
3. Then the full run: delete both logs, `CDAudioRedirector_v10` in Startup Items, reboot with
   the tray empty, insert the disc, wait for it to mount, run **`CDPlayProbe_v12`** three times.
   Phases A to C should behave as they did on an audio CD but targeting **track 2**, and
   **phase D** finally gets a data track to contend with.
4. Then launch Warcraft itself and send the log. The pump records every serviced request with
   its parameters, so that captures the game's real call pattern.

Also worth doing: tell Jubadub the bug he found is fixed, and ask him to retest with v10. He is
already set up, and his disc is a second machine and a second copy of the game.

## Where this stands

On the G4 mini, with a pressed audio CD: a legacy `AudioPlay` — the call a mixed-mode game
issues — produces **audible music**, plus a truthful advancing position, on a machine with no
analog CD-audio path. Verified end to end many times, with the numbers checking out to the
frame. Faceless Startup-Items packaging works, unattended, from a cold boot with an empty tray.

✅ **The freeze is solved.** `CDAudioRedirector_v10` runs clean and drops nothing: the request
ring lives in its own system-heap allocation, so the published block stays at 160
bytes — essentially v1's 148, the size profile with the most hardware behind it. Three runs,
120 polls, 18 of 18 requests serviced, 0 dropped.

✅ **Every mechanism gap is now closed.** Track switching and natural end-of-track both passed
on 2026-08-07, the latter landing on the track boundary to the frame. See the table in
FINDINGS 2026-08-07j.

★ **A REAL MIXED-MODE DISC HAS NOW BEEN TRIED, and it found a defect no test here could
have.** Jubadub ran v9 against Warcraft: Orcs & Humans and the TOC parser called its 261 MB
data track AUDIO, because it read the high nibble of the control field instead of the low one.
On an all-audio disc both nibbles read as zero, so roughly thirty hardware runs could not have
seen it. Fixed in **v10**, along with a pump guard that refuses to stream a DATA track and a
ring grown 16 → 64 after his run overflowed it. Full account in FINDINGS 2026-08-07m.

⚠ **Not finishable yet.** v10 has never run against a mixed-mode disc; it fixes a bug found on
one but has not been tested on one. The `paramErr` question is settled-negative (step 5), and
the freeze's mechanism is mitigated rather than explained (FINDINGS 2026-08-07g).

## Current artifacts (staged wherever `scripts/stage-artifacts.sh` points)

**Engine block version 5.** Every reader checks it and refuses on a mismatch, so a stale
binary announces itself rather than misreporting. The matching set:

| Artifact | What it is |
|---|---|
| **`CDAudioRedirector_v10`** | ★ **The current build.** Faceless, 64 request slots, ring in its own allocation (block 160 B). No freeze, nothing dropped |
| **`CDPump_v14`** | Diagnostic build of the same source: window, option to patch, click to stop. Log: `CD Engine Log` |
| **`CDPlayProbe_v12`** | Stands in for a game. Three phases now: A track start, **B track switch**, **C natural end of track**. Reports the pump's published state under every poll. Sweep is opt-in (option); refuses to run with no disc |
| **`CDTraceRead_v5`** | Reads the engine's trace ring via `Gestalt('CDau')` |
| `CDRecon_v2` | Phase-0 recon: driver identity, TOC, DAE gate, mounted volumes |
| `CDCtlDump_v1` | Dumps the driver's Control entry (read-only) |
| `CDAudioSpike_v1` | The standalone Phase-1 DAE → Sound Manager spike |
| `CDAudioRedirector_bisectB` / `bisectC` | Inline-ring data points, 2 and 4 slots. Diagnostics that established the size threshold, **not** candidates — bisectB drops a request every run |
| **`CDAudioRedirector_v1` + `CDPlayProbe_v3`** | ★ **The known-good control pair.** 4 runs, 0 freezes. Keep both on the share — this pair is what every regression gets measured against |

⚠ Everything else is superseded: `CDAudioRedirector_v2`–`v6` and `bisectA/bisectD`,
`CDPump_v4`–`v10`, `CDPlayProbe_v4/v5`, `CDTraceRead_v1/v2`. Readers built against an older
engine version refuse rather than misreport, but delete them from the share anyway so the
right file is the obvious one to pick.

**How to tell what you actually ran:** every build now logs its own configuration from the
compiled constants. The current one says
`build config: CD_RING_MODE=1  ringEntries=64  ringSeparate=1  faceless=1  structBytes=160`.

## How to run it

**`CDAudioRedirector_v10` is the build to run.** It meets the ship gate's two mechanical
requirements — it does not freeze and it drops nothing — but it is not validated for release;
see the remaining work below.

**Always, before any run:** delete `CD Audio Redirector Log` and `CD Play Probe Log` first.
A run that appends to an old log has put the interesting session in the *middle* of the file,
and "read from the last banner" has already pointed at the wrong session once.

**A faceless build:**

1. Put it in `System Folder:Startup Items:`, removing any earlier copy. Reboot **with the
   drive empty** — that is the real installed configuration.
2. Insert an audio CD.
3. Run **`CDPlayProbe_v12`**. Expect music, `the CD Audio Redirector IS resident`, and a
   `pump: beat=… reqR=… reqW=…` line under every poll.
4. Evidence is `CD Play Probe Log` — the pump's own log has gone silent before and cannot be
   relied on as the only channel.

**The diagnostic build** (`CDPump_v14`), when you want the window and manual control:
reboot with the drive empty, launch it **holding option**, leave it open, insert the disc, run
`CDPlayProbe_v12`, then click the pump window to stop. `CDTraceRead_v5` shows the trace.

⚠ **Always confirm the disc has mounted before launching the probe.** With no disc there is no
CD in the drive queue; the probe now refuses rather than sweeping the unit table, which hung
the machine once.

Logs land in the System Folder: `CD Engine Log` (or `CD Audio Redirector Log` for a faceless
build), `CD Play Probe Log`, `CD Trace Log`. They **append**, which is why they must be deleted
before each run — see above.

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

**Both follow-ups from that run are now folded in** (in `CDPump_v6` / `CDAudioRedirector_v2`):
- `EnsureTOC` now calls `RefreshDriveNumber()`, which re-walks the drive queue while
  `gDriveNum` is still 0. With an empty tray the CD has no queue entry, so the pump used to
  start with `drive=0` and pass it as `ioVRefNum` on every `PBRead`. It worked here because
  the read is driver-level and `ioRefNum` selects the driver — but that is now the *default*
  shipping configuration, and it assumed this driver ignores `ioVRefNum`. Another drive need
  not be so forgiving.
- The `discovery stage 2 SKIPPED (shift held)` message now says what the code actually did.
- Do not assume the CD driver's refNum: it was −66 one boot and −56 the next.

### 1. ✅ Faceless Startup-Items packaging — **PASSED on hardware 2026-08-06d**
Installed in Startup Items, booted with an empty tray, disc inserted after: patched
unattended with no key held, both Apple event handlers installed, not in the Application
menu, `drive number resolved to 4`, 16-track disc picked up as `TOC generation 1`, music
audible, 0 underruns.

✅ **The quit path is verified too.** The log re-read after a restart carries, at the end of
that first session, `=== pump stopped: 2196 KB delivered, 0 underruns ===` followed by the
faceless NOTE block and `=== end of run ===`. So the quit Apple event reached the app at
shutdown, the pump stopped cleanly and the log was closed properly rather than the app being
force-quit. The same file then shows a **second** session from the next boot — this one with
a disc already in the tray, so the startup TOC read succeeded and logged all 16 tracks. The
normal-boot-with-disc case is therefore covered as well.

One source, two targets, selected by `CD_FACELESS`. Building the shipping artifact from the
same file as the tested one is deliberate: a parallel copy of an installer is exactly how the
thing that ships stops being the thing that was tested.

What the faceless build changes, and nothing else: no progress window, no modifier key
required, quit and open-application Apple events handled, and the pump loop ends only on
quit rather than on a click.

**SIZE flags are verified, not asserted.** A Rez `SIZE` resource is sixteen anonymous
booleans in a fixed order; naming one out of place sets a different flag silently.
`scripts/check-size-flags.sh` reads the word back out of the built binary and fails the build
unless it is exactly **0x14C0** (`canBackground | onlyBackground | is32BitCompatible |
isHighLevelEventAware`). Negative control: the interactive build assembles to 0x58C0, and the
check correctly rejects it.

⚠ It **cannot** become a real INIT. Three independent reasons, all established:
an INIT has no ongoing task-level context; the audio must run outside any driver Control
call (the deadlock); and an INIT patches too early to find the real ATAPI driver. A faceless
app gives the identical user experience — one file, one restart, invisible.

⚠ **The open question this build cannot answer without hardware:** whether a background-only
app gets enough time to keep the ring fed while a *game* holds the foreground. Every run so
far had the pump in the background behind `CDPlayProbe`, which sleeps generously, and it
recorded zero underruns. A full-screen game may be greedier. The underrun counter in the log
is the measurement; if it climbs, the dials are a bigger ring first, then larger reads per
refill.

### 1b. ✅ The request ring — DONE, and the freeze it caused is SOLVED
The single-slot mailbox dropped requests whenever two arrived inside one pump pass. Replacing
it with a 16-entry ring fixed that and froze the machine, 4 runs out of 4 — and a long bisect
showed the cause was neither the ring's behaviour nor the pump but the **size of the published
block**: 468 bytes inline froze, 152 bytes with the ring separately allocated does not, at the
same total system-heap cost. `CD_RING_SEPARATE` is now the default. Full account in FINDINGS
2026-08-07c/d/e/g.

⚠ **The mechanism is still unknown.** The mitigation is that the block is back to v1's proven
size, not that we understand what a 468-byte allocation does to this machine. If this ever
misbehaves elsewhere, start there.

### 2. ✅ Multi-track and looping behaviour — **BOTH PASSED on hardware 2026-08-07**
**All three now pass.** Repeat play (2026-08-06b), the track switch (2026-08-07i) and natural
end of track (2026-08-07j). `CDPlayProbe_v8` exercises all of it in one run:

- **Phase B — track switch. ✅** The pump recomputes the range and the track base, and the
  cursor restarts for the new track rather than continuing the old count.
- **Phase C — natural end of track. ✅** The position stops on the track boundary **to the
  frame** (abs 33447 = LBA 33297 = track 3's start), the status byte becomes `0x13` and the
  pump reports `state=3`, held for every poll afterwards. Reached cheaply by starting playback
  six seconds before the boundary rather than sitting through the track.

Both phases skip themselves, loudly, on a disc without a second audio track that has a
successor in the TOC. Use a disc with three or more audio tracks.

⚠ **Phase C's `AudioPlay` is REFUSED by the original driver** (`paramErr`, because the address
is mid-track) **and the pump plays it anyway** — the handler posts before it chains. The probe
polls regardless for exactly that reason. See the open question in step 5.

### 2b. ✅ Phase D — data reads DURING playback (built, needs a mixed-mode disc)
The contention case: a game reads level data off track 1 **while** its music plays. Never
exercised, and the most likely thing to break under a real game, because the pump takes the
drive's block size to 2352 for the whole of playback while the File Manager expects 512. That
race is documented in `cd_pump_audio.c` and has never been resolved.

`CDPlayProbe_v12` phase D hunts the **silent** version of that failure, not just errors:

1. finds the CD's data volume and the largest file in its root;
2. reads a 32 KB chunk with **nothing playing** and keeps a checksum;
3. starts playback, then re-reads the same bytes 24 times during it;
4. reports read errors, **checksum mismatches**, and the pump's underrun count.

A mismatch means the same bytes read differently while audio was playing, which is corruption
no other part of the system would report. Phase A is the built-in control for the other half:
it plays identically with no data reads and has recorded zero underruns every run, so underruns
appearing only in phase D means the reads are starving the audio.

Skips loudly on a disc with no data volume, so it is harmless on an ordinary audio CD.

### 3. ⚠ A REAL MIXED-MODE GAME DISC — the biggest untested gap
**A copy of Warcraft: Orcs & Humans (original Mac CD) was bought 2026-08-07 and is in the
post.** That is one of the two titles this project's problem statement names, so it is the
canonical case rather than a proxy.

When it arrives, before anything else: put it in a modern Mac and confirm the Finder mounts a
data volume **while** the Music app shows audio tracks. Then run `CDRecon_v2` on the mini and
check track 1 reads `DATA`. Only then is it worth a test boot.

Note what is already instrumented for this: the pump logs **every serviced request** with its
parameters, unbounded, to `CD Audio Redirector Log`. So a real game's call pattern - which
tracks it asks for, in what order, by MSF or by track number, and how it polls - is captured
without any new code. The 512-entry trace ring may wrap during a long session; the log will not.


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

### 3b. Publication hygiene — DONE, and one decision recorded
The working tree is sanitized: the eight `push-to-pi.sh` scripts are one
`scripts/stage-artifacts.sh` with no machine details, reading an untracked `.stage-target`.
`scripts/publish-dist.sh` scrubs the build-machine account name out of the shipped binaries,
which Retro68's prebuilt newlib bakes in via its own `__FILE__` strings and no compiler flag
can reach. Tracked tree scans clean.

**Decision, 2026-08-07: git history keeps the old build-host details, and that is accepted.**
Around a dozen commits still contain the staging host name, the account name and the share path
in their diffs. Sanitizing the tree does not sanitize history, a public repo exposes it, and the
alternatives were a fresh-history clone or `git filter-repo`. The user chose to accept it.
**Do not re-raise this**; it is settled, and the history is worth more as a record of the
investigation than the exposure costs.

(Deliberately not spelling those strings out here. Writing them into a tracked file would put
them back into the published tree, which is the leak one level up, and is exactly what happened
in the first draft of this paragraph.)

### 4. ✅ HANDED OFF — repo public 2026-08-07, forum post made by the user
`github.com/UnexpectedBomb/MacOS9-CD-Audio-Redirector` is public under MIT, `dist/` carries
`CDAudioRedirector_v10` and `CDPlayProbe_v12`, and the user posted to macos9lives themselves.

**Now waiting on Jubadub.** Two branches from here:

- **It works for him.** Then the mixed-mode gap is closed by someone else's disc, and the
  Warcraft disc in the post becomes a nice-to-have second data point rather than the critical
  path.
- **It does not.** The Warcraft disc arrives in a few days and becomes the troubleshooting
  vehicle. **Ask for the log first, before any theory**: `CD Audio Redirector Log` distinguishes
  "never installed", "the game never asked for audio" and "tried and failed", which are
  indistinguishable from a verbal description. That is what the log was built to do.

### 4b. (superseded) Third-party README, then the handoff
⚠ Gated by the ship gate at the top of this file: do not write the handoff for a build with a
known path to music-not-starting. The README is not a place to disclose a hole we chose not to
close.
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
  the staging script had `BASES="CDPump_v3"` hardcoded; when the target became v4 it
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

### 5. ✅ SETTLED — the game IS still told "no", and it cannot be helped
With the redirector installed the **driver's return code no longer indicates whether audio
will play**. The handler posts the request to the pump *before* chaining, so the pump has
already accepted the work when the original driver forms its own opinion — and they can
disagree. Observed 2026-08-07i: `.AppleCD` returns `paramErr` for a **mid-track** `AudioPlay`
while the pump resolves it correctly and plays.

A game that *checks* that error could conclude CD audio is unavailable and disable its music,
while the redirector is working perfectly. Real games mostly address track starts, which
return `noErr`, so this is secondary — but the ship gate's standard is "every time".

**Tried, and it cannot work.** v8 replaced the return value with `noErr`. Hardware showed the
counter incrementing three times while the caller received −50 all three times: these calls are
**queued**, so the driver completes through `jIODone`, `ioResult` carries the answer, and the
Device Manager has already captured it before our chain returns. Rewriting `csParam` after the
chain works; rewriting the *result* does not, and those are not the same manoeuvre.

Reverted in v9. The counter stays as **`refusalsServiced`** — requests the pump played that
the driver refused — because that is what a real game's complaint would look like from here.

**This does not fail the ship gate:** the pump plays regardless, so the music starts. The
residual exposure is a game that *disbelieves* a `paramErr` and disables its own music —
reachable only via a mid-track `AudioPlay` (track starts return `noErr`), speculative, and
untestable without a real game. Full reasoning, including why writing `ioResult` was rejected,
in FINDINGS 2026-08-07l.

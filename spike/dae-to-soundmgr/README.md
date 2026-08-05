# Phase 1 — DAE → Sound Manager spike

`CDAudioSpike_v1`. Reads CD-DA off the drive and plays it through the Sound
Manager. No interception anywhere; that is Phase 2, which will reuse this engine.

Put an **audio CD** in the drive and double-click. Progress appears in a window as
it works, and the log is `CD Audio Spike Log` in the System Folder (appended — read
from the **last** banner).

## Three questions, one run

Hardware trips are expensive, so the run answers everything it can.

| | Question | How | Discriminator |
|---|---|---|---|
| **Q1** | Which byte order does the Sound Manager want? | Stage A plays the **same 5 seconds twice**: A1 as `'sowt'` with bytes untouched, A2 as `'twos'` with bytes swapped | Whichever sounds like music. The wrong one is unmistakable — wrong-endian 16-bit PCM is loud white noise, not subtly-off music |
| **Q2** | Does the double-buffer engine work at all? | Stage A preloads all 5 s into RAM first, so nothing depends on refilling in time | Clean audio in either A1 or A2 ⇒ channel, rate, stereo and the interrupt-time doubleback proc are all correct |
| **Q3** | Can the ring sustain real-time playback from the drive? | Stage B streams 30 s through a 2-second ring refilled at task level | Continuous music and zero underruns in the log ⇒ Phase 2 can be built on this. Stutter ⇒ the log's underrun count and KB/s figure say what to change |

Separating Q2 from Q3 is deliberate: if streaming stutters, stage A still settles
the format and proves the engine, so the trip is not wasted and the follow-up is
narrow.

The run also measures and logs **read throughput in KB/s**. CD-DA needs 172 KB/s to
keep up, and the measured figure is what Phase 2 needs in order to size its
read-ahead.

## What it touches

Changes the drive's block size to 2352 and restores it on every exit path,
including the failure paths. Plays audio. Nothing is written to the disc. If the
block size fails to restore, the log says so and the fix is to eject and re-insert.

## Design notes that matter for Phase 2

- **The doubleback proc runs below task level.** It does not allocate, does not
  touch the File Manager, does not log and never waits. On an underrun it emits
  silence and bumps a counter (`reference_os9_no_filemgr_at_interrupt` — this exact
  mistake cost the USB2 work three hardware cycles).
- **Context reaches the interrupt through `dbUserInfo[0]`,** not fragment globals —
  the same discipline the VBL probe used.
- **The ring is single-producer/single-consumer with no shared mutable counter.**
  `writeOff` is written only at task level, `readOff` only at interrupt level, and
  available bytes are derived from the difference, so there is no read-modify-write
  race to lose. Cursors are monotonic longs, good for 2 GB against a few MB of
  playback.
- **Refill uses a lean `PBRead` with no logging.** The shared
  `CDStatusCall`/`CDControlCall` wrappers flush the log on every call, which is
  right for a recon probe and would starve the ring here. Timings are logged
  afterwards instead.
- **Both `compressionID` values are tried** (`notCompressed`, then
  `fixedCompression`) if `SndPlayDoubleBuffer` refuses the first. Which one a
  `SndDoubleBufferHeader2` with `dbhFormat` wants is not worth guessing on a machine
  that costs a trip to reach.
- Reads start **2 seconds into the track** so a silent pregap cannot be mistaken for
  a broken engine.

## Tunables

At the top of `cd_audio_spike.c`: stage lengths, ring size, double-buffer size
(0.25 s), read chunk (32 sectors = 75,264 bytes), pregap skip. If stage B underruns,
those are the dials — bigger ring first, then larger reads.

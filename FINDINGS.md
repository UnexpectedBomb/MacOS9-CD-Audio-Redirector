# Findings — hardware

Phase 0 below, then **[Phase 1](#phase-1-findings--hardware-2026-08-05)** at the end.

# Phase 0 findings — hardware, 2026-08-05

Machine: Mac mini G4, Mac OS 9. Disc: ordinary pressed audio CD, 12 tracks.
Artifacts: `CDRecon_v2`, `CDPlayProbe_v2`. Raw logs: `CD Recon Log`,
`CD Play Probe Log`.

**Bottom line: the gate is passed and the project is GO.** Two of Phase 0's
expected answers came back differently from what the probes' own summary lines
claimed, and both corrections are recorded below.

---

## P4 — THE GATE: PASSED

Route A works exactly as hoped. `ChangeBlockSize(2352)` returned `noErr`, a
driver-level `PBRead` at LBA 750 (track 1 + 10 s) returned all 9408 requested
bytes, and the content is real audio: 9280 of 9408 bytes non-zero.

```
STEP PBRead lba=750 off=1764000 len=9408 drive=4
  PBRead err=0 actCount=9408
  content: nonZeroBytes=9280/9408 peakLE=16450 peakBE=32766
  cdda +0000: DF DF A9 F1 7E F2 9B EA BE FF AE E6 FD FC 06 E7
```

Block size restored to 512 cleanly afterwards.

**⇒ CD-DA is readable straight through the driver. Route B (ATAPI `READ CD` 0xBE
via the ATA Manager) is not needed.** The `'dvrf'` handle (`0x00030100`) is logged
anyway in case a later phase wants it.

### Byte order: confirmed little-endian, and the peak test is the proof

The two peak figures diagnose byte order on their own. Interpreted little-endian
the peak is 16450, a normal music level around half full scale. Interpreted
big-endian it is 32766, i.e. pinned at clipping — the signature of noise, which is
what wrong-endian PCM looks like. Hand-decoding the first samples agrees: LE gives
a smooth waveform (−8225, −3671, −3458, −5477, −66, −6482) while BE gives
implausible jumps (−8225, −22031, +32498, −25622).

**⇒ CD-DA is 16-bit signed LE as expected, so the `'sowt'` plan in REVIEW.md §5
stands and the byte swap FEASIBILITY §3 called mandatory is avoidable.**

## P2 — CORRECTION: it is a classic `DRVR`, not a native `ndrv`

CDRecon's summary line said `NATIVE ndrv`. **That was wrong**, and the probe's own
hex dump disproves it. `GetDriverInformation` returned `noErr` but with degenerate
data — an empty `DriverDescription`, a `flags` value that is just `dCtlFlags`
echoed back, and a unit number trivially derivable from the refNum. It is not a
reliable test of nativeness.

The bytes at `dCtlDriver` are a textbook DRVR header:

```
dCtlDriver +0000: 5D 04 00 78 00 00 00 00 01 14 01 34 01 54 01 74
dCtlDriver +0016: 01 94 08 2E 41 70 70 6C 65 43 44 00 00 00 01 48
```

| Field | Value | Cross-check |
|---|---|---|
| `drvrFlags` | 0x5D04 | |
| `drvrDelay` | 0x0078 = 120 | **matches `dCtlDelay` = 120** |
| `drvrEMask` | 0x0000 | **matches `dCtlEMask` = 0x0000** |
| `drvrMenu` | 0x0000 | **matches `dCtlMenu` = 0** |
| `drvrOpen` | 0x0114 | |
| `drvrPrime` | 0x0134 | |
| **`drvrCtl`** | **0x0154** | **the interception target** |
| `drvrStatus` | 0x0174 | |
| `drvrClose` | 0x0194 | |
| `drvrName` | 0x08 `.AppleCD` | **matches the reported driver name** |
| trailing | 0x0148 | **matches `'vers'` = 0x01488000, i.e. 1.4.8** |

Five independent cross-checks. `dCtlNodeID` is also 0, consistent with classic and
inconsistent with a native driver holding a Name Registry entry.

Driver identity: refNum **−66**, unit 65, name **`.AppleCD`**, `'devt'` = `'cdrm'`,
`'intf'` = **`'atpi'`** (ATAPI), version 1.4.8, drive number 4, `dRAMBased` clear so
`dCtlDriver` is a pointer.

**⇒ Consequences, all favourable:**

1. The interception target is the classic DRVR `Control` entry at
   `dCtlDriver + 0x0154`, not a native dispatch. Simpler, and the eSATA project's
   garbage-UPP hazard does not apply.
2. That entry is **68K code**, so the patch can be 68K — which Retro68 builds
   natively as a code resource. Combined with REVIEW.md §3, CodeWarrior looks
   unnecessary for this project.
3. The classic technique applies: build a DRVR-shaped stub in the system heap whose
   `drvrCtl` offset points at our code, repoint `dCtlDriver` at it, and chain to the
   original absolute entry (`origDriver + 0x154`). Entry points are 16-bit offsets
   from the header base, which is why a new header rather than an edited one.

## P5a — the audio surface: neither H1 nor H2, but "accepted and ignored"

Every legacy audio call **succeeds**, and **nothing happens**.

```
AudioTrackSearch (103)  err=0
AudioPlay        (104)  err=0    posType=0, MSF form
AudioPause       (105)  err=0   (both pause and resume)
AudioStop        (106)  err=0
AudioStatus      (107)  err=0   (as Control)
ReadQ            (101)  err=0   (as Control)
user reported: SILENCE
```

But the reported position **never moved across 10 seconds of polling**:

```
first +0000: 00 00 00 07 18 68 ...
last  +0000: 00 00 00 07 18 68 ...
```

The drive is not transporting. This is a third possibility distinct from both H1
and H2, and it is the one FEASIBILITY §1 hedged with "succeeds or silently
no-ops": **the driver accepts the transport commands, returns `noErr`, and never
executes them.** `AudioTrackSearch` did not even move the head — the position
reported is a leftover from iTunes' earlier use, in track 3, while we asked for
track 1.

**⇒ The fix design is unchanged and confirmed:** intercept the audio csCodes and
service them digitally. P4 says we can get the samples.

**⇒ But status synthesis moves from optional to mandatory.** Any game that polls to
detect track end currently sees a frozen position, so our extension must answer
those polls from its own playback cursor. FEASIBILITY §5 planned for this; it is
now a requirement, not a refinement. It also predicts the community's reported
symptom precisely: music that never advances or loops.

### The driver answers truthfully, it just does not move — and that validates our parsing

The `ReadQ` response is internally consistent *and* consistent with the TOC we
read, which is a rigorous check on the whole decoding path:

```
ReadQ: 00 03 01 00 10 21 07 18 68
       │  │  │  └ rel MSF  └ abs MSF   (all BCD)
       │  │  └ index 1
       │  └ track 3
       └ control/adr: audio, no pre-emphasis
```

Track 3 starts at 07:08:47 per the TOC = frame 32147. Absolute 07:18:68 = frame
32918. The difference is 771 frames, and the reported track-relative position
0:10:21 is exactly 771 frames. **The arithmetic closes to the frame.** That
confirms our TOC parse, our BCD handling and our MSF→LBA maths simultaneously, and
it means the driver is reporting honestly rather than returning garbage.

**Byte layouts to emulate in Phase 3, taken from the real driver:**

- `ReadQ` (101): `[ctrl/adr, track BCD, index BCD, relM, relS, relF, absM, absS, absF]`, MSF in BCD.
- `AudioStatus` (107): `[?, ?, ?, absM, absS, absF, …]` — bytes 3..5 are the absolute
  MSF (they match `ReadQ`'s). Bytes 0..2 were 0x00 throughout; plausibly
  play-state and mode, unconfirmed because the drive never played.

## Status vs Control: settled, and the doc had it backwards

**Every** audio and TOC call is a **Control** call. As Status they return −18
(`statusErr`):

| csCode | as Status | as Control |
|---|---|---|
| 100 ReadTOC | −18 | **0** |
| 101 ReadQ | −18 | **0** |
| 107 AudioStatus | −18 | **0** |
| 112 ReadAudioVolume | −18 | not retried (probe gap) |
| 126 GetPlayMode | −18 | not retried (probe gap) |
| 97 WhoIsThere | −18 | not retried |

Basilisk II's dispatch table, which puts them all under Control, was right. The
*develop* issue 3 split (ReadTOC and AudioStatus as `DStatus` subcalls) describes
the old SCSI AppleCD SC driver and does **not** apply to this ATAPI-era `.AppleCD`.
`cd_cscodes.h` is updated to the hardware-verified truth.

Working ordinary Status calls, for the record: 98 GetBlockSize (**512**, not 2048),
96 GetDriveType (3), 95 Get2KOffset (0), 121 GetCDFeatures (speed 256, features
0xD81D), 120 ReturnDeviceIdent (0x0701), 8 DriveStatus.

**ReadTOC action encoding: the WORD form works** (`csParam[0] = action`), reported
as `enc=word`. The byte-at-offset-0 alternative was never needed.

**AudioPlay address encoding: `posType = 0` with MSF form** was accepted on the
first attempt. Since the drive never executed the command this confirms only what
the driver *accepts*, not what it acts on.

## P3 — CORRECTION: a qualified NO. The `FSRead` shortcut is dead

CDRecon printed `AUDIO TRACK AS A FILE — P3 is GO: DAE may be a plain FSRead`.
**That conclusion was wrong**, and the same log line disproves it. The tracks do
appear as files on a mounted `Audio CD 1` volume, but every one has an empty data
fork:

```
vol 2: 'Audio CD 1' vRefNum=-2 drvNum=4 drvRefNum=-66 sig=0x4244 fsID=19016 files=12
  file 'Track  1' type='trak' creator='hook' dataEOF=0
  … 'Track  2' … 'Track 12', all dataEOF=0
```

`dataEOF = 0` means there is nothing to read. Type `'trak'` / creator `'hook'` are
reference files serviced by QuickTime, not containers of PCM. So audio-track-as-file
does **not** give us DAE for free via the File Manager; that route would mean going
through QuickTime instead.

**⇒ Immaterial, because P4 route A works.** Recorded so nobody re-tries it. Still
open for a genuinely mixed-mode disc: whether an `Audio CD` volume appears *at all*
alongside the HFS data volume. Worth a look when such a disc exists, but no longer
on the critical path.

## P1 — half done

iTunes plays the tracks on this machine: the digital path works, as assumed.

The AppleCD Audio Player half is **moot**. `CDPlayProbe` *is* that experiment — it
drives the same legacy Control path and got silence with a frozen transport. The
`ReadQ` arithmetic closing to the frame shows the probe is talking to the driver
correctly, so an independent cross-check with Apple's app would add little. (It is
on the 9.2.2 install CD at `CD Extras:AppleCD Audio Player:` — not installed by
default, since 9.2.2 ships iTunes 2.0.4 for this job.)

---

## Probe gaps to fix if these run again

- Retry csCodes **112** (ReadAudioVolume) and **126** (GetPlayMode) as Control when
  Status fails. Only 107 and 101 were retried, so the CD volume could never be read
  back — `AudioControl(255,255)` returned `noErr` so the zero-volume confound is
  handled, but unverified.
- CDRecon's P2 summary should classify from the DRVR-header evidence, not from
  `GetDriverInformation`, which returns success for any open driver.
- CDRecon's P3 line should require `dataEOF > 0` before claiming a readable file.

## Where this leaves the project

The gate is passed, and every remaining unknown that Phase 0 was built to answer
has an answer:

| Question | Answer |
|---|---|
| Can we read CD-DA? | **Yes**, driver route A, no ATA Manager needed |
| Byte order | LE confirmed; use `'sowt'`, no swap |
| Interception target | classic DRVR `Control`, `dCtlDriver + 0x0154`, 68K |
| Toolchain | Retro68 throughout, on current evidence |
| H1/H2 | neither: accepted and ignored ⇒ status synthesis mandatory |
| Status vs Control | all audio/TOC calls are Control |
| Formats to emulate | `ReadQ` and `AudioStatus` layouts captured above |

Phase 1 (DAE → Sound Manager spike) is unblocked and can be built and validated on
the mini itself, since P4's read path is proven on that machine.

---

# Phase 1 findings — hardware, 2026-08-05

Machine: Mac mini G4, Mac OS 9. Disc: same pressed audio CD. Artifact:
`CDAudioSpike_v1`. Raw log: `logs/2026-08-05-CDAudioSpike_v1-mini.log`.

**Bottom line: Phase 1 succeeded. DAE → Sound Manager works, streaming sustains in
real time, and no byte swap is needed.** Phase 2 can be built on this engine.

## The engine works (Q2)

```
SndNewChannel(sampledSynth, initStereo) err=0
A1 sowt: SndPlayDoubleBuffer err=0, callbacks=20, delivered=882000/882000, underruns=0
A2 twos: SndPlayDoubleBuffer err=0, callbacks=20, delivered=882000/882000, underruns=0
```

`compressionID = notCompressed` (0) was accepted on the first attempt, so the
`fixedCompression` fallback never fired. Channel setup, 44.1 kHz, stereo and the
interrupt-time doubleback proc are all correct. Block size switched to 2352 and
restored to 512 on the way out.

## Streaming sustains in real time (Q3)

```
pre-roll complete: 352800 bytes, next LBA 300
stage B done: delivered=5292000/5292000 callbacks=120 UNDERRUNS=0
```

Listener: **continuous**, no gaps. 120 callbacks × 11025 frames = exactly 30.0 s of
audio, i.e. the Sound Manager consumed at real time throughout, so sustained read
throughput during playback met or beat the 176,400 B/s CD-DA requires.

**⇒ A 2-second ring with 32-sector (75 KB) reads refilled at task level is
sufficient. Zero underruns over 30 s.**

### ⚠ The logged "106 KB/s" throughput figure is wrong — do not size Phase 2 from it

```
read 882000 of 882000 bytes in 485 ticks = 106 KB/s (CD-DA needs 172 KB/s to keep up)
```

That measurement is broken by design: it times a *cold* five-second read, so drive
spin-up and the initial seek dominate a window far too short to amortise them. It
reports below real time while stage B then streamed 30 s with zero underruns, which
is a flat contradiction — and stage B is the trustworthy half. Any Phase 2
read-ahead sizing needs a **warm, longer** measurement.

## Byte order (Q1): `'sowt'` confirmed — but the test as written did not discriminate

Listener answer: **both arms sounded like music**.

**The v1 experiment was designed wrong.** A1 played little-endian bytes tagged
`'sowt'`; A2 played *byte-swapped* bytes tagged `'twos'`. That is two changes which
cancel: both arms were internally consistent, so if the Sound Manager honours
`dbhFormat` both play correctly. The window nevertheless told the listener that one
arm had to be noise. A test whose arms are both valid cannot reveal which the system
prefers.

The answer still resolves, because only one case produces "both":

| Heard | Meaning |
|---|---|
| **Both music** | **`dbhFormat` is honoured** — both arms were matched |
| Second only | tag ignored, big-endian assumed |
| First only | tag ignored, little-endian assumed |
| Neither | channel or output routing broken |

Had `dbhFormat` been ignored, A1's little-endian bytes read as big-endian would have
been unmistakable noise. They were not.

**⇒ `'sowt'` works. CD-DA goes to the Sound Manager exactly as it comes off the
disc, the doubleback proc stays a plain copy, and no per-sample byte swapping
happens at interrupt time. REVIEW.md §5 holds and FEASIBILITY §3's "mandatory" swap
is confirmed unnecessary.**

**Fixed for any future run:** A2 now plays the *same untouched little-endian bytes*
deliberately mislabelled `'twos'`. One matched arm, one mismatched arm, so "first
only" means the tag is honoured and every other answer means something distinct. The
byte-swap helper is gone entirely, so no dead code implies Phase 2 might need one.

## Bug found and fixed: a second `CDLogOpen` silently discarded the log

The listener verdicts were never written. `main()` called `CDLogOpen` again before
recording them, on the mistaken belief the log had been closed — it had only been
flushed. A second `FSpOpenDF` on a file already open for writing returns `opWrErr`,
the old refNum leaked, `gLogRef` was left at 0, and every subsequent `CDLogf`
vanished without a word. The verdicts are the entire reason the run existed.

CDPlayProbe did not have that extra call, which is why its verdict recorded fine in
Phase 0. Fixed twice over: the redundant call is gone, and `CDLogOpen` is now
idempotent so this class of mistake cannot silently discard a log again.

## Where this leaves the project

| Question | Answer |
|---|---|
| Does DAE → Sound Manager work end to end? | **Yes**, audible on the mini |
| Byte swap needed? | **No** — `'sowt'` honoured |
| `compressionID` | `notCompressed` (0) |
| Ring size sufficient? | **2 s**, 32-sector reads, 0 underruns over 30 s |
| Sustained throughput | ≥ real time (the 106 KB/s figure is a cold-read artifact) |
| Interception target (Phase 0) | classic DRVR `Control`, `dCtlDriver + 0x0154`, 68K |

**Phase 2 is unblocked**: the INIT that patches the CD driver's Control entry,
catches `AudioPlay`, and routes it to this engine. Note from Phase 0 that status
synthesis is mandatory — the driver's own `AudioStatus`/`ReadQ` report a frozen
position, so the extension must answer those polls from its own playback cursor
using the byte layouts captured above.

---

# Phase 2 pre-work findings — hardware, 2026-08-05

Machine: Mac mini G4, Mac OS 9. Artifact: `CDCtlDump_v1` (read-only). Raw log:
`logs/2026-08-05-CDCtlDump_v1-mini.log`.

**The blocking question is answered, and the answer is a third possibility neither
FEASIBILITY.md nor PHASE2.md anticipated: `.AppleCD`'s entry points are not 68K code
and not stubs jumping to 68K code. They are Mixed Mode routine descriptors wrapping
native PowerPC routines.**

## All five DRVR entries are `0xAAFE` routine descriptors

`GetPtrSize(dCtlDriver)` = **476 bytes**, `MemError` = 0. That alone says the block
is a shell, not a driver — a real ATAPI CD driver is many KB. The contents confirm
it: DRVR header, some 68K glue, then five 32-byte routine descriptors, then a name.

Reconstructed at their true offsets:

| Entry | Offset | `procDescriptor` (TVector) |
|---|---|---|
| Open | 0x114 | 0x017678F8 |
| Prime | 0x134 | 0x017678F0 |
| **Control** | **0x154** | **0x017678E8** |
| Status | 0x174 | 0x017678E0 |
| Close | 0x194 | 0x017678D8 |

Every one has the identical shape:

```
+00: AA FE 07 00 00 00 00 00 00 00 00 00 00 17 98 22
+10: 00 01 00 04 01 76 78 E8 00 00 00 00 00 00 00 00
     │  │     │  └ procDescriptor = the PPC TVector
     │  │     └ routineFlags = 0x0004
     │  └ ISA = 0x01 = kPowerPCISA
     └ (procInfo 0x00179822 ends here)
```

- `0xAAFE` = `_MixedModeDispatch`, the routine-descriptor magic.
- version 7, routineCount 0 (one routine).
- **`procInfo` = 0x00179822.** Low nibble 2 = **`kRegisterBased`**, result size code 2
  = a word, result register D0 — exactly the classic DRVR contract (A0 = ParmBlkPtr,
  A1 = DCtlPtr in, OSErr in D0 out), expressed so Mixed Mode can marshal it.
- **`ISA` = 1 = PowerPC.** The implementations are native.
- The five TVectors are 8 bytes apart in a contiguous array (0x017678D8…F8), in
  reverse entry order — a TVector table, as expected for a CFM fragment's exports.

Also recorded: a 5-character Pascal name (`"AZBay"`) near 0x1B4, and `dCtlFlags`
0x7D24 decoded — `dReadEnable`, `dCtlEnable`, `dStatEnable`, `dNeedGoodbye`,
**`dNeedTime`**, `dNeedLock` all set, `dRAMBased` clear. `dCtlDelay` = 120 ticks.
`dNeedTime` being set confirms the driver already receives periodic `accRun`
(csCode 65) Control calls at task level, which PHASE2.md §2 wanted as the refill pump.

## What this settles — the good news

**PHASE2.md §3's blocking question dissolves in the best possible way.** The Control
entry is neither an RTS-style 68K routine nor a `jIODone` jumper: it is a **callable
routine descriptor that returns normally**. So a patch can:

1. chain to the original by its address, and
2. **inspect and rewrite `csParam` after it returns** — which is exactly what
   synthesising `AudioStatus` and `ReadQ` requires, and Phase 0 proved that
   synthesis is mandatory.

Better still, we do not have to *construct* a descriptor to chain: the original's
descriptor address is reused as-is, and calling it from either 68K (set A0/A1, JSR —
the `0xAAFE` word traps into Mixed Mode) or PowerPC (`CallUniversalProc` with
procInfo 0x00179822) works.

It also refines, without reversing, the Phase 0 conclusion. The **driver structure
is** a classic DRVR — the header cross-checked five ways and `drvrCtl` = 0x154 is
where the Control entry lives. What is *not* 68K is the code behind it. "Patch the
DRVR Control entry" remains correct; "the entry is 68K code" was wrong.

## ⚠ What this refines — and one error of mine, corrected

PHASE2.md §2 chose an **all-68K** engine on the reasoning that the entry we patch is
68K code so the patch must be 68K anyway. **That premise is false** — the entry is a
PPC routine descriptor. The conclusion survives for better reasons (trivial residency
for a 68K blob, and no descriptor construction needed to chain), but the reasoning is
corrected in PHASE2.md §2.

I then compounded it. Having found the entry was PowerPC, I checked whether a 68K
build was possible, looked only in `~/Retro68-build/toolchain/` and its `bin/`, found
only `powerpc-apple-macos`, and concluded **the installation was PowerPC-only and no
INIT of any kind could be built** — escalating a multi-hour toolchain build to the
user as an open decision.

**That was wrong. The m68k toolchain was already installed**, at
`~/Retro68-build/toolchain-m68k/`, complete with `m68k-apple-macos-gcc` 12.2.0, its
own CMake toolchain file, and a `build-68k.log` ending "Done building Retro68." It was
listed in a directory listing I had already printed and did not read carefully.

Verified by building Retro68's own `Samples/SystemExtension` end to end: a real
`'INIT' (128, locked)` resource, 53 KB MacBinary. **FEASIBILITY §2's intended artifact
is buildable today at no toolchain cost**, and PHASE2.md §7 is resolved rather than
open. Pipeline and residency details are recorded there.

## Probe bugs found and fixed

- **`GetPtrSize` floor rejected the correct answer.** The check required
  `sz >= 0x200`; the real size was 476, so the probe fell back to a 1536-byte window
  and read ~1060 bytes past the end of the allocation. It did not fault only because
  the neighbouring heap was mapped — and that guard existed specifically to prevent
  the over-read. Floor lowered to 0x80. Everything in the `drvr` dump beyond
  +0x1DC is neighbouring heap, not driver content.
- **`CDLogHex` restarted its offset at 0 on every call**, and `DumpCode` fed it 16
  bytes at a time, so all 96 lines of the driver dump printed `+0000` and the true
  offsets had to be reconstructed by counting lines. Added `CDLogHexAt`, and offsets
  now print in hex rather than decimal.
- **The jump decoder did not know `0xAAFE`**, so it reported "not a recognised jump ⇒
  the Control code appears to start here", which was actively misleading. It now
  recognises and decodes routine descriptors, printing procInfo, ISA and TVector.

---

# Phase 2a findings — hardware, 2026-08-05

Artifacts: `CDPatch2a` (extension) + `CDPatchInstall` + `CDTraceDump_v1`.
Logs: `logs/2026-08-05-CDPatch2a-boot2-mini.log`,
`logs/2026-08-05-CDTraceDump-boot2-mini.log`.

**2a WORKS. The patch installs, is called, and passthrough is transparent enough that
the machine and CDRecon carried on normally.** 126 Control calls captured. Two real
defects found, both mine, and one design premise corrected.

## The interception mechanism is proven

```
=== PATCH IS INSTALLED ===
  version=1  origDriver=0x00FFE02E  ring=0x01599390  ringEntries=512
  callCount=126  audioCallCount=3  writeCount=126
```

- The **revised passive unit-table name scan works at INIT time** — `CD Patch 2a:
  INSTALLED` at boot, where v1's drive-queue scan had found nothing.
- `dCtlDriver` points at our shell, and the shell's stubs are correct: four tail-jump
  into the original, only Control comes to us.
- The `already patched` guard fired correctly when `CDPatchInstall` ran afterwards.
- The machine stayed up, `CDRecon` read the TOC through the patch (its
  `ChangeBlockSize` and `ReadTOC` calls are visible in the trace), and the drive
  mounted.

## ★ The Phase 2b answer: the calls we must synthesise are QUEUED

106 immediate, 20 queued — and *which* ones matters far more than the ratio:

| csCode | count | trap | |
|---|---|---|---|
| 65 accRun | 103 | 0xA204 | IMMEDIATE |
| 100 ReadTOC | 14 | 0xA004 | queued |
| 79 ChangeBlockSize | 2 | 0xA004 | queued |
| 43 DriverGestalt | 2 | 0xA204 | IMMEDIATE |
| 22 GetMediaIcon | 2 | 0xA004 | queued |
| 109 AudioControl | 1 | 0xA204 | IMMEDIATE |
| **107 AudioStatus** | 1 | 0xA004 | **queued** |
| **101 ReadQ** | 1 | 0xA004 | **queued** |

`AudioStatus` and `ReadQ` — the two calls Phase 0 proved we *must* synthesise — arrive
**queued**. So the original ends at `jIODone` and never returns to us, and 2b cannot
call it and then rewrite `csParam`.

**This is fine, and it resolves cleanly.** The codes we need to rewrite are exactly the
codes we do not need the original's answer for: 2b answers `AudioStatus` and `ReadQ`
from its own playback cursor. So it never chains for those — it completes the request
itself, the way `libDRVRRuntime` does: result in D0, then push `jIODone` and `RTS` when
`noQueueBit` is clear, plain `RTS` when it is set. Both endings must be implemented,
because `AudioControl` came through immediate while `AudioStatus` came through queued.

## ⚠ Defect 1: renaming the shell broke iTunes

v1 gave the shell its own name, `.CDAudio`. On hardware the unit table then read:

```
  unit 65 refNum -66  '.CDAudio'
```

where it had read `.AppleCD` before, and **iTunes could no longer recognise an audio CD
it had read fine earlier in the project**. Anything that finds this driver by name —
`OpenDriver`, Audio CD Access, the mounting machinery — has to keep finding it. We are
impersonating the driver, so we have to look exactly like it.

**Fixed:** the shell now copies the original's name byte for byte. The rename was
gratuitous and there was never a reason for it.

## ⚠ Defect 2: the INIT patched a DIFFERENT driver than the one Phase 0 characterised

This is the more important finding. The saved original was:

```
  origDriver=0x00FFE02E
  stubs +0020: 4E F9 00 FF E1 2E  4E F9 00 FF E0 5E  4E F9 01 5B 62 10
  stubs +0030: 4E F9 00 FF E0 6E  4E F9 00 FF E1 34
```

Those tail-jump targets imply entry offsets of **0x100 (open), 0x30 (prime), 0x40
(status), 0x106 (close)** from `0x00FFE02E`. The ATAPI `.AppleCD` that Phase 0 and the
Phase-2 dump characterised has offsets **0x114 / 0x134 / 0x154 / 0x174 / 0x194**, each
entry a `0xAAFE` Mixed Mode descriptor wrapping native PowerPC, and lived at a RAM
address around `0x0164xxxx`.

So during the extension parade, unit 65's `dCtlDriver` pointed at an **earlier,
different incarnation** of the driver — a different shape at a different address — and
the INIT patched that. Everything still worked, because our passthrough is a faithful
tail jump, but we were not intercepting the driver the whole design was built against.
It is also a plausible contributor to the iTunes regression alongside the rename.

**Fixed, and turned into a decision the code makes for itself:** the install now
requires the original's Control entry to begin with `0xAAFE`. That is the unmistakable
signature of the driver we characterised. At INIT-parade time the check fails and the
install cleanly declines (`kInstallNotATAPIDriver`); post-boot it succeeds. No more
"did we happen to run late enough?".

**⇒ Consequence for the vehicle:** an INIT runs too early to catch the real driver. The
patch should be installed **post-boot** — from `CDPatchInstall` for testing, and from a
faceless Startup-Items app for shipping. That is the same conclusion the USB2 work
reached for its own reasons (`reference_os9_init_resident_driver`: the shippable auto-run
vehicle is a top-level process, not an INIT), now reached here on direct evidence. The
INIT stays in the build as a harmless no-op that declines politely, and as the vehicle
to revisit if the driver ever turns out to be patchable that early.

## Minor

`CDPatchInstall` logs the blob's address as `0x` followed by a **decimal** number
(`AppendNum` is decimal). Cosmetic, but misleading in a log; worth fixing next time the
file is touched.

## Causation confirmed, and a less invasive design to consider

**Confirmed 2026-08-05:** removing the extension and rebooting restored iTunes' ability
to read an audio CD. So the patch was the cause, not the disc and not a coincidence.
Two rebuilds address the two known defects (name preserved; the install now requires the
real ATAPI driver), but neither has been tested for coexistence yet.

### The deeper issue: swapping `dCtlDriver` changes the driver's identity

Every problem in this episode traces to one design choice — replacing `dCtlDriver` with
a shell of our own. That makes our block *become* the driver as far as the rest of the
system is concerned, so:

- its name is what name-based lookups find (broke Audio CD Access / iTunes);
- its address is what anything caching `dCtlDriver` sees;
- and whatever would later have installed the real ATAPI driver is either blocked by us
  or silently clobbers us.

### The alternative: patch one field inside the existing descriptor

The Control entry at `origBase + 0x154` is a 32-byte Mixed Mode routine descriptor whose
`procDescriptor` field (descriptor + 0x10) holds the PPC TVector `0x017678E8`. That field
alone could be repointed at our own routine, leaving the DRVR header, the name, the
address and `dCtlDriver` **completely untouched**. The entire class of
"we changed the driver's identity" failures disappears, and uninstall becomes a 4-byte
write of the saved value.

Trade-offs, not yet resolved:

- **Atomicity favours it.** Repointing one aligned 4-byte field is effectively atomic,
  whereas building a shell and swapping `dCtlDriver` is also a single aligned write but
  leaves a *semantically* half-patched driver visible (ours) for the rest of the session.
- **ISA is the catch.** The descriptor says `ISA = kPowerPCISA`. Pointing it at 68K code
  means also flipping the ISA byte, and no ordering of those two writes is safe — either
  window has a moment where a call would execute code under the wrong architecture.
  Keeping `ISA = PowerPC` means our handler must be a **PPC** routine, which needs the
  `InstallDriverFromMemory` residency route — more machinery, but it is proven on this
  project (USB2 R2b-3), and Phase 1's engine is already PowerPC, so it is *closer* to the
  working code than a 68K rewrite is.
- **Chaining gets easier**, not harder: from PPC, calling the saved original TVector is
  an ordinary indirect call. From 68K we would JSR a private *copy* of the original
  32-byte descriptor, which also works since a descriptor is self-contained.
- **Open question for either route:** who performs `jIODone` for queued requests. The
  original PPC routine evidently does it itself today (queued Control calls work), so
  chaining is fine — but for the codes we answer ourselves (`AudioStatus`, `ReadQ`, both
  observed queued) we must complete the request, and doing that from PPC needs a callable
  path to `jIODone` rather than the 68K push-and-RTS trick.

**Decision gate:** if the rebuilt shell-swap patch coexists cleanly with iTunes and Audio
CD Access, it is good enough and 2b proceeds on it. If iTunes still breaks even with the
name preserved and the real ATAPI driver targeted, then interposing at the `dCtlDriver`
level is too invasive and the descriptor-field patch becomes the design.

## The decisive log, 2026-08-05 — and two corrections to my own claims

`CD Patch Log`, four `CDPatchInstall` runs across three boots.

### Correction 1: the dry-run build never ran

The log contains **zero** dry-run output — no `DRY RUN` banner, no `entry ctl`, no
`first word`, no `header 0x00`. The grey-screen run was an **older build** still sitting
on the OS 9 machine.

That is my process failure, not the user's. `feedback_version_stamp_installed_artifacts`
exists precisely for this, and every other artifact in the project honours it —
`CDRecon_v2`, `CDPlayProbe_v2`, `CDAudioSpike_v1`, `CDTraceDump_v1`, `CDCtlDump_v1` —
while I shipped this one as plain `CDPatchInstall`, four times, with no way for anyone to
tell which build was on the machine. **Everything from here carries a version in its
name.**

### Correction 2: we HAD patched the ATAPI driver — three times

I said the real ATAPI driver had never been successfully patched. Wrong. Three of the
four runs show `unit 65 refNum -66 '.AppleCD'` immediately before
`install result: INSTALLED`:

| run | unit 65 before | loader | result | session ended |
|---|---|---|---|---|
| A | `.AppleCD` | pre-`NewPtrSys` (no "blob copied" line) | **INSTALLED** | MacsBug |
| B | `.CDAudio` | `NewPtrSys` | already patched (**changed nothing**) | fine |
| C | `.AppleCD` | `NewPtrSys` | **INSTALLED** | MacsBug |
| D | `.AppleCD` | `NewPtrSys` | **INSTALLED** | grey screen |

Runs C and D both used the fixed `NewPtrSys` loader, so neither crash is explained by the
`preload` bug. The install *itself* always succeeds; the machine dies when the patched
driver is first **used** — which, thanks to the alert pumping `SystemTask`, is seconds
later via `accRun`.

### ⇒ The real conclusion, now well evidenced

| interposition target | attempts | outcome |
|---|---|---|
| the **classic** early driver (parade, `0x00FFE02E`, plain 68K entries) | 1 | worked, **126 calls traced**, machine stable |
| the **ATAPI** driver (`0x0164xxxx`, `0xAAFE` Mixed Mode descriptors) | 3 | **crashed every time** |

**A 68K shell plus tail-jump works on a classic DRVR and fails on this
Mixed-Mode-descriptor driver.** Not a coincidence at 3/3, and it identifies the mechanism:

- Unpatched, the Device Manager JSRs the descriptor directly, and Mixed Mode performs
  exactly one 68K→PPC transition.
- Patched, the DM JSRs our 68K stub, which jumps to our 68K shim, which calls a 68K C
  function, and then `RTS`es into the descriptor. **We inserted an extra 68K↔PPC boundary
  that did not exist before**, and did it inside a driver entry with a register-based
  calling convention.

The TVector-field patch in PHASE2.md §8 inserts **no new boundary at all**: the DM still
JSRs the same descriptor, Mixed Mode performs the same single transition, and it lands in
our PowerPC routine instead of Apple's. That is the minimal possible intervention, and it
removes precisely the mechanism that is crashing. What was a reasonable-looking
alternative is now the evidence-backed design.

---

# Step 2 findings — hardware, 2026-08-05

Artifact: `CDEngineInstall_v1`. Log: `logs/2026-08-05-CDEngineInstall_v1-step2-run1-mini.log`.

**The all-PowerPC architecture works. Residency is proven. And the validation guard
caught a transcription bug of mine before anything was written — which is the whole
reason Step 2 exists as a separate step.**

## Proven

```
PEF at 0x3EA24EC0, 4789 bytes
GetDriverMemoryFragment err=0 connID=0x00000ADF main=0x017E5C4C desc=0x017E5BB0
SetDriverClosureMemory err=0
DoDriverIO returned 0
magic=0x43444531 version=1
cdRefNum=-66  dCtlDriver=0x01621DB0  ctlDescriptor=0x01621F04
```

- **The PEF is accepted.** `GetDriverMemoryFragment` returned 0 with a real connID and
  main, so the fragment's exports and its patched `main` are correct.
- **Residency is proven.** `SetDriverClosureMemory(connID, true)` returned 0, so the
  code outlives the application — without `InstallDriverFromMemory` and without a
  `RegEntryID` this driver does not have.
- **Our PowerPC code ran.** `DoDriverIO` returned 0 and filled the info block, magic
  `'CDE1'` intact.
- **Discovery works.** refNum −66 found by passive name scan, and
  `0x01621F04 − 0x01621DB0 = 0x154`, exactly the `drvrCtl` offset characterised in
  Phase 0.

Every previous approach failed before reaching this point. Nothing was modified.

## ⚠ The bug the guard caught: descriptor offsets off by four

```
descriptor: rdVersion=0x07 procInfo=0x00000000 ISA=0x17
⇒ DO NOT PROCEED to Step 3: descriptor ISA is not PowerPC
```

`rdVersion = 0x07` is right, but `procInfo` should be `0x00179822` and `ISA` should be
`0x01`. I had transcribed the offsets as procInfo +0x08, ISA +0x0D, procDescriptor
+0x10. The true layout, from `MixedMode.h:198` and `:177`, is **procInfo +0x0C, ISA
+0x11, procDescriptor +0x14** — 32 bytes total, which is exactly why `.AppleCD`'s five
entries sit 0x20 apart.

The log's own numbers prove the correction: reading "ISA" at +0x0D returned **0x17**,
which is the second byte of procInfo (0x00**17**9822), and reading "procInfo" at +0x08
returned 0, which is `reserved2`/`selectorInfo`/`routineCount`. Both wrong values are
exactly what the wrong offsets would produce.

**This is the guard earning its place.** Had the ISA check not been there, Step 3 would
have written a TVector at +0x10, landing across `reserved1`, `ISA` and `routineFlags` —
corrupting the descriptor the Device Manager uses on every CD Control call, with a crash
as the best case and silent misbehaviour as the worst. The reason Step 2 patches nothing
is precisely so a mistake at this stage costs a log line instead of a session.

**Fixed by removing the failure mode, not the numbers:** the engine now uses
`MixedMode.h`'s real `RoutineDescriptorPtr` and reads
`rd->routineRecords[0].ISA` / `.procDescriptor`, so an offset cannot be mis-transcribed
again. Step 3's write becomes one typed assignment. `CDCtlDump`'s decoder had the same
four-byte slip in code that had never been exercised; corrected too.

## What Step 2's next run should show

`procInfo = 0x00179822`, `ISA = 0x01`, a non-zero original TVector with plausible code
and TOC words, our own TVector likewise, a non-zero ring, and
`⇒ STEP 3 IS SAFE TO ATTEMPT`.

## Step 2 run 2 — validation passes, and the TVector sanity check earns its place

```
GetDriverMemoryFragment err=0 connID=0x000009BB main=0x018743DC desc=0x01874340
SetDriverClosureMemory err=0
status=0 (OK - descriptor validated)
cdRefNum=-66  dCtlDriver=0x01622070  ctlDescriptor=0x016221C4
descriptor: rdVersion=0x07 procInfo=0x00179822 ISA=0x01
ORIGINAL TVector = 0x01758A58  -> code=0x01755950 toc=0x01760880
OUR      TVector = 0x018743D0  -> code=0x3DB2355C toc=0x018743F0
ring=0x018BB520 entries=512
⇒ STEP 3 IS SAFE TO ATTEMPT
```

The descriptor now reads exactly as characterised: **`procInfo = 0x00179822`, `ISA = 0x01`**,
`ctlDescriptor − dCtlDriver = 0x154`. The typed `RoutineDescriptorPtr` access fixed the
off-by-four, and the original TVector's two words are plausible and self-consistent
(code `0x01755950`, TOC `0x01760880`, both in the same region).

### ⚠ But our own code was in the APPLICATION heap

The engine said "safe to proceed" and it was wrong, because I had not thought to check
*where our own code lives*. The instrumentation that reads both TVector words is what
exposed it:

```
PEF resource at 0x3DB234E0
OUR TVector   = 0x018743D0 -> code=0x3DB2355C  toc=0x018743F0
```

`0x3DB2355C` is `0x3DB234E0 + 0x7C` — **inside the PEF resource handle**, i.e. the
application's heap. Only the data section (TVectors, TOC, at `0x0187xxxx` alongside
`main` and `desc`) was copied into CFM-owned memory.

That is normal behaviour: `GetDriverMemoryFragment` prepares a fragment from the memory
you hand it and uses the PEF's **code section in place** — that is the point of preparing
from memory rather than from disk. And `SetDriverClosureMemory(connID, true)` returning 0
does not relocate a section CFM never copied; it holds the closure, not somebody else's
buffer.

**So Step 3 would have installed a Control handler whose code is freed the moment the
installer quits** — the same fate as the 68K `preload` bug, reached by a different route.
This is the third time this project has been bitten by letting another allocator decide
which heap resident code lives in (`preload` → app heap; `GetDriverMemoryFragment`
in-place code → app heap). The lesson generalises: **copy it into `NewPtrSys` yourself,
then hand that over.**

**Two fixes:**

1. The installer now `NewPtrSys`-copies the PEF and prepares the fragment **from the
   copy**, releasing the resource immediately so nothing ties the resident code to the
   app.
2. The engine now **refuses** with `kEngineCodeInAppHeap` if its own code address falls
   inside the application zone (via the `ApplZone` low-memory global, since
   `ApplicZone()` is absent from this toolchain's PowerPC import libs). A guard that
   catches this class rather than relying on me remembering.

### What run 3 should show

`code=` for our TVector sitting near the system-heap PEF copy (in the `0x016`–`0x018`
range like everything else CFM-owned), **not** near the PEF resource handle, and
`⇒ STEP 3 IS SAFE TO ATTEMPT`. That is the last thing standing between here and the
one-line patch.

## Step 2 run 3 — PASSED. All Step-3 prerequisites verified

```
PEF resource at 0x3DB23510, 4789 bytes
PEF copied to the SYSTEM heap at 0x018A7770
GetDriverMemoryFragment err=0 connID=0x000009BB main=0x0187EE6C
SetDriverClosureMemory err=0
status=0 (OK - descriptor validated)
cdRefNum=-66  dCtlDriver=0x0161FFD0  ctlDescriptor=0x01620124
descriptor: rdVersion=0x07 procInfo=0x00179822 ISA=0x01
ORIGINAL TVector = 0x01766DE8 -> code=0x01763CE0 toc=0x0176EC10
OUR      TVector = 0x0187EE60 -> code=0x018A77EC toc=0x0187EE80
ring=0x018C12F0 entries=512  patched=0
⇒ STEP 3 IS SAFE TO ATTEMPT
```

`0x018A77EC` is `0x018A7770 + 0x7C` — our code is inside the **system-heap** PEF copy, not
the resource handle at `0x3DB23510`, and the application-zone guard passed.
`ctlDescriptor − dCtlDriver = 0x154`, matching Phase 0 exactly.

Everything Step 3 depends on is now established on hardware: the fragment is accepted,
the closure is held, the code is in the system heap, the descriptor is located and reads
exactly as characterised, both TVectors are real and self-consistent, and the ring is
allocated. Three runs, nothing modified, two defects caught.

# Step 3 — the one-field patch (built 2026-08-05, not yet run)

`CDEngineInstall_v2`. Modifier keys, with the harmless action as the default:

| launch | action |
|---|---|
| plain | validate only, exactly as Step 2 |
| **option** | **patch**: write our TVector into `procDescriptor` |
| **shift** | unpatch: write the saved TVector back |

The patch is one aligned 4-byte store,
`gCtlRD->routineRecords[0].procDescriptor = (ProcPtr)CDEngineControl`, through
`MixedMode.h`'s real struct. `procInfo` and `ISA` are untouched, so Mixed Mode performs
exactly the transition it performs today and simply lands in our routine.

Safety properties that make this different from every earlier attempt:

- **The default action is still validation.** Patching needs a deliberate keypress. The
  two defects Step 2 caught would each have corrupted the machine had the default been
  "modify".
- **Everything is re-validated immediately before the store** — magic, ISA, and that
  `procDescriptor` still holds the value we saved — because the driver could have been
  reloaded since init.
- **The value is read back after writing** and reported, so the log states what is
  actually in the driver rather than what we intended.
- **The patch never survives a restart.** Nothing is installed in the Extensions folder
  and nothing persists, so recovery is always simply a reboot. That is a much better
  position than the extension generation, where a bad patch came back on every boot.
- **`patch/` is retired**, not deleted: it holds the 126-call trace and the evidence that
  the 68K shell works on a classic DRVR.
- The handler reads Ticks from the low-memory global rather than calling `TickCount()`,
  because it may run at interrupt time and a plain aligned read is safer there than a
  trap.
- Patch/unpatch state lives in the **engine's** globals, never in the installer's info
  block — that block is an application global and dies when the app quits.

**No trace reader yet, deliberately.** The two questions this run answers are "does it
crash" and "does iTunes still work". Reading the ring needs a publication channel for the
engine's globals, and that is worth building only once the patch is known to be safe.

## Step 3 run 1 — THE PATCH WORKS

```
=== OPTION HELD: patching the Control descriptor ===
PEF copied to the SYSTEM heap at 0x018C45D0
GetDriverMemoryFragment err=0 connID=0x00000AE4 main=0x018A8BFC
SetDriverClosureMemory err=0
status=0 (OK - descriptor validated)
ORIGINAL TVector = 0x01766DE8 -> code=0x01763CE0 toc=0x0176EC10
OUR      TVector = 0x018A8BF0 -> code=0x018C464C toc=0x018A8C08
patch returned 0, status=0, patched=1
⇒ PATCHED, and read back correct. The Control descriptor now
  points at 0x018A8BF0 instead of 0x01766DE8.
```

**The interposition succeeded and the machine stayed up.** First time in the project that
the real ATAPI `.AppleCD` has been interposed without a crash. Details worth keeping:

- `patched=1` and the read-back matched, so the descriptor genuinely holds our TVector.
- Our code at `0x018C464C` is `0x018C45D0 + 0x7C`, inside the system-heap PEF copy.
- The app **ran to completion and quit normally**. That matters: a modal alert pumps
  `SystemTask`, which delivers `accRun` (csCode 65) to this driver, so our handler was
  called — repeatedly — within milliseconds of the patch, and chained correctly.

### Why this worked where three 68K attempts crashed

The 68K shell inserted a second 68K↔PPC boundary into a driver entry that previously took
exactly one Mixed Mode transition. This build inserts none: the Device Manager still JSRs
the same descriptor with the same `procInfo` and `ISA`, Mixed Mode performs the same single
transition, and it lands in our PowerPC routine. Chaining is an ordinary indirect call
through the saved TVector. The DRVR header, the driver's name, its address and
`dCtlDriver` were never touched.

### ⚠ Still outstanding: the coexistence test

Not yet run. **Does iTunes still read the audio CD, and does `CDRecon_v2` still show the
`Audio CD 1` volume, with the patch live?** That is what killed the 68K generation, and it
must be tested in the same boot session — the patch does not survive a restart.

## Step 3 coexistence test — PASSED

With the patch live: **iTunes still reads the audio CD**, and `CDRecon_v2` still sees the
disc's volume. This is the test that killed the 68K generation, and it passes.

The user noted the volume now shows the album's real name rather than `Audio CD 1`.
**That is almost certainly not our doing** and should not be recorded as an effect of the
patch: our handler traces and chains verbatim without touching `csParam`, so it cannot
influence naming. What changed is that iTunes identified the disc earlier in the session
and cached its title in the CD Remote Programs database, which Audio CD Access then uses
in place of the generic name.

**⇒ Interposition on the ATAPI `.AppleCD` is now proven safe:** the patch installs, the
handler is called, calls chain correctly, and the OS's own CD stack — Audio CD Access,
volume mounting, iTunes — carries on unaffected. That is everything Phase 2a set out to
establish, finally achieved on the architecture that inserts no new Mixed Mode boundary.

# Step 4 — the trace reader (built 2026-08-05, not yet run)

`CDTraceRead_v1`, plus engine changes.

The 68K reader found the ring by following the patched `dCtlDriver`. That anchor is gone
**by design**: the new patch leaves `dCtlDriver`, the DRVR header and the driver's name
completely alone. Nothing about the driver reveals us any more — which is exactly why
iTunes kept working, and exactly why a publication channel was needed.

So the engine now allocates one system-heap `CDEnginePublic` block and publishes its
address with **`SetGestaltValue('CDau', addr)`**. Value-based deliberately: `NewGestalt`
takes a selector *function*, which would mean handing the native Gestalt Manager a
callback to invoke; a plain long needs no UPP and no callback. `SetGestaltValue` is
create-or-replace, so installing twice in one boot repoints the selector instead of
failing.

The counters live in that block rather than in the engine's statics, so a reader sees live
values, and the handler still only does plain stores — `writeCount` incremented **last**,
so a reader never sees a slot count that outruns its contents.

What the reader reports: `patched`, `callCount`, `audioCallCount`, both TVectors, and the
ring decoded oldest-first with csCode names and immediate-vs-queued per entry. It warns
explicitly when `patched=0` or `callCount=0`, since after any reboot the expected state is
"engine not resident" — the patch never persists.

## Step 4 run 1 — the patch works, the publication did not

```
CDEngineInstall_v2:  patch returned 0, status=0, patched=1
                     ⇒ PATCHED, and read back correct
CDTraceRead_v1:      Gestalt err=-5551  value=0x00000000
```

−5551 is `gestaltUndefSelectorErr`: nothing ever registered `'CDau'`. **The patch itself
succeeded again** — this is purely a publication failure.

**And I made it undiagnosable.** The engine's registration call was written as

```c
(void)SetGestaltValue(kEnginePublicSelector, (long)gPub);
```

so when it failed there was nothing in any log to say why. Discarding a return value from
a call that can fail silently is the same mistake pattern as the `CDLogOpen` reopen that
ate the Phase-1 verdicts. All three symbols (`NewGestaltValue`, `ReplaceGestaltValue`,
`SetGestaltValue`) are genuinely present in InterfaceLib, so the call happened and
returned an error I threw away.

**Two fixes rather than a guess about which call OS 9 wants:**

1. **Try all three registration entry points in turn and record every result** in the info
   block, so the log states exactly which one OS 9 accepts for a brand-new value selector.
   `NewGestaltValue` first (the documented way to *install* one), then
   `ReplaceGestaltValue`, then `SetGestaltValue`.
2. **A fallback publication channel that involves no OS mechanism at all**: the installer
   writes `CD Engine State` into the System Folder — four bytes of magic, then the 4-byte
   address of the public block — and the reader falls back to it. A stale file after a
   reboot is harmless: the reader validates the *block's own* magic before trusting it,
   and on OS 9 all RAM is readable so a wrong address cannot fault.

The next run therefore produces the trace regardless of whether Gestalt cooperates, and
tells us which registration call works for future use.

## Step 4 run 2 — PASSED. The handler is live, and the ring is readable

```
Gestalt registration: NewGestaltValue=0 ReplaceGestaltValue=1 SetGestaltValue=1
⇒ 'CDau' IS published
CDTraceRead: Gestalt err=0 value=0x0182A590
  version=1  patched=1  cdRefNum=-66
  origTVector=0x01854D00  ourTVector=0x01861B80
  callCount=3  audioCallCount=0  writeCount=3
  0  t=12723  cs=65  accRun  ioTrap=0xA204 IMMEDIATE
  1  t=12843  cs=65  accRun  ioTrap=0xA204 IMMEDIATE
  2  t=12965  cs=65  accRun  ioTrap=0xA204 IMMEDIATE
```

**`NewGestaltValue` is the call that works** for installing a brand-new value selector;
`SetGestaltValue` (my original choice) does not, and returned an error I had discarded.
Worth remembering for any future OS 9 work: use `NewGestaltValue` to install,
`ReplaceGestaltValue` to change an existing one.

**Our PowerPC handler is live, is being called, and chains correctly.** Everything the 68K
generation was trying to establish is now established on an architecture that coexists
with iTunes and Audio CD Access.

### ★ Measured: `accRun` arrives every 120 ticks = 2.0 seconds

`t=12723 / 12843 / 12965` — 120 ticks apart, exactly `dCtlDelay = 120`. So `accRun` is a
genuine task-level pump, and it is **far too slow as configured**: a 2-second ring
refilled every 2 seconds runs dry continuously. PHASE2.md §2 predicted this; it is now
measured. Step 5 must shorten `dCtlDelay` while playing (≈6 ticks ⇒ ~10 Hz ⇒ ~17.6 KB per
refill, comfortable against 32-sector reads) and restore it on stop.

`audioCallCount=0` is expected: nothing on the machine issues the legacy audio csCodes.
iTunes uses DAE, not `AudioPlay`. **`CDPlayProbe_v2` does** — which makes it the
stand-in for a game until a real mixed-mode disc is available, and the natural way to
drive Step 5 end to end.

# Step 5a — the audio engine (built 2026-08-06, not yet run)

`CDEngineInstall_v3`. The Phase-1 playback engine, moved inside the resident PowerPC
engine and driven from the patched Control entry.

## The design decision that makes this safe: side effects only, always chain

For **every** csCode, including the ones we act on, the original driver is still called
and its result is still returned. We add audio; we never take responsibility for
completing a request.

That sidesteps the one piece of mechanism I could not verify by reading: Phase 2a measured
`AudioStatus` and `ReadQ` arriving **queued**, and a queued request must end at `jIODone`
rather than simply returning. Reproducing that completion protocol from PowerPC is
guesswork. Since the transport csCodes are ones this driver "accepts and ignores" anyway
(Phase 0), letting it run costs nothing and the audio is pure addition.

**⇒ Consequence, stated plainly: Step 5a will play music but will not loop.** A game
polling `AudioStatus` for track end still sees the driver's frozen position. Synthesis is
Step 5b, and it needs one fact this run supplies for free — whether chaining a *queued*
call returns to us at all. If it does, 5b is chain-then-rewrite-`csParam`. If not, 5b
needs a real completion path.

## What happens on each csCode

| csCode | side effect | then |
|---|---|---|
| 104 AudioPlay | decode position → start streaming from that LBA | chain |
| 103 AudioTrackSearch | same, unless the hold flag at csParam+6 is set | chain |
| 105 AudioPause | pause/resume the channel | chain |
| 106 AudioStop | stop, restore block size and `dCtlDelay` | chain |
| 109 AudioControl | record volume (scaling is 5b) | chain |
| 65 accRun | refill the ring | chain |
| anything else | — | chain |

## Details that come from measurement rather than assumption

- **`accRun` is the refill pump, and `dCtlDelay` is shortened to 6 ticks while playing**
  (~10 Hz, ~17.6 KB per refill). Measured last run: `accRun` arrives every 120 ticks =
  2.0 s, which would starve a 2-second ring continuously. Restored on stop.
- **The sound channel is allocated once at install with the SYSTEM zone current.** A
  channel allocated from driver context would otherwise land in whatever application is
  frontmost and vanish when it quits — the hazard REVIEW.md §5 flagged.
- **The TOC is read at install**, at task level, so `AudioPlay` only has to seek and start.
- **`'sowt'` and `notCompressed`**, both confirmed by Phase 1 on this hardware. No byte
  swap anywhere.
- **Block size is restored around every read.** 2352 is wrong for data reads, so leaving it
  set for the duration of playback would break a game reading level data from track 1.
  ⚠ A data read landing *inside* a refill window would still see the wrong size; that race
  is documented, not solved, and removing it entirely needs the ATA (`'dvrf'`) route.
- **Re-entrancy**: the engine issues TOC reads and block-size changes to the very driver
  it has patched, so `gInSelfCall` makes the handler pass its own traffic straight through.
- **The doubleback proc** copies from a pre-allocated ring, emits silence on underrun, and
  never allocates, blocks or touches the File Manager.

## Known limitation to watch for

The transport handlers issue synchronous I/O, so they assume task level. A game's
`PBControlSync` is task level; if one ever arrives at interrupt time the reads would spin
there. Flagged rather than defended against, since no observed caller does it.

## The test, and why it needs no game disc

`audioCallCount` was 0 last run because nothing on the machine issues these csCodes —
iTunes uses DAE. **`CDPlayProbe_v2` issues them**: `AudioPlay`, status polling, pause,
resume, stop. It produced **silence** in Phase 0 against the unpatched driver. So:

> patch → run `CDPlayProbe_v2` → **listen**

Music where there was silence is the entire project demonstrated, on an ordinary audio CD,
before Jubadub ever sees it.

## Step 5a run 1 — DEADLOCK. You cannot do synchronous driver I/O from inside that driver's Control entry

```
CDPlayProbe_v2 log, final line:
  STEP Control csCode=104 refNum=-66        ← written before PBControlSync; never returned

CDEngineInstall_v3 log:
  driveNum=4  audioInitErr=0 (ring, double buffers, sound channel and TOC are ready)
  Gestalt registration: NewGestaltValue=-5552 ReplaceGestaltValue=0
```

`AudioTrackSearch` (103) returned `err=0` because CDPlayProbe sets the hold flag at
csParam+6, so the handler chained without starting playback. **`AudioPlay` (104) froze
inside our handler.**

### The proof is the contrast with install time

`audioInitErr=0` means the *same* operations — `ChangeBlockSize`, `ReadTOC`, driver reads —
succeeded at install. The difference is only *where they ran from*: install runs in the
installer application's own context, while the AudioPlay handler runs **inside the
driver's Control entry**.

The Device Manager serialises I/O per DCE. A synchronous `PBRead`/`PBControl` issued from
inside a Control call cannot begin until that Control call completes, and the Control call
is waiting for the I/O. Self-deadlock, and the very first nested call is enough. Both the
`AudioPlay` pre-fill and the `accRun` refill were built on that impossibility.

### ⇒ The audio cannot live inside the handler. It has to live in a task-level pump.

Three ways out, and only one is sound:

1. **Async I/O from the handler** — issue `PBReadAsync`, return, chain completions at
   interrupt time. Avoids the deadlock but builds an async state machine whose completion
   routines issue further I/O at interrupt level: precisely the class of thing that has
   cost this project the most.
2. **Do it at `accRun`** — no better. `accRun` is *also* delivered as a Control call, so it
   deadlocks identically.
3. **A task-level pump outside the driver** — a process that is not inside any Control call
   does the I/O in its own context, exactly where Phase 1 proved it works.

(3) is the answer, and it converges with the vehicle already required for shipping to
Jubadub: a Startup-Items application.

### The restructure

| | before | after |
|---|---|---|
| driver handler | decode, read, start playback | **write the request into a mailbox, chain. No I/O at all.** |
| audio engine | inside the resident PEF | in the pump application, reusing the Phase-1 code that already works |
| TOC decode | engine | pump (so the engine needs no TOC, and its install-time work shrinks to nothing risky) |

The handler becomes trivially safe at any interrupt level — a few stores and a chain — and
the audio runs where Phase 1 measured 30 seconds with zero underruns. If the pump is not
running, `AudioPlay` requests are simply unserviced: no crash, safe degradation.

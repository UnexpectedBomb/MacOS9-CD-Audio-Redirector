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

# Step 5b — the restructure: driver posts, pump plays (built 2026-08-06, not yet run)

`CDPump_v1` replaces `CDEngineInstall_v3`. Same patch, same engine PEF minus all audio,
plus a pump loop in the application.

## The split

| | driver patch (resident PEF) | pump (application) |
|---|---|---|
| on an audio csCode | copy csCode + 16 csParam bytes into a mailbox, bump `reqSeq`, **chain** | notice `reqSeq` changed, decode, play |
| I/O | **none, ever** | all of it, at task level |
| TOC | not needed | read at startup |
| ring, buffers, sound channel | none | owns them |

The handler is now a few plain stores and a chain, so it is safe at any interrupt level and
cannot deadlock. Verified in the build: the engine PEF no longer imports `SndNewChannel` or
`SndPlayDoubleBuffer` at all, and still exports `DoDriverIO`, `TheDriverDescription` and
`CDEngineControl`.

`reqSeq` is written **last** by the handler and read **twice** by the pump — a seqlock, so a
request half-written when the pump looks is retried rather than acted on.

## Everything in the pump is Phase-1 code

DAE via `ChangeBlockSize(2352)` + driver-level `PBRead`; `SndPlayDoubleBuffer` with
`'sowt'` and `notCompressed`; 0.25 s double buffers; a 2-second ring refilled from the
event loop; 32-sector reads. That is stage B of the Phase-1 spike, which measured 30
seconds with zero underruns on this machine. The only new thing is what triggers it.

Two refinements carried over: the sound channel is allocated with the **system zone**
current so it cannot die with the application, and the 2352-byte block size is taken only
while playing and given back on stop, so data reads see the normal size the rest of the
time.

## Safe degradation

If no pump is running, `AudioPlay` requests are recorded and never serviced. Nothing
crashes; the driver answers exactly as it does unpatched. That is a much better failure
mode than anything the 68K generation had.

## Not yet done

`AudioStatus`/`ReadQ` synthesis. The game still sees the driver's frozen position, so music
will play but will not loop. Now that the pump owns the playback cursor, synthesis needs a
route back from pump to handler — the mailbox in reverse — which is the next piece.

---

# ★★ IT WORKS — 2026-08-06. Music from a legacy AudioPlay on a Mac with no analog CD-audio path

`CDPump_v1` + `CDPlayProbe_v2`, G4 mini, pressed audio CD. Logs archived as
`logs/2026-08-06-WORKING-*`.

```
patch returned 0, status=0, patched=1
CDPumpInit err=0
=== PUMP RUNNING ===
--- request 3: csCode 104 ---            (AudioPlay)
  pump: play LBA 0 .. 14564 (14564 sectors, 194 s)
  pump: pre-roll 352800 bytes
  pump: SndPlayDoubleBuffer err=0
--- request 4: csCode 105 ---            (AudioPause: pause)
--- request 5: csCode 105 ---            (AudioPause: resume)
--- request 6: csCode 106 ---            (AudioStop)

CDPlayProbe_v2:  user reported: MUSIC WAS AUDIBLE
```

Every stage of the chain is visible: the probe issued the legacy `AudioPlay` that a
mixed-mode game issues → our patched Control descriptor caught it → the handler posted it
to the mailbox → the pump decoded it, resolved track 1 to **LBA 0…14564** (exactly track 1's
start to track 2's start, 194 s), pre-rolled the full 2-second ring, and started
`SndPlayDoubleBuffer` → **audible music**. Pause, resume and stop all arrived and were
serviced.

## Why this is definitely ours and not the analog path

Three independent reasons:

1. **The same probe, same machine, same disc produced SILENCE in Phase 0.** The only thing
   that changed is our software.
2. **The drive is not transporting.** The probe reports `reported status did NOT change over
   10 s` — the transport position is frozen, so the drive is not playing the disc. Sound
   cannot be coming from it.
3. Phase 0 established this driver "accepts and ignores" the transport commands, which is
   why the position never moves.

Drive stationary + music audible = the audio is coming from our DAE + Sound Manager path.

## What this validates, cumulatively

| Established | Where |
|---|---|
| CD-DA is readable through the driver | Phase 0 P4 |
| `'sowt'`, no byte swap; streaming holds at real time | Phase 1 |
| The ATAPI driver can be interposed safely, coexisting with iTunes and Audio CD Access | Phase 2 Steps 3–4 |
| The legacy `AudioPlay` can be caught and serviced with real audio | **Step 5b, here** |

## Two housekeeping fixes made immediately

- **`CDPlayProbe`'s conclusion text was stale and actively misleading.** Written in Phase 0,
  it said audible music meant "the analog path works, and if this is the mini the project
  premise needs revisiting" — the exact opposite of what it now means. It now branches on
  the transport position: moved ⇒ analog path (the MDD control case), frozen ⇒ the
  redirector is working.
- **The pump only put its underrun count on screen, never in the log.** Now logged at stop,
  with the dials to turn if it is non-zero.

## What remains before Jubadub can verify it

1. **`AudioStatus`/`ReadQ` synthesis.** Confirmed still outstanding — the probe saw a frozen
   position throughout. Music plays; a game polling for track end will not loop or advance.
   The pump owns the cursor now, so this needs the mailbox in reverse.
2. **Faceless Startup-Items packaging** (SIZE flags 0x14C0). Packaging, not new mechanism.
3. **A real mixed-mode game disc.** Everything so far is a pressed audio CD. Track 1 being
   data, the game issuing the calls, and whether an audio session mounts alongside the HFS
   volume are all untested. That is precisely what Jubadub's run settles.
4. A README a third party can follow, then the forum post.

# Step 5c — position synthesis (built 2026-08-06, not yet run)

`CDPump_v2`. The last remaining functional gap: a game polling for track end saw the
driver's frozen position, so music played but never looped.

## Chain-then-rewrite, made possible by the last run

The open question was whether a **queued** call returns to us if we chain it. The last run
answered it for free: CDPlayProbe polled `AudioStatus` and `ReadQ` **forty times through our
handler**, and every one chained and came back with data. So the handler can let the
original answer and then correct it — no need to reproduce the `jIODone` completion
protocol from PowerPC, which was the thing I did not want to guess at.

```c
err = gOrigCtl(pb, dce);        /* let the original answer */
... overwrite csParam from our cursor ...
return err;                     /* the caller sees our values */
```

Safe because the caller is blocked in the same thread of control: even if the original
completed the request internally, it cannot resume until we return.

## The cursor comes from what the speaker has heard

Published by the pump, derived from `gReadOff` — bytes the Sound Manager has **consumed** —
not from what was requested and not from how much has been read off the disc. FEASIBILITY §6
warned that a cursor taken from the wrong place gives music that never loops or loops
instantly; the only honest source is what has actually been played.

2352 bytes = one CD frame, and absolute frame numbering is LBA + 150, matching what `ReadQ`
reported on hardware in Phase 0.

## What gets rewritten, and what deliberately does not

- **`ReadQ` (101): all nine bytes.** Its layout was cross-checked against the TOC to the
  exact frame in Phase 0 — control/adr, track BCD, index, track-relative MSF, absolute MSF —
  so there is nothing to guess.
- **`AudioStatus` (107): bytes 3..5 only, plus byte 0.** Bytes 3..5 are the absolute MSF and
  match `ReadQ`'s. The rest of the original's answer is left untouched, because we cannot
  decode it and inventing values would be worse than passing them through.
- **⚠ Byte 0 is a guess, and marked as one.** The MMC audio-status codes are 0x11
  play-in-progress, 0x12 paused, 0x13 completed; this driver returned 0x00 in every Phase-0
  sample, which is not a valid MMC code. Reporting play-in-progress is the reading most
  likely to make a polling game behave, it is a single named constant
  (`kSynthStatusPlaying`) to change, **and the pump now logs what the original actually
  answered** so the guess can be replaced with evidence.

Synthesis only happens while `playState != 0`, so when nothing is playing the driver's own
answers pass through untouched.

## What the next run should show

- Music, as before.
- `ORIGINAL AudioStatus answer (before our rewrite)` and the same for `ReadQ`, logged once
  each — the evidence for byte 0.
- `synthesised answers: N AudioStatus, M ReadQ` at stop, with N and M non-zero.
- ★ And in `CDPlayProbe`'s own log, the line that has said `reported status did NOT change`
  in every run so far should finally say the position **CHANGED** — the probe polls for 10
  seconds, so a moving cursor is exactly what it is built to detect.

## Step 5c run 1 — SYNTHESIS WORKS, and the numbers verify to the frame

```
CDPlayProbe:  ⇒ reported status CHANGED over 10 s
              user reported: MUSIC WAS AUDIBLE
  first AudioStatus: 00 00 00 07 18 68     ← the driver's stale answer
  last  AudioStatus: 11 00 00 00 12 00     ← ours
```

Byte 0 is `0x11` (our play-in-progress marker) and the MSF decodes to **00:12:00**. From
LBA 0 that is absolute frame 900, i.e. 750 frames = **exactly 10.0 s of audio delivered** —
the probe's poll window, to the frame. Red Book puts LBA 0 at 00:02:00, so LBA 750 is
00:12:00. Independently: `gReadOff` advances 44,100 bytes per callback, and 1,764,000 /
44,100 = 40 callbacks = 40 × 0.25 s = 10.0 s. **The cursor is not merely moving, it is
correct.**

### The captured originals settle byte 0 and re-validate the layout

```
ORIGINAL AudioStatus: 00 00 00 07 18 68
ORIGINAL ReadQ:       00 02 01 03 40 38 07 18 68
```

- **AudioStatus byte 0 = 0x00 when nothing is playing.** So reporting `0x11` for playing
  does not collide with anything the driver itself uses. The guess is now evidence-backed
  as *consistent*, though not yet confirmed against a game.
- **The ReadQ arithmetic closes again, on a different position.** abs 07:18:68 = frame
  32918 → LBA 32768; rel 03:40:38 = 16538 frames; difference = **LBA 16230, exactly track
  2's start on this disc**. Our layout understanding is right for the second time, at a
  different point on a different disc.

## Two fixes made straight away, one of them my own regression

### 1. The probe's discriminator was wrong AGAIN — and this time I broke it myself

I had "fixed" it last run to read: position moved ⇒ analog path, position frozen ⇒ the
redirector is working. **Synthesis inverts that**, because the redirector now deliberately
makes the position advance. "Moved" is consistent with both.

There is no way to tell from the position at all any more, so the probe now **asks the
redirector directly** via `Gestalt('CDau')` and reports `patched`, `pumpAlive`, `pumpPlaying`
and the underrun count. Resident ⇒ the music and the movement are both ours; not resident ⇒
the unpatched baseline, where music could only be the analog path. Definitive rather than
inferred, and it makes the log self-explanatory for a third party.

*Lesson worth keeping: a diagnostic's interpretation text is not write-once. Each time the
system under test gains a capability, every "therefore" in the tooling has to be re-checked
— this one went stale twice in three runs.*

### 2. End of track now holds the final position instead of reverting to a stale one

Previously, finishing a track set `playState = 0`, which stopped synthesis and let the
driver's own answer through — a position left over from an earlier iTunes session, **in a
different track entirely**. A game polling for track end would have seen the position jump
backwards to nonsense rather than arriving at the boundary.

Now natural completion moves to `playState = 3`: the final position is held and the status
byte reports completed (`0x13`), which is what a real drive does. Only an explicit
`AudioStop` returns to the driver's own answers.

## State: the mechanism is complete

`CDPump_v3` + `CDPlayProbe_v3`. Everything the fix needs to do, it now does: catch the
legacy call, play the audio digitally, and report a truthful advancing position with a
sane end-of-track. What remains is packaging and validation against a real game, not
mechanism.

# Run 2026-08-06b: repeat play, the loop path

G4 mini, pressed audio CD (15 audio tracks, lead-out 57:03:45). One boot: `CDPump_v3`
launched with option, then **`CDPlayProbe_v3` run three times against that one live pump**,
then `CDTraceRead_v1`. Logs in `logs/2026-08-06-repeat-play/`.

Run validation: last banners are `CDPump v3` / `CDPlayProbe v3`, `patched=1`, three
`pump: play LBA` blocks, 329 trace entries in a 512-entry ring so **nothing wrapped** — the
trace is a complete record of the boot, not a window onto it.

## The re-arm works. Three plays, and the cursor restarts from zero each time

All three probe runs: `MUSIC WAS AUDIBLE`, `reported status CHANGED`, redirector reported
resident. The decisive evidence is the synthesised position at the end of each run:

| run | last AudioStatus | abs MSF | frames | LBA | seconds delivered |
|---|---|---|---|---|---|
| 1 | `11 00 00 00 12 18` | 00:12:18 | 918 | 768 | 10.24 |
| 2 | `11 00 00 00 12 00` | 00:12:00 | 900 | 750 | 10.00 |
| 3 | `11 00 00 00 12 00` | 00:12:00 | 900 | 750 | 10.00 |

Each equals the probe's own 10 s poll window, to the frame. **This is the discriminator**:
had `CDPumpPlay` failed to reset `gReadOff`/`gWriteOff`, run 2 would have reported ~20 s and
run 3 ~30 s. It reports 10 s every time, so the stop-and-restart path in `CDPumpPlay` is
correct and a game that loops its music will be served indefinitely.

The first sample of every run is the pass-through original `00 00 00 07 18 68` — status byte
`0x00`, idle — captured before the first synthesised answer. Consistent across all three.

`0 underruns` across all three plays, 2282 KB delivered on the final one.

## The block size is balanced: taken three times, given back three times

The trace ring shows one shape, repeated exactly three times with nothing left over:

```
AudioControl(volume) -> AudioTrackSearch -> AudioPlay -> ChangeBlockSize(0x0930 = 2352)
   -> 40 x [AudioStatus + ReadQ] -> AudioPause(1) -> AudioPause(0) -> AudioStop
   -> ChangeBlockSize(0x0200 = 512)
```

Six `ChangeBlockSize` calls, alternating 2352 and 512. `TakeBlockSize`/`GiveBackBlockSize`
do not leak the drive's block size across plays, which is what would have poisoned a game's
data reads after the first piece of music ended.

Full histogram over the boot: 120 AudioStatus, 120 ReadQ, 12 ReadTOC, 6 AudioPause,
3 each of AudioControl / AudioTrackSearch / AudioPlay / AudioStop, 6 ChangeBlockSize, the
rest `accRun`. The engine reports 114 synthesised AudioStatus against 120 seen: the missing
6 are the two polls per run that arrive before the pre-roll finishes, so the pump is not yet
playing and correctly passes them through. 120 − 6 = 114 exactly.

## ★ The warm read throughput, finally measured: ~330 KB/s, 1.9x real time

FINDINGS previously flagged the 106 KB/s figure as a cold-read artifact and asked for a
warm measurement. The trace gives it, because the pre-roll read sits between two timestamped
calls: `ChangeBlockSize(2352)` and the next poll to arrive.

| run | pre-roll gap | 352800 bytes at | vs real time (176.4 KB/s) |
|---|---|---|---|
| 1 (cold) | 138 ticks = 2.30 s | 153 KB/s | 0.87x — below real time |
| 2 (warm) | 66 ticks = 1.10 s | 320 KB/s | 1.82x |
| 3 (warm) | 64 ticks = 1.07 s | 330 KB/s | 1.87x |

**Warm, the drive delivers CD-DA at about 1.9x real time.** That is the headroom figure to
reason about, and it explains the zero underruns. It is also the number that decides the
data-contention question on a real game disc: a game reading level data from track 1 while
music plays has roughly 0.9x real time of spare drive bandwidth to work with, which is not
generous. Cold, the drive is *below* real time, so the pre-roll is doing real work.

## ★ Start latency: 2.7 s cold, 1.5 s warm, from AudioPlay to first sound

Measured `AudioPlay` (trace) to the first call after the pre-roll completes:

- run 1: t=28109 → 28269 = 160 ticks = **2.67 s**
- runs 2 and 3: ~90 ticks = **1.50 s**

Two components: ~0.37 s of mailbox latency (the handler posts, the pump collects it on its
next event-loop pass) plus the 2 s pre-roll read itself.

A real drive starts audio in a few hundred milliseconds. 1.5 s is probably tolerable in a
game, but it is a visible difference in behaviour and it is cheap to improve: the 352800-byte
(2.0 s) pre-roll is larger than it needs to be now that sustained throughput is known to be
1.9x real time. Starting at ~0.5 s of pre-roll and letting the refill loop catch up would cut
perceived latency by roughly a second. **Not yet done, and it should not be done blind** —
the 2 s pre-roll is also the underrun cushion, and the cold-read number above shows the first
read genuinely is slower than real time.

## ⚠ The pump reads the TOC once, at launch. This blocks the Startup-Items packaging

`CDPumpInit` calls `CDReadTOC` once (`cd_pump_audio.c`), and nothing re-reads it.
`DecodePos` resolves every request against that one snapshot. Confirmed in the trace: the
pump's three `ReadTOC` calls are at t=26837, at install; the second group at t=28104 is the
probe's own, chained through.

Consequences:

1. **For testing:** the disc must be in the drive before the pump is launched, and cannot be
   swapped afterwards. One disc per boot.
2. **For shipping — this is a blocker.** A faceless Startup-Items app launches at boot with
   an empty drive. `gTOC.valid` is false, and it stays false for the whole session, so no
   request can ever be resolved. The fix is to re-read the TOC inside `CDPumpPlay` when the
   TOC is invalid or the disc may have changed. **Do this before building the faceless
   version**, or the handoff ships a build that only works when a disc happened to be in the
   tray at startup.

## Still untested after this run

- **A second AudioPlay for a *different* track.** All three runs targeted track 1, because
  `CDPlayProbe` always selects the first audio track on the disc. The re-arm is proven; track
  *switching* — a new `gTrackStartLBA`, a new range — is not.
- **Natural end of track.** The probe stops after ~12 s; track 1 is 216 s long. The
  `playState = 3` / status `0x13` path has never executed on hardware.
- **A disc whose track 1 is data.** `DecodePos` has only ever run against an all-audio TOC.
- **Data reads contending with music.** The 1.9x figure above says it will be tight.

# Run 2026-08-06c: the empty-drive boot — `CDPump_v4` PASSES

G4 mini. Booted with the **tray empty**, launched `CDPump_v4` with option, *then* inserted the
pressed audio CD, then ran `CDPlayProbe_v3` twice. Logs in `logs/2026-08-06-empty-drive-boot/`.

This is the run that validates the TOC re-read, and it is deliberately the faceless
Startup-Items scenario: the pump exists before the disc does.

⚠ **Run validation caught one thing:** the `CD Trace Log` on the share was byte-for-byte
identical to the previous run's, so `CDTraceRead_v1` was not re-run and that file is stale. It
is **not** archived with this run (see `NOTE.txt` there). Nothing was lost — the engine log
carries the whole answer — but it would have been easy to read a previous boot's trace as
this one's.

## The result

```
ReadTOC first/last FAILED err=-65 (no disc, or the driver does not implement ReadTOC)
pump: no TOC at startup (empty drive?) - will re-read on the first play request
CDPumpInit err=0
=== PUMP RUNNING ===
--- request 3: csCode 104 ---
pump: TOC generation 1 - 15 track(s), 15 audio, first=1 last=15
   ... full 15-track table ...
pump: play LBA 0 .. 16230
```

- **The empty-drive TOC read fails cleanly and fast** with `err=-65` (`offLinErr`). It does
  not hang, which was the one thing I could not predict — the drive is asked and answers.
- **`CDPumpInit` returns 0 anyway**, the pump runs, and the patch is live with no disc present.
- **The disc inserted afterwards is picked up on the first play request**, full table logged
  as `TOC generation 1`, and music plays. Both probe runs: audible, resident, status changed.
- **The change suppression works.** Two plays, and **exactly one** `TOC generation` line: the
  second re-read found the same disc and said nothing, which is what keeps the log readable
  and the play path cheap.
- 0 underruns; cursors 10.49 s and 10.24 s, each restarting from zero, so v4 did not regress
  the re-arm proven in the previous run.

**The shipping blocker is closed.** A faceless app can now start at boot with an empty tray.

## ★ Two things this run exposed that the audio-CD-first runs could not

### 1. The CD driver's refNum is not stable across boots — it was −66, this boot it is −56

Nothing hardcodes it (the engine discovers it at `kInitialize`, and the discovery is what
found −56), so nothing broke. Worth knowing all the same: **any future code, log
interpretation or test instruction that assumes −66 is wrong.** The likely cause is that the
device enumeration order differs when the tray is empty at boot.

### 2. ⚠ `driveNum` is 0 in exactly the configuration we are about to ship

With an empty tray the CD does not appear in the drive queue, so the pump-start discovery
reports `no driver reported devt=='cdrm'` and `CDPumpInit(refNum=-56 drive=0)`. `gDriveNum`
is then used as `ioParam.ioVRefNum` on every `PBRead`.

**It worked** — the reads succeeded and the music played, because the read is driver-level and
`ioRefNum` is what selects the driver. But this is the same "captured once at launch" shape as
the TOC bug just fixed, it is now the *default* shipping configuration rather than an edge
case, and it rests on this particular driver ignoring `ioVRefNum`. Jubadub's drive may not.

`EnsureTOC` should refresh `gDriveNum` alongside the TOC. Cheap, and it removes the assumption
rather than relying on it holding.

### 3b. A stale interpretation line, again

The log says `--- discovery stage 2 SKIPPED (shift held) ---`. Shift was **not** held; option
was. The call site passes `allowFullSweep = false` unconditionally
(`cd_engine_install.c:474`), so the message names a cause that had nothing to do with it.
Third instance of the same class in this project — the message must state what the code
actually did, not why it once did it.

# Run 2026-08-06d: the shipping artifact, installed and booted — PASSES

`CDAudioRedirector_v1` dropped into `System Folder:Startup Items:`, machine rebooted with the
**tray empty**, disc inserted afterwards, `CDPlayProbe_v3` run once. Logs in
`logs/2026-08-06-faceless-startup/`. This is the end-user flow with no human in the loop.

```
=== CD Audio Redirector v1 - Red Book CD audio for legacy Mac CD games
=== FACELESS: patching automatically, no window, runs until shutdown
  quit Apple event handler installed err=0
  open-application handler installed err=0
  ...
  patch returned 0, status=0, patched=1
  pump: no TOC at startup (empty drive?) - will re-read on the first play request
=== PUMP RUNNING ===
  pump: drive number resolved to 4 (it was 0 at launch, when the tray was empty)
  pump: TOC generation 1 - 16 track(s), 16 audio, first=1 last=16
  pump: play LBA 0 .. 13922 (13922 sectors, 185 s)
  pump: SndPlayDoubleBuffer err=0
```

- **Patches unattended at boot with no key held**, both Apple event handlers install cleanly,
  and the user confirms it does not appear in the Application menu.
- **`RefreshDriveNumber` fired and did its job**: `drive number resolved to 4`, so the shipping
  build no longer rests on the driver tolerating `ioVRefNum = 0`.
- Probe: `MUSIC WAS AUDIBLE`, `patched=1 pumpAlive=1 underruns=0`, cursor `11 00 00 00 12 00`
  = 10.0 s delivered, exact to the frame — and this was a **different disc** (16 tracks,
  track 1 = 185 s) from every previous run, so `DecodePos` has now resolved against two TOCs.
- **Zero underruns with the pump as a genuine background Startup Items app.** Encouraging for
  the background-time question, though the probe still is not a game.

## ✅ The quit path, settled by re-reading the log after a restart

The first copy of the log ended mid-session with no `=== pump stopped ===` block, because
that is written only when the pump loop exits — and the pump was still running. Re-copied
after a normal restart, the same file now carries, at the end of that first session:

```
=== pump stopped: 2196 KB delivered, 0 underruns ===
  synthesised answers: 37 AudioStatus, 37 ReadQ
  ⇒ zero underruns: the ring and the event-loop refill kept up.
  NOTE: reaching this line means the pump loop ended, which for a faceless
    build means a quit Apple event - normally shutdown.
=== end of run ===
```

So at shutdown the quit Apple event reached the app, `PumpLoop` exited, the pump stopped
cleanly and the log was closed properly. It was not force-quit, and nothing was lost.

The same file then shows a **second** session from the following boot — that one with a disc
already in the tray, so the startup TOC read succeeded and logged all 16 tracks immediately.
The normal-boot-with-disc case is covered as well, which no earlier run had exercised for the
faceless build.

## ★ REAL BUG FOUND IN THE LOG: the mailbox is one slot, and it drops requests

The request numbers in this run are **1, 4, 5, 6, 7** — 2 and 3 never appear. Previous runs
show the same shape (3,4,5,6 then 9,10,11,12: 7 and 8 missing).

`PumpLoop` is the cause, and it is a design property rather than a slip:

```c
seq = pub->reqSeq;
if (seq != lastSeq) {
    cs = pub->reqCsCode;          /* whatever is in the slot RIGHT NOW */
    ...
    lastSeq = seq;                /* jumps over everything in between */
```

The handler writes each request into a **single slot** and bumps `reqSeq`. If two requests
arrive between two passes of the pump's event loop, the first is overwritten and lost —
silently, since `lastSeq` jumps straight to the newest sequence number.

It has been harmless so far only by luck of ordering: the dropped calls were
`AudioControl` (volume) and `AudioTrackSearch` with the hold flag, neither of which the pump
acts on anyway. **Nothing protects an `AudioPlay`.** A game that issues `AudioStop`
immediately followed by `AudioPlay` — which is exactly how a music loop restarts — can have
the `AudioPlay` land in the same window and be dropped, giving silence with no error anywhere.

The pump polls fast (its loop sleeps 1 tick), so the window is small, which is why this has
not bitten yet. Small is not zero, and the failure is silent.

**Fix: make the mailbox a small ring** — say 16 entries, the same single-producer /
single-consumer discipline already used for the PCM ring, with the pump draining every
pending entry per pass instead of reading one. The handler stays what it must be: a write and
a chain, safe at any interrupt level.

## The refNum has now been three different values

−66, −56, and −68 across three boots. Nothing hardcodes it, and the discovery finds it every
time. Recorded because the temptation to write "the CD driver is −66" into a doc or a test
instruction is real, and it would be wrong two times in three.

## Something other than our probe issues legacy audio calls (2026-08-06d)

`request 1: csCode 106` (`AudioStop`) arrives on its own, right after `PUMP RUNNING` and
before the probe was launched — almost certainly the Finder or the CD Remote machinery on
disc insertion. Harmless (a stop on an idle pump does nothing), and it quietly amends the
earlier note that `audioCallCount = 0` because *nothing* on the machine uses the legacy audio
API. Something does, on the disc-insertion path.

# Run 2026-08-06e: `CDAudioRedirector_v2` — the ring works, and the machine crashed

Two separate results in one run, and they must not be conflated.

⚠ **Reading note:** `CD Audio Redirector Log` was not deleted before this run, so it holds
**two** sessions. The crash is the **first** (line 3); the second (line 157) is the reboot
*after* the crash and shows only `PUMP RUNNING` with no probe activity. Reading the last
banner — the usual rule — gives the wrong session here.

## ✅ The request ring works. Requests arrive consecutively for the first time

```
--- request 0: csCode 109 ---     AudioControl (volume)
--- request 1: csCode 103 ---     AudioTrackSearch
  hold flag set; positioning only, not playing
--- request 2: csCode 104 ---     AudioPlay
```

**0, 1, 2 with no gaps.** That was the stated discriminator and it passed. In every previous
run the volume call and the TrackSearch were silently swallowed — the numbers always skipped.
The single-slot mailbox was losing exactly what the analysis said it was losing, and the ring
fixed it.

Note also that request 1 is correctly recognised as `AudioTrackSearch` **with the hold flag
set**, so it positions without playing. That branch had never been exercised before, because
the call that reaches it had always been one of the dropped ones.

## ✅ The TOC re-read caught a genuinely bogus startup TOC

At `CDPumpInit` the drive reported **one** track and a lead-out of 73:40:66. The disc actually
has 16 tracks and a lead-out of 75:23:02 — the drive answered before it had really read the
disc. `EnsureTOC` re-read at play time and got the true 16-track TOC, and the play range was
clamped correctly to `LBA 0 .. 13922` by track 2's start.

Under the old read-once code the pump would have spent the whole session believing in a
one-track disc. This is the second distinct failure the TOC re-read has now absorbed.

## ⚠ THE CRASH — unexplained, and NOT yet attributed

The machine died roughly 3.5 s after `AudioPlay`: about 1.5 s of pre-roll, then 1.6 s of
playback. The probe's log stops mid-call:

```
  poll 6 (t=23453 ticks)
STEP Status csCode=107 refNum=-57     -> answered, synthesised 11 00 00 00 03 37
STEP Status csCode=101 refNum=-57     <- last line in the file, no result
```

Synthesis was working right up to the end: poll 5 reported abs MSF 00:03:18 (LBA 93, 1.24 s)
and poll 6 reported 00:03:37 (LBA 112, 1.49 s), advancing correctly.

The pump's own log ends after the two `ORIGINAL ...` captures, which is **expected** — the
pump logs nothing between requests — so the pump's silence is not evidence of where it died.

### What has been ruled out

- **A stale engine PEF.** The obvious suspect, since a resident fragment built against the old
  `CDEnginePublic` would write the ring beyond a smaller allocation and corrupt the heap —
  which would look exactly like this. It is not what happened: the PEF is newer than the
  header that changed, and the resident engine reported `version=2` in the crash log itself.
- **The pump and shared code.** `cd_pump_audio.c` and `cd_probe_common.c` are byte-identical
  between the working v1 and the crashing v2 (`git diff d9377b4..HEAD`). The only code that
  changed is the ring producer, the ring consumer, and version strings.
- **The probe's new struct handling.** It runs only after `Cleanup`, long after poll 6.

### What has not been ruled out

- **The ring itself.** It is the main thing that changed. Reviewing the producer and consumer
  has not turned up a memory-safety defect — the producer is still plain aligned stores, the
  index masks to 0..15, and the overflow branch terminates — but "I could not find the bug"
  is not "there is no bug".
- **The environment.** `EHCIUIM_init.log`, `USB Disk Log` and `USB2 Activate Log` are on the
  share from earlier the same afternoon, so the **experimental USB 2.0 EHCI stack was also
  live on this machine**. Two experimental drivers, one of them doing bus-master DMA, while
  our pump hammers the ATAPI bus. That is a variable, not an accusation.
- **The disc or drive.** The same drive returned a fabricated one-track TOC at startup in this
  very run, which is not a healthy answer.

### The next run is the control, not a fix

Project rule, learned the expensive way: run the known-good control first when a regression
appears. `CDAudioRedirector_v1` and `CDPlayProbe_v3` are still on the share and are a matched
engine-version-1 pair. Running them on this machine, in this state, with this disc splits the
question cleanly:

- **crashes too** ⇒ not the ring; the cause is environmental (USB2, drive, disc) and the ring
  change is exonerated;
- **does not crash** ⇒ the ring is implicated and gets instrumented rather than guessed at.

Shipping a speculative fix before that run would repeat the two-wrong-ROMs mistake.

# Run 2026-08-06f: the v1 control — the ring IS implicated

Same machine, same disc, same session state, `CDAudioRedirector_v1` back in Startup Items.
Logs in `logs/2026-08-06-v1-control/`.

**It ran to completion.** All 40 polls, music audible, no freeze. Against v2's freeze at poll
6 on the identical setup, that is as clean an A/B as this project gets.

Two things the control also settled for free:

- **The drop diagnosis is confirmed from the other direction.** v1's request numbers are
  `3, 4, 5, 6` — it never saw 0, 1 or 2. v2 logged `0, 1, 2` consecutively. The single-slot
  mailbox really was swallowing the volume call and the TrackSearch, exactly as claimed.
- **The version guard works.** The probe run was actually `CDPlayProbe_v4` against engine
  version 1, and it said so rather than printing nonsense:
  `!! engine reports version 1, this probe was built for 2.` The guard added alongside the
  ring earned its place on its first exposure. (It also means the control was v1+v4, not
  v1+v3 — which does not weaken it: the probe only reads the block after `Cleanup`, long
  after the point where v2 froze.)

**The USB 2.0 stack is exonerated**, as the user judged: it was live for both runs.

## The freeze signature, and why it argues against a wild pointer

Force-quit drew its window **frame with no text and no buttons**. That is not the signature of
a bus error — that would be a System Error dialog with a type number. A frame with no content
means the Toolbox got far enough to draw a window and then could not finish: something is
wedged below the application, not scribbling over it.

## What makes this hard, stated honestly

At the moment of the freeze **the ring is idle**. The pump has drained requests 0, 1 and 2;
`AudioStatus` and `ReadQ` are not in the posted set, so nothing writes the ring and the drain
loop's condition is false on every pass. The change that is implicated is not executing when
the machine dies.

That leaves two shapes of explanation, and the log cannot choose between them:

1. damage done earlier, during requests 0-2, that only surfaces once playback is running;
2. something about the *steady state* that differs because of the change — including the
   simple fact that v2 **delivers** two requests v1 threw away, so the pump reaches code paths
   with the drive in a different state.

Reviewing the producer and consumer has not found a memory-safety defect: the producer is
plain aligned stores, the index masks to 0..15, the overflow branch terminates. And the
handler's synthesis was still returning correct advancing positions on the last completed
poll (00:03:37 = LBA 112 = 1.49 s), which means the handler and the pump agreed on the new
field offsets — so a layout mismatch is ruled out too.

"I could not find it by reading" is where this project has historically shipped a wrong guess.
So the next build measures instead.

## `CDAudioRedirector_v3` / `CDPump_v7`: the ring unchanged, plus a heartbeat

One log line per second while playing:

```
beat t=<ticks>  <N>KB  underruns=<n>  reqR=<r> reqW=<w> drop=<d>
```

The ring is left exactly as it is, so the freeze should reproduce. The heartbeat answers the
one question that splits the field:

- **beats stop with the probe's last line** ⇒ the whole machine wedged at once; the cause is
  below the application and the next look is at the handler and the driver;
- **beats continue past the probe's last line** ⇒ the pump is alive and it is the probe's
  `Status` call into the driver that never returned; that points at the interrupt-level
  handler, not at the ring's bookkeeping.

`reqR`/`reqW`/`drop` ride along so a runaway index appears as a number rather than a theory.

Engine version stays 2, so `CDPlayProbe_v4` and `CDTraceRead_v2` remain the correct partners.

# Run 2026-08-06g: the heartbeat measured nothing — and its absence is the finding

`CDAudioRedirector_v3` froze again, at probe poll 13 (v2 froze at poll 6). Logs in
`logs/2026-08-06-v3-heartbeat/`. Both logs were deleted first this time, so each holds
exactly one session.

**The heartbeat never appeared. Neither did anything else from the pump.** After
`=== PUMP RUNNING ===` the redirector's log contains **not one further line** — no
`--- request N ---`, no `pump: play LBA`, no `beat`.

## But the pump was alive and playing the whole time

The probe's log settles it. Every `AudioStatus` answer carries byte 0 = **`0x11`**, which is
*our* synthesis marker — the handler only writes it when `playState` says playing, and
`playState` is set by the pump. And the positions climb smoothly:

| probe poll | synthesised abs MSF | LBA | audio delivered |
|---|---|---|---|
| early | 00:02:37 | 37 | 0.49 s |
| … | 00:03:00 | 75 | 1.00 s |
| … | 00:03:56 | 131 | 1.75 s |
| … | 00:04:18 | 168 | 2.24 s |
| last | 00:05:00 | 225 | 3.00 s |

Those numbers are derived from `gReadOff` — bytes the **Sound Manager consumed**. So the
pump's ring, its refill loop and `PublishCursor` were all running normally, right up to the
freeze, having delivered three full seconds of audio.

## And the File Manager was healthy

In that same window the **probe wrote 359 lines** to its own log, each one flushed. The
volume was fine. It was not a wedged disk, and it was not the File Manager.

## So the pump's logging channel was already dead, and that defeated the instrumentation

The pump was playing, publishing, and being scheduled — while producing no log output at all.
Only two things in `CDLogf` can do that:

- **`gQuietDepth > 0`** — a quiet region entered and never left, so every line is discarded.
  That counter is new, added with the TOC re-read (`CDLogSetQuiet`), and `EnsureTOC` and
  `RefreshDriveNumber` both wrap calls in it.
- **`FSWrite` failing** — and `CDLogf` *discarded its result*, so a failure left no trace.
  Third time this project has been blinded by a dropped error, after `CDLogOpen` and
  `SetGestaltValue`.

**Lesson, and it is a general one: instrumentation is only as good as the channel it travels
down. A heartbeat written to a log that has stopped working measures nothing, and its silence
looks exactly like the thing you were trying to detect.** The v3 build could not have
distinguished "pump wedged" from "pump fine, log broken" — those were the two hypotheses, and
it was blind to the difference.

## `CDAudioRedirector_v4` / `CDPlayProbe_v5`: measure through the channel that works

The pump now publishes liveness to the **shared block** instead of the log, and the **probe**
reports it — because the probe's log is the one that demonstrably survived:

- `pumpBeat` — incremented once per pump event-loop pass
- `pumpReqSeen` — requests actually drained
- `logQuietDepth`, `logLastErr`, `logWrites` — the logger reporting on itself
- alongside `playState`, `curAbsFrame`, `reqRead`/`reqWrite`/`reqDropped`, underruns

`CDLogf` now keeps the `FSWrite` result rather than dropping it, and `CDLogDiag` exposes it.

One line per poll, in the probe's log, four times a second. When it next wedges:

- **`beat` frozen while polls continue** ⇒ the pump stopped being scheduled;
- **`beat` still climbing at the last poll** ⇒ the pump was alive and the wedge is elsewhere;
- **`quiet > 0`** ⇒ the pump's log was suppressed by an unbalanced quiet region — and that is
  a bug in my TOC code, not in the ring;
- **`logErr != 0`** ⇒ its writes were failing, and now we will know it;
- **`reqW` climbing while `reqR` sticks** ⇒ the drain loop stopped draining.

Engine version is now **3** (the block grew), so the matching set is `CDAudioRedirector_v4`,
`CDPump_v8`, `CDPlayProbe_v5`, `CDTraceRead_v3`. Older readers will refuse rather than
misreport, which is the guard doing its job.

# Run 2026-08-07: the instrumentation worked, and it exonerates the ring

`CDAudioRedirector_v4` + `CDPlayProbe_v5`. Froze again, at poll 28. Logs in
`logs/2026-08-07-v4-published-beat/`.

This time the measurement went down a channel that worked, and it answered every question the
last three runs could not.

## The pump was perfectly healthy at the moment the machine died

The last pump-state line the probe wrote, one poll before the freeze:

```
pump: beat=74262 reqSeen=3 state=1 absF=675 | reqR=3 reqW=3 drop=0 | under=0
      | quiet=0 logErr=0 logWrites=118
```

Every field is what it should be:

- **`beat` climbing** — 74219 → 74262 across the run. The pump was still being scheduled at
  the last measurement. It did not wedge.
- **`reqR=3 reqW=3 drop=0 reqSeen=3`** — the ring drained all three requests, then sat idle
  and consistent for the remaining seven seconds. **It is not doing anything when the machine
  freezes.**
- **`state=1 absF=675`** = LBA 525 = **7.00 s of audio delivered, 0 underruns.**
- **`quiet=0 logErr=0`** — both of my suspects from the v3 run are dead. The quiet counter was
  never stuck and `FSWrite` never failed.

The pump's own log confirms it independently: six heartbeats, one per second,
`86KB → 1033KB delivered, underruns=0, reqR=3 reqW=3 drop=0`.

**(The v3 run's totally empty pump log therefore remains unexplained.** `logWrites` proves the
same code logs fine here. It is no longer load-bearing, but it was never accounted for.)

## ⚠ I was too confident about the v1 control. The freeze point moves.

| build | polls completed | time | outcome |
|---|---|---|---|
| v2 (ring) | 6 | 1.50 s | froze |
| v3 (ring + log heartbeat) | 13 | 3.25 s | froze |
| v4 (ring + published heartbeat) | 28 | 7.00 s | froze |
| **v1 (control)** | 40 | 10.00 s | completed |

The onset varies by more than **4×** between builds that are supposed to fail the same way.
That is the signature of an intermittent fault, not a deterministic one — and against a
failure whose onset ranges from 1.5 s to beyond 7 s, **a single clean 10-second v1 run is not
evidence that v1 is immune.** It is one sample that happened to survive.

So two earlier conclusions were over-claimed on n=1 each, and I am withdrawing both:

- **"The ring is implicated."** The instrumentation now shows the ring idle, consistent and
  correct at the freeze. Nothing about it is executing.
- **"The USB 2.0 stack is exonerated."** That rested on the same single control run.

What is actually established is narrower: **the freeze happens during CD-DA streaming, is
intermittent, and is not caused by the pump, the ring, the logger or the drain loop** — all
four were measured healthy at the last sample before the machine stopped.

## Next: the control, properly powered

One boot, `CDAudioRedirector_v1` + `CDPlayProbe_v3`, and run the probe **three times** against
the one pump. Three 10-second plays instead of one.

- **Any run freezes** ⇒ v1 is not immune, the ring is fully exonerated, and this is a
  pre-existing fault that has been there all along — the thing to chase is CD-DA streaming
  itself, not this month's changes.
- **All three complete** ⇒ v1 really does look different, and the difference has to be
  something other than the ring, since the ring is provably idle when v4 dies.

Either way it costs one reboot and removes the guesswork that three builds have not.

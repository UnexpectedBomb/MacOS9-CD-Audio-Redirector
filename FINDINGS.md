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

## ⚠ What this breaks — the toolchain

PHASE2.md §2 chose an **all-68K** engine, on the reasoning that the entry we patch is
68K code so the patch must be 68K anyway. **That premise is now false**, and worse:

**This Retro68 installation is PowerPC-only.** There is no `m68k-apple-macos`
toolchain directory, no m68k gcc in `toolchain/bin`, and no 68K CMake toolchain file
— only `powerpc-apple-macos`. Retro68's own `Samples/SystemExtension` says
"PowerPC is not currently supported here" for code resources, and a classic INIT
*is* a 68K code resource.

**⇒ With the toolchain as installed, no INIT of any kind can be built.** That is not
a Phase 2 detail; it decides the shape of the shipped artifact, and it is recorded as
an open decision rather than settled unilaterally. See PHASE2.md §7.

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

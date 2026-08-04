# Review of FEASIBILITY.md — 2026-08-04

Separate from `FEASIBILITY.md` on purpose: that doc stays the original scoping snapshot. This is the
critique, with the evidence each point rests on. Where a claim was checked against real headers, the
file and line are cited (`~/Retro68-build/toolchain/powerpc-apple-macos/include/`).

**Verdict:** scoping is sound, the phasing is right, and the Phase-0 DAE gate is correctly placed as
the kill switch. Four changes are worth making before any engine code exists, plus one factual fix.

---

## 1. The premise cites evidence that cuts both ways

§1: "iTunes / Apple CD Audio Player *can* play those same tracks on the Mini, digitally."

These are not equivalent evidence. **AppleCD Audio Player is a transport controller** — it issues the
same legacy audio csCodes to the drive that the games do, i.e. the analog path. iTunes is the DAE case.

- If AppleCD Audio Player really produces sound on the Mini, H1 and H2 are both wrong and the analog
  route is not the broken link.
- If it is silent while iTunes works, the §3 model is confirmed.

So it is a free Phase-0 discriminator, not supporting evidence. As written the doc leans on a claim
that, if true, contradicts its own diagnosis. → Phase 0 probe P1.

## 2. "Patch the DCE" is the right call, but for a reason the doc does not give

§5 prefers the DCE patch because it is "surgical", and offers `Get/SetOSTrapAddress` on
`_Control`/`_Status` as the easier alternative.

This project has already paid for that alternative. `reference_os9_init_resident_driver` records that a
68K `WaitNextEvent` trap patch (0xA860, hand-built ProcInfo 0x3F90) **installed but never fired**,
because the PPC-native Finder calls WNE through InterfaceLib and bypasses the 68K trap table. The same
reasoning applies to `_Control` for any PPC-native caller.

⇒ The trap route is a **68K-callers-only fallback**, and should be labelled as such with that reason.
The argument *for* the DCE patch is that it is caller-architecture-agnostic: everything funnels through
the DCE dispatch regardless of whether the caller is 68K or PPC.

⇒ Phase 0 must record, per target title, whether the caller is 68K or PPC-native (probe P5).

### 2a. But §5 under-specifies the hard part: classic `DRVR` vs native `ndrv`

`Devices.h:205` `AuxDCE` carries `dCtlNodeID` (a Name Registry `RegEntryID` — native drivers only), and
the ndrv command enum (`kOpenCommand`…`kControlCommand = 4`) sits immediately below it at
`Devices.h:230`. On a Mini G4 the optical driver is very likely **native**. "Save its original Control
entry" is two completely different mechanisms in the two cases, and the eSATA work already established
that a mis-shaped native dispatch field fails as a **garbage-UPP branch**, not a clean error
(`project_esata_sil3512`).

⇒ Add a Phase-0 step that dumps the CD driver's DCE and classifies it. One app run; it decides the
entire interception implementation. → probe P2.

### 2b. Resolve the driver by drive queue, not by the name `.AppleCD`

`GetDrvQHdr()` (`Disks.h:204`) → the optical drive's `dQRefNum` (`Disks.h:105`) → `GetDCtlEntry(refNum)`
(`Devices.h:1072`). Matching a name string is exactly the assumption that varies across ATAPI-era
driver builds.

## 3. §8 is probably wrong about needing CodeWarrior — a large de-risk

The doc reasons: resident PPC INIT → Retro68 cannot → CodeWarrior.

`reference_retro68_no_ppc_init` says Retro68 cannot build a PPC **code resource**. True, and not the
constraint here, because this project has a **hardware-proven Retro68 route to resident PPC code
installed at INIT-parade time**: a 68K INIT that `InstallDriverFromMemory`s a native-driver PEF, which
lands in the system Unit Table and survives the INIT's context being freed (proven 2026-07-08,
USB2 R2b-3, `reference_os9_init_resident_driver`).

That maps onto this project cleanly: **make the audio engine itself a native driver.** The 68K INIT
installs `.CDAudioRedirect`; its `kInitialize` locates the optical driver's DCE, saves the original
dispatch, installs our Control shim. Resident by a mechanism already validated on this hardware, and
Retro68 all the way. Worth reconfirming, but "almost certainly CodeWarrior" should not stand — it
changes the iteration cost of every later phase.

## 4. Two cheaper audio sources the doc does not consider

§5 commits to real-time DAE streaming off the CD. That inherits the worst risk in the design, and the
doc only half-names it: not seek latency, but **head contention with the game's own data reads through
the same drive**. Every level load fights the audio ring.

- **Audio CD Access / Foreign File Access.** Under classic Mac OS with QuickTime, audio CDs mount as a
  volume whose tracks appear as AIFF-readable files. If that also surfaces the audio session of a
  **mixed-mode** disc, DAE becomes a plain `FSRead` and the whole "READ CD (0xBE) through the ATA
  Manager" branch evaporates. → probe P3.
- **Rip-ahead to disk.** Stream from the hard disk instead of the CD; retires head contention entirely
  for ~40 MB per track. Cost is a first-play stall (a slim G4 drive at ~3.5 MB/s reads a 4-minute track
  in roughly 12 s), so the sane form is: stream from CD immediately *and* rip ahead in background, then
  switch source.

**The trade-off is real, so measure rather than guess.** CD-direct streaming via the ATA Manager async
path is interrupt-safe (`ATA.h`: `kATAFnExecIO` at :298, `ATAPICmdPacket` at :441, `ataPBPacketPtr` at
:657). File streaming needs the File Manager, which is task-level only — straight into
`reference_os9_no_filemgr_at_interrupt` — so it depends on the foreground game yielding often enough to
refill the ring. That is measurable in Phase 1, not decidable now.

## 5. Smaller corrections

- **The "mandatory" byte swap (§3) is probably not mandatory.** `Sound.h:472` defines
  `k16BitLittleEndianFormat = 'sowt'`, and `SndDoubleBufferHeader2` (`Sound.h:985`) carries a
  `dbhFormat` OSType. Try `'sowt'` and let the Sound Manager convert; keep the manual swap as fallback.
  Removes per-sample work from the interrupt-time doubleback proc, which is precisely where §5 says
  nothing expensive belongs.
- **Sound channel ownership is an unlisted crash risk.** `SndNewChannel` from driver context allocates
  in whatever heap is current — i.e. the *game's* heap. Game quits, channel goes with it, our resident
  engine holds a dangling pointer. Use `SetZone(SystemZone())` (the pattern already noted in
  `reference_os9_init_resident_driver`). Create the channel lazily on the first `AudioPlay` (arrives at
  task level from the app), not at `kInitialize`, where Sound Manager readiness during the INIT parade
  is an open question.
- **Games may bypass the Device Manager.** Some titles drive the drive through SCSI Manager
  pass-through; a DCE patch never sees those. The Phase-0 trace must cover both paths, or at minimum
  prove the target titles use `PBControl`. → probe P5.
- **`AudioControl` (volume) should be honored, not ignored** — scale samples in software, or in-game
  music sliders appear broken.
- **No capability guard is specified.** On a Mac that still has the analog wire, the extension should
  detect that and no-op rather than double-play — same discipline as the VBL fix's Classic-environment
  guard. Add that, the shift-key-disables convention, and a decision about user-facing on/off and
  volume UI (§2–§7 never say how a user configures or disables this).
- **Retro68 has no CD csCode constants** (grepped: nothing for `AudioTrackSearch`/`ReadTOC` anywhere in
  the toolchain). §3's instruction to pull the numbers from *Inside Macintosh: Devices* stands, and
  Phase 0 should produce one project-owned header rather than scattering literals.

## 6. The test-machine blocker is softer than §11 says

§11 says do not start Phase 1+ without a healthy Mini. Phases 1–3 do not need the Mini. The **MDD** can
validate the digital engine, the interception, and status/loop fidelity, with a clean discriminator:
mute the CD input in the Sound control panel, and if music still plays it is arriving through our
digital path. Only the final "analog really is absent" confirmation needs Mini hardware.

## 7. Phase 0, rewritten as five probes

Each with its discriminator stated before the run, per the project's hardware-test discipline.

| # | Probe | Discriminator |
|---|-------|---------------|
| P1 | iTunes vs AppleCD Audio Player on the Mini, same mixed-mode disc | iTunes audible + AppleCD AP silent ⇒ §3 model confirmed. Both audible ⇒ diagnosis is wrong, stop and re-scope. |
| P2 | DCE dump of the optical driver | `dCtlNodeID` valid / DriverLoaderLib identifies it ⇒ native ndrv (interception = native dispatch). Else classic `DRVR` (interception = entry offsets). |
| P3 | Does a mixed-mode disc expose its audio tracks as files? | Audio-track files visible and readable ⇒ DAE is `FSRead`, skip the 0xBE branch entirely. |
| P4 | Raw CD-DA sector read: ATA Manager ATAPI `READ CD (0xBE)` | 2352-byte sectors returning plausible PCM ⇒ **GO**. Nothing readable by any route ⇒ project blocked, fall back to §10. |
| P5 | Call trace while a broken game runs | Exact csCodes + params + return values; H1 (accepted, silent) vs H2 (rejected); caller 68K or PPC; Device Manager or SCSI path. |

P1–P3 are cheap enough to run in one sitting, and any of them can shrink the rest of the project
substantially. P4 remains the gate.

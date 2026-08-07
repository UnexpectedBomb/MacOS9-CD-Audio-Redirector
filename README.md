# OS 9 CD Audio Redirector

Red Book CD audio for legacy Mac CD games, on Macs that no longer have the analog CD-audio
wire.

## The problem

Early mixed-mode Mac CD games — the original *Warcraft: Orcs & Humans*, pre-1.09 *Quake* and
their contemporaries — play their music by asking the CD drive to play a Red Book audio track
out over an **analog** cable from the drive to the sound hardware. Later Macs, including the
G4 Mac mini, do not have that cable. Sound effects work; the music is silent.

The drive itself is fine, and the disc is fine: iTunes can read the same audio tracks
digitally. It is specifically the legacy game → CD-audio route that has nowhere to go.

## The approach

One background application that:

1. **Intercepts the CD driver's Control entry.** A single aligned store puts our TVector into
   the driver's Control routine descriptor. Nothing else is touched — not the DRVR header, not
   the driver name, not `dCtlDriver` — which is why iTunes and Audio CD Access keep working.
2. **Catches the legacy audio csCodes** (`AudioPlay`, `AudioTrackSearch`, `AudioPause`,
   `AudioStop`) and records what was asked for.
3. **Reads the audio track digitally** (DAE: `ChangeBlockSize(2352)` plus driver-level
   `PBRead`) and plays it through the **Sound Manager** with `SndPlayDoubleBuffer`.
4. **Synthesises the transport status.** This drive accepts the legacy audio calls and then
   silently does nothing, so its reported position never moves. A game polling for the end of
   a track would never see one — the community's "music never loops" symptom. The extension
   answers `AudioStatus` and `ReadQ` from its own playback cursor instead.

The work is split across two execution contexts for a reason discovered the hard way: **you
cannot do synchronous driver I/O from inside that driver's own Control entry.** The read
cannot start until the Control call returns, and the Control call is waiting on the read. So
the intercept only posts a request and chains; all I/O happens in an ordinary application at
task level.

## Status: NOT READY TO SHIP

The mechanism is proven on hardware. A legacy `AudioPlay` produces audible music, with a
truthful advancing position, on a G4 Mac mini with no analog CD-audio path — verified
repeatedly, with the position numbers checking out to the exact CD frame. Faceless
Startup-Items packaging works unattended from a cold boot with an empty tray.

A long freeze investigation is now resolved. The request ring lives in its own system-heap
allocation, which keeps the published block at 152 bytes; at 468 bytes the same machine froze
during playback, four runs out of four, for reasons still not understood. The current build
runs clean with all sixteen request slots and nothing dropped.

The governing rule for this project is that **the music must start every time it is supposed
to.** An extension that usually starts the music has not fixed the problem — it has reproduced
it, since the symptom being fixed *is* silence. By that standard this is not finished:

- a second `AudioPlay` for a **different track** has never been run;
- **natural end of track** has never executed on hardware;
- no **mixed-mode disc** (data track 1) has been tried;
- and three ten-second plays are not evidence of "every time".

`NEXT.md` is the current state and the next step. `FINDINGS.md` is the evidence, run by run,
including the failures and the conclusions that were later withdrawn.

## Layout

| path | what |
|---|---|
| `engine/` | The resident PowerPC engine (a PEF shaped as a native driver) and the pump application. One source builds both the faceless shipping shape and a diagnostic build |
| `probes/` | Hardware probes: driver recon, the legacy-audio driver probe that stands in for a game, the trace reader, the Control-entry dumper |
| `spike/` | The standalone DAE → Sound Manager streaming spike that proved the audio path |
| `patch/` | The retired 68K generation, kept for its call trace |
| `logs/` | Archived hardware run logs, one directory per run |
| `include/` | `cd_cscodes.h`, the project-owned CD csCode header — Retro68 has none |

## Building

PowerPC throughout, via [Retro68](https://github.com/autc04/Retro68):

```
cmake -S engine -B engine/build \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake --build engine/build
```

The `scripts/push-to-pi.sh` in each component stages built artifacts on a local file share for
transfer to the OS 9 machine, and is specific to this author's setup — it is not needed to
build.

## Credits

Written with [Claude Code](https://claude.com/claude-code). The CD csCode numbers were taken
from Basilisk II's `cdrom.cpp` dispatch table and then verified against real hardware; several
turned out to differ from the published *develop* documentation, which describes the older
SCSI AppleCD driver rather than the ATAPI one.

# Mac OS 9 CD Audio Redirector

Brings back the **Red Book CD music** in early Mac CD games, on Macs whose CD drive has no
analog audio path to the sound hardware. One file, dropped into Startup Items. Nothing
permanent is changed.

> ### Status: it works on hardware, and it has never met a real game
>
> On a G4 Mac mini with no analog CD-audio wire, the exact call a mixed-mode game makes
> (`AudioPlay`) now produces **audible music**, with a truthful advancing position and a proper
> end-of-track. That is verified over many runs, with the numbers checking out to the exact CD
> frame.
>
> **But every one of those runs used an ordinary audio CD, driven by a test program.** A real
> mixed-mode game disc, where track 1 is data and the game reads levels off it while the music
> plays, has **never been tried**. That is exactly the case this was built for, and it is the
> one thing that cannot be tested here.
>
> **If you have such a disc, you are the person this needs.** See
> [What to report back](#what-to-report-back).

---

## The problem

Early Mac CD games (the original *Warcraft: Orcs & Humans*, pre-1.09 *Quake*, and their
contemporaries) play their music by asking the CD drive to play a Red Book audio track out over
an **analog cable** running from the drive to the sound hardware.

Later Macs, including the G4 Mac mini, simply do not have that cable. Sound effects work, and
the music is silent.

Nothing is broken: the drive is fine and the disc is fine. iTunes can read the very same audio
tracks digitally. It is specifically the legacy game route that has nowhere to go.

## Do you need this?

You are a candidate if all of these are true:

- a PowerPC Mac running **Mac OS 9** (developed and tested on 9.2.2),
- whose CD or DVD drive has **no analog audio connection** to the sound hardware. The G4 Mac
  mini is the case this was written for,
- and an old CD game that **plays sound effects but no music**, where the music is on the disc
  as ordinary Red Book audio tracks (put the disc in a Mac with iTunes: if the tracks appear as
  an audio CD, that is the kind).

If your Mac still has the analog wire, you do not need this and should not install it.

## Install

1. Copy **`CDAudioRedirector_v10`** onto the OS 9 machine. In [dist/](dist/) there is a
   MacBinary `.bin` (easiest to transfer, decode with StuffIt Expander) and a disk image
   `.img` if you prefer to mount it.
2. Drag it into **`System Folder:Startup Items:`**
3. **Restart.**

That is the whole installation. It is a faceless background application, so nothing appears on
screen, it does not show up in the Application menu, and there is no control panel.

Then just run your game.

## How to tell it is working

The quiet way: put an audio CD in, launch the game, and listen.

The certain way: a log file called **`CD Audio Redirector Log`** appears in your System Folder.
Open it in SimpleText. Near the top of the last section you should see:

```
=== CD Audio Redirector v10
patch returned 0, status=0, patched=1
```

`patched=1` means it is installed and live.

There is also a test program, **`CDPlayProbe_v12`** in [dist/](dist/), which stands in for a
game: it issues the same legacy audio calls, plays a track, switches tracks, runs into a track
boundary, and then asks you whether you heard anything. Useful if you want to check the
extension without launching a game.

## How to remove it

1. Drag `CDAudioRedirector_v10` **out** of `System Folder:Startup Items:`
2. Restart.

Done. There is nothing else to undo.

## What it actually changes

Worth being precise, because this patches a driver:

- It changes **one field in memory**: the pointer inside the CD driver's Control routine
  descriptor, so that audio calls come to us first and are then passed straight on to Apple's
  driver.
- It does **not** modify the driver on disk, the System file, the ROM, or any other file.
- Everything it does lives in RAM and **disappears at restart**. If anything ever goes wrong,
  restarting puts the machine back exactly as it was, and removing the file from Startup Items
  stops it coming back.
- iTunes and the Finder keep working with audio CDs while it is installed. That was tested
  deliberately, because an earlier design broke iTunes and was thrown away for it.

## What is not tested yet

Being straight about this, because it is the whole reason for asking testers:

1. **A real mixed-mode game disc.** Every test so far used a plain audio CD. On a game disc,
   track 1 is data, and the game reads level data from it **while** music is playing. That
   contention has never been exercised.
2. **Any actual game.** A test program has stood in for one throughout. Real games may issue
   the calls in orders that have not come up.
3. **Any machine other than a G4 Mac mini.**
4. One internal detail is understood but not explained: the extension has to keep one of its
   memory blocks small, or the machine freezes during playback. Keeping it small is proven over
   many runs, but the underlying reason is still unknown. It is written up in
   [FINDINGS.md](FINDINGS.md) under 2026-08-07g in case it ever matters.

## What to report back

Whatever happens, the useful things are:

- **which Mac and which version of Mac OS 9**,
- **which game and which disc**, and whether the music played,
- the **`CD Audio Redirector Log`** file from your System Folder. Please send the whole file.
  It records what it found, what the game asked for, and what it did about it.
- if you used `CDPlayProbe_v12`, the **`CD Play Probe Log`** as well.

If it does **not** work, the log is far more useful than a description, because it distinguishes
"the extension never installed", "the game never asked for audio", and "the extension tried and
failed", which look identical from the outside.

If the machine freezes or behaves oddly, say so plainly and restart. Nothing is written to disk,
so a restart is always a clean recovery.

## How it works, briefly

1. It intercepts the CD driver's Control entry, changing a single pointer and passing every call
   through to Apple's driver afterwards.
2. When it sees a legacy audio call (`AudioPlay`, `AudioTrackSearch`, `AudioPause`,
   `AudioStop`), it records what was asked for.
3. A background pump reads the audio track **digitally** off the disc and plays it through the
   Sound Manager.
4. It answers the game's position queries from its own playback cursor. The unpatched driver
   accepts the audio calls and then does nothing, so its reported position never moves, which is
   why the usual symptom is music that never loops. The redirector reports a position that
   really does advance and a track end that really does arrive.

The reading and playing happen in a separate background application rather than inside the
driver patch, because a driver cannot wait for its own disk read to finish. That deadlock was
discovered the hard way and is why the design looks the way it does.

Full engineering detail, including everything that failed on the way, is in
[FINDINGS.md](FINDINGS.md). Current state and next steps are in [NEXT.md](NEXT.md).

## Building from source

PowerPC throughout, via [Retro68](https://github.com/autc04/Retro68):

```
cmake -S engine -B engine/build \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake --build engine/build
```

After a build, `scripts/stage-artifacts.sh` can copy the results somewhere the OS 9 machine
can reach. It is a convenience, it is not needed to build, and it does nothing unless you
point it at a destination of your own (see the comments at the top of that file).

## Credits

Written with [Claude Code](https://claude.com/claude-code).

The CD command numbers were taken from Basilisk II's `cdrom.cpp` dispatch table and then checked
against real hardware. Several turned out to differ from the published *develop* documentation,
which describes the older SCSI AppleCD driver rather than the ATAPI one, so the values here come
from what the drive actually did.

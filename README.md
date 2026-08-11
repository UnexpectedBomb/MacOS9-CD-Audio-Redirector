# Mac OS 9 CD Audio Redirector

Brings back the **Red Book CD music** in early Mac CD games, on Macs whose CD drive has no
analog audio path to the sound hardware. One file, dropped into Startup Items. Nothing
permanent is changed.

> ### ⚠ Status: works on an audio CD. **v10, the version below, does NOT work with a real game.**
>
> On a G4 Mac mini with no analog CD-audio wire, the exact call a mixed-mode game makes
> (`AudioPlay`) produces **audible music**, with a truthful advancing position and a proper
> end-of-track. That is verified over many runs, with the numbers checking out to the exact CD
> frame. But every one of those runs used a plain **audio CD**, and drove it with our own test
> program rather than a game.
>
> **Against a real mixed-mode game disc it does not work, and we now know exactly why.** Two
> separate defects have been found this way, both by the same tester, both invisible to any test
> we could run here:
>
> 1. The table-of-contents parser read the wrong half of the track type field, so it called the
>    disc's 261 MB *data* track an audio track and streamed it to the speakers. On an ordinary
>    audio CD that field is zero either way. Fixed in v10.
> 2. **The audio position encoding was wrong**, and our test program had the same error, so the
>    two agreed with each other and roughly thirty hardware runs confirmed nothing. A game asks
>    for a track in a form v10 mis-reads, which lands inside the data track, and v10 then
>    correctly refuses to play it. **The result is silence, with no error reported anywhere.**
>
> The second one was found by disassembling Apple's own `.AppleCD` driver, and the fix is
> written and built. **It is deliberately not published yet, because it has not been run on real
> hardware.** A disc to do that with is on its way here.
>
> **So: on an audio CD this works. On the mixed-mode game discs it was written for, v10 will be
> silent.** If that is your case, it is worth waiting for the next release rather than
> installing this one.
>
> Thanks to Jubadub on MacOS9Lives, who ran it twice, sent the logs that made both diagnoses
> possible, and worked out the first fault before we did. **Testing this is our job, not a
> tester's**, and the next version will be posted once it has been proven here.

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

⚠ **Read the status note at the top first.** The build in `dist/` is v10, which works on a plain
audio CD but is **silent with a real mixed-mode game disc**. If a game is why you are here, wait
for the next release.

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

Being straight about this:

1. **This build on a mixed-mode game disc: it does not work.** Two such runs have now happened,
   and each found a real defect. The first is fixed in v10. The second, the position encoding,
   is **not** fixed in v10 and is why v10 is silent with a game. The fix exists but has not been
   run on hardware, so it is not published. A disc to prove it with is on its way here, and
   verifying it is our job rather than a tester's.
2. **Data and audio at the same time.** On a game disc the game reads level data off track 1
   **while** music plays from the tracks after it. That contention has still never been
   exercised. `CDPlayProbe_v12` has a phase D that hunts it specifically, including the silent
   version where reads return the wrong bytes rather than an error.
3. **Any actual game.** A test program has stood in for one throughout. Real games may issue
   the calls in orders that have not come up.
4. **Any machine other than a G4 Mac mini.**
5. One internal detail is understood but not explained: the extension has to keep one of its
   memory blocks small, or the machine freezes during playback. Keeping it small is proven over
   many runs, but the underlying reason is still unknown. It is written up in
   [FINDINGS.md](FINDINGS.md) under 2026-08-07g in case it ever matters.

## What to report back

Not a request to go and test v10 against a game: we already know what that does, and proving the
fix is our job. This is here for anyone who runs it anyway, or who hits something unexpected on
an audio CD. If you do, the useful things are:

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

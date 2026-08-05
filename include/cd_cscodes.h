/*
 * cd_cscodes.h — Device Manager Control/Status codes for the Apple CD-ROM driver.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * Retro68's Universal Interfaces do NOT define any of these (grepped the whole
 * toolchain: no AudioTrackSearch, no ReadTOC, nothing). Every project that talks
 * to the CD driver therefore ends up with magic numbers sprinkled through it.
 * This is the one place those numbers live.
 *
 * PROVENANCE — VERIFIED ON HARDWARE 2026-08-05
 * --------------------------------------------
 * The csCode values were transcribed from the Control/Status dispatch of Basilisk
 * II's `BasiliskII/src/cdrom.cpp` (cebix/macemu), then checked against a real
 * driver: `.AppleCD` version 1.4.8, an ATAPI ('atpi') classic DRVR, on a G4 mini
 * under Mac OS 9. See FINDINGS.md for the raw evidence.
 *
 * ★ THE STATUS-vs-CONTROL SPLIT IS SETTLED, AND NOT THE WAY THE DOCS SAY.
 * Every audio and TOC call on this driver is a **Control** call. Issued as Status
 * they return -18 (statusErr): confirmed for 100 ReadTOC, 101 ReadQ,
 * 107 AudioStatus, 112 ReadAudioVolume, 126 GetPlayMode and 97 WhoIsThere.
 * Basilisk's table, which puts them all under Control, was right.
 *
 * Apple's `develop` issue 3 (MacTech mirror, "ROM Audio") says ReadTOC and
 * AudioStatus are *DStatus* subcalls. That describes the older SCSI AppleCD SC
 * driver and does **not** apply to the ATAPI-era `.AppleCD`. Do not "fix" this
 * header back to match that article.
 *
 * Also verified: the ReadTOC action code works as a **word** at csParam+0
 * (`csParam[0] = action`); the byte-at-offset-0 alternative was never needed.
 * AudioPlay/AudioTrackSearch accept posType = 0 with the MSF form — though on this
 * driver "accept" means noErr without the drive ever moving, so that pins down
 * what it parses, not what it acts on.
 *
 * csParam layouts are byte offsets into CntrlParam.csParam (which C declares as
 * short csParam[11], i.e. 22 bytes). Where a field's width or encoding is
 * ambiguous in the source material it is flagged AMBIGUOUS and CDRecon tries
 * more than one encoding.
 */

#ifndef CD_CSCODES_H
#define CD_CSCODES_H

/* ---- generic disk-driver Control codes (well documented, IM: Devices) ------ */
#define kcsKillIO               1
#define kcsVerifyTheDisc        5
#define kcsFormatTheDisc        6
#define kcsEjectTheDisc         7
#define kcsGetDriveIcon        21   /* csParam+0: icon address                  */
#define kcsGetMediaIcon        22   /* csParam+0: icon address                  */
#define kcsGetDriveInfo        23   /* csParam+0: drive info long               */
#define kcsAccRun              65   /* periodic action                          */

/* ---- Apple CD-ROM driver, non-audio Control ------------------------------- */
#define kcsSetPowerMode        70   /* csParam+0: mode byte                     */
#define kcsModifyPostEvent     76   /* csParam+0: flag word                     */
#define kcsChangeBlockSize     79   /* csParam+0: size word (512/2048; 2352?)   */
                                    /* ^ the DAE route-A lever: ask for 2352    */
#define kcsSetUserEject        80   /* csParam+0: flag word                     */
#define kcsSetPollFreq         81   /* uses dCtlDelay                           */

/* ---- Apple CD-ROM driver, TOC / Q sub-channel ----------------------------- */
/* CONTROL calls, verified. Both return -18 if issued as Status.
 *
 * ReadQ's 10 bytes, decoded from a live drive and cross-checked against the TOC
 * (the relative and absolute positions closed to the exact frame):
 *   [0] control/adr nibbles   [1] track (BCD)   [2] index (BCD)
 *   [3..5] track-relative M, S, F (BCD)
 *   [6..8] absolute M, S, F (BCD)
 * This is the layout our extension has to emulate. */
#define kcsReadTOC            100   /* csParam+0: action code — see below       */
#define kcsReadTheQSubcode    101   /* csParam+0..9: 10 bytes of subcode        */

/* ReadTOC action codes (csParam+0). AMBIGUOUS: whether the driver reads a byte
 * at offset 0 or a word at offset 0 is not established by the source material.
 * CDRecon writes both 0x000N (word) and 0x0N00 (byte-at-offset-0) and logs which
 * one the driver accepts. */
#define kTOCActionFirstLast     1   /* csParam+0: first track (BCD), +1: last   */
#define kTOCActionLeadOut       2   /* csParam+0..2: M, S, F   (+3 pad)         */
#define kTOCActionTrackAddrs    3   /* csParam+2: buffer addr (long),           */
                                    /* +6: buffer size — as a WORD: a long here */
                                    /*     would cover 6..9 and overlap the     */
                                    /*     track byte at +8, so the layout      */
                                    /*     cannot be long-at-6 plus byte-at-8   */
                                    /* csParam+8: starting track (BCD)          */
#define kTOCActionSessionInfo   5   /* csParam+0..1 first session, +2..3 last,  */
                                    /* +4 first track, +6..9 MSF                */

/* ---- Apple CD-ROM driver, the legacy audio surface we intercept ------------ */
/* These are the calls a mixed-mode game issues to play its music, and the whole
 * reason this project exists. ALL Control calls, verified on hardware.
 *
 * ★ On the G4 mini's .AppleCD 1.4.8, every one of these returns noErr and the
 * drive never moves: AudioTrackSearch does not seek, AudioPlay does not play, and
 * AudioStatus/ReadQ keep reporting a stale frozen position. Accepted and ignored.
 * So our interception cannot lean on the driver for playback position — the
 * extension must answer AudioStatus/ReadQ from its own playback cursor. */
#define kcsAudioTrackSearch   103   /* +0 postype, +2 position, +6 hold, +9 mode */
#define kcsAudioPlay          104   /* +0 postype, +2 position, +6 stop, +9 mode */
#define kcsAudioPause         105   /* +0: 0 = resume, 1 = pause                 */
#define kcsAudioStop          106   /* +0 postype, +2 position                   */
#define kcsAudioStatus        107   /* Control (NOT Status). +3..5 = absolute     */
                                    /* M, S, F in BCD, matching ReadQ's. +0..2    */
                                    /* were 0x00 throughout; presumably play      */
                                    /* state and mode, unconfirmed because the    */
                                    /* drive never played.                        */
#define kcsAudioScan          108   /* +0 postype, +2 position, +6 direction      */
#define kcsAudioControl       109   /* +0: left volume, +1: right volume          */
#define kcsReadAudioVolume    112   /* +0 left, +1 right (read-only)              */
#define kcsGetSpindleSpeed    113   /* +0: speed word                             */
#define kcsSetSpindleSpeed    114   /* +0: speed word                             */
#define kcsGetPlayMode        126   /* +0: mode word                              */

/* ---- Apple CD-ROM driver Status codes ------------------------------------- */
#define kcsReturnFormatList     6   /* +0 count, +2 address of format data      */
#define kcsDriveStatus          8   /* +0..21: 22-byte status record            */
#define kcsDriverGestalt       43   /* DriverGestaltParam, not CntrlParam       */
#define kcsGetPowerMode        70
#define kcsGet2KOffset         95   /* +0: offset word                          */
#define kcsGetDriveType        96   /* +0: type word (3 = CD300 or later)       */
#define kcsWhoIsThere          97   /* +1: bitmask of drives present            */
#define kcsGetBlockSize        98   /* +0: size word                            */
#define kcsReturnDeviceIdent  120   /* +0: ident word                           */
#define kcsGetCDFeatures      121   /* +0: speed, +2: features                  */

/* ---- CD-DA geometry (unambiguous, from the Red Book) ---------------------- */
#define kCDDASectorBytes     2352   /* 588 stereo frames of 16-bit signed PCM   */
#define kCDDAFramesPerSector  588
#define kCDDASectorsPerSec     75   /* ⇒ 176400 bytes/s                        */
#define kCDDABytesPerSec  (kCDDASectorBytes * kCDDASectorsPerSec)
#define kCDDALeadInSectors    150   /* MSF→LBA: (((M*60)+S)*75+F) - 150        */

/* CD-DA on the disc is 16-bit signed LITTLE-endian — CONFIRMED on hardware: the
 * sectors read back peak at 16450 read as LE (a normal music level) versus 32766
 * read as BE (pinned at clipping, the signature of wrong-endian noise), and
 * hand-decoding the samples agrees. The Sound Manager will take them as-is if
 * handed 'sowt' (k16BitLittleEndianFormat, Sound.h:472) via
 * SndDoubleBufferHeader2.dbhFormat (Sound.h:985), so the byte swap FEASIBILITY §3
 * calls mandatory is avoidable. Still to be proven end-to-end in Phase 1. */

/* BCD helpers — the TOC returns track numbers and MSF in BCD. */
#define kBCDToBin(b)  ((((b) >> 4) & 0x0F) * 10 + ((b) & 0x0F))
#define kBinToBCD(n)  ((unsigned char)((((n) / 10) << 4) | ((n) % 10)))

#endif /* CD_CSCODES_H */

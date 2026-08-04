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
 * PROVENANCE — READ THIS BEFORE TRUSTING A NUMBER
 * -----------------------------------------------
 * The csCode values below are transcribed from the Control/Status dispatch of
 * Basilisk II's `BasiliskII/src/cdrom.cpp` (cebix/macemu), which implements the
 * guest-visible Apple CD-ROM driver API. That is a re-implementation, not Apple
 * documentation, so treat the numbers as HIGH CONFIDENCE / NOT YET VERIFIED ON
 * HARDWARE until the Phase-0 recon run confirms them against a real driver.
 *
 * Independent cross-check on the Control/Status split, from Apple's `develop`
 * issue 3 (MacTech mirror, "ROM Audio"): audio playback is driven by five
 * *DControl* subcalls (AudioPlay, AudioPause, AudioScan, AudioStop, AudioSearch)
 * while disc/drive state comes from two *DStatus* subcalls (ReadTOC,
 * AudioStatus). Basilisk's table does not preserve that split cleanly, so the
 * STATUS-vs-CONTROL classification below is the weakest claim in this header.
 * CDRecon deliberately probes the read-only calls BOTH ways and logs both
 * results; the answer lands in PHASE0.md when the run comes back.
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
/* Per develop issue 3 these two are STATUS calls. Basilisk lists them in the
 * Control dispatch. Probed both ways. */
#define kcsReadTOC            100   /* csParam+0: action code — see below       */
#define kcsReadTheQSubcode    101   /* csParam+0..9: 10 bytes of subcode        */

/* ReadTOC action codes (csParam+0). AMBIGUOUS: whether the driver reads a byte
 * at offset 0 or a word at offset 0 is not established by the source material.
 * CDRecon writes both 0x000N (word) and 0x0N00 (byte-at-offset-0) and logs which
 * one the driver accepts. */
#define kTOCActionFirstLast     1   /* csParam+0: first track (BCD), +1: last   */
#define kTOCActionLeadOut       2   /* csParam+0..2: M, S, F   (+3 pad)         */
#define kTOCActionTrackAddrs    3   /* csParam+2: buffer addr, +6: buffer size, */
                                    /* csParam+8: starting track (BCD)          */
#define kTOCActionSessionInfo   5   /* csParam+0..1 first session, +2..3 last,  */
                                    /* +4 first track, +6..9 MSF                */

/* ---- Apple CD-ROM driver, the legacy audio surface we intercept ------------ */
/* These are the calls a mixed-mode game issues to play its music, and the whole
 * reason this project exists. Control unless noted. */
#define kcsAudioTrackSearch   103   /* +0 postype, +2 position, +6 hold, +9 mode */
#define kcsAudioPlay          104   /* +0 postype, +2 position, +6 stop, +9 mode */
#define kcsAudioPause         105   /* +0: 0 = resume, 1 = pause                 */
#define kcsAudioStop          106   /* +0 postype, +2 position                   */
#define kcsAudioStatus        107   /* STATUS per develop; +0..4 status fields    */
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

/* CD-DA on the disc is 16-bit signed LITTLE-endian. The Sound Manager will take
 * it as-is if handed 'sowt' (k16BitLittleEndianFormat, Sound.h:472) via
 * SndDoubleBufferHeader2.dbhFormat (Sound.h:985) — so the byte swap FEASIBILITY
 * §3 calls mandatory is probably avoidable. To be confirmed in Phase 1. */

/* BCD helpers — the TOC returns track numbers and MSF in BCD. */
#define kBCDToBin(b)  ((((b) >> 4) & 0x0F) * 10 + ((b) & 0x0F))
#define kBinToBCD(n)  ((unsigned char)((((n) / 10) << 4) | ((n) % 10)))

#endif /* CD_CSCODES_H */

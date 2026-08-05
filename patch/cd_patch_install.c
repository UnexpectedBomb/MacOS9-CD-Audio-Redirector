/*
 * CDPatchInstall — installs the Phase-2a patch on demand, from an application.
 *
 * WHY THIS EXISTS
 * ---------------
 * The first hardware boot of the 2a extension logged "no CD driver yet": the INIT ran
 * during the extension parade, found no CD driver, and correctly refused to patch.
 * That is the safe outcome, but it means the patch's actual mechanics — residency,
 * interception, transparent passthrough — could not be tested at all, and every
 * attempt to fix the timing would cost a reboot.
 *
 * This app breaks that dependency. It performs the identical install, from a normal
 * application at task level, long after every extension has loaded, every volume has
 * mounted and the drive has been enumerated. The patch it installs is the very same
 * 'CDpt' code resource the INIT uses, loaded the same way into the system heap, so
 * what gets tested is what will ship.
 *
 * ★ Residency without an INIT works because the blob is COPIED INTO A NewPtrSys BLOCK
 * and the copy is what runs, so the resident code is in memory we allocated in the
 * system heap ourselves and it survives this app quitting. The first version relied on
 * SetZone(SystemZone()) + Get1Resource + DetachResource + HLockHi and crashed the
 * machine into MacsBug: 'CDpt' was marked `preload`, so it had already been read into
 * this application's heap at launch, and the patch ended up pointing at app-heap code
 * that was freed the moment the app quit. See cd_blob_load.h.
 *
 * It also DIAGNOSES the boot-time failure, which the INIT could not do cheaply: it
 * dumps the whole unit table by name and the drive queue, so we learn whether
 * `.AppleCD` simply had not loaded yet, or whether it had loaded and only the drive
 * queue was empty. Those need different fixes and guessing would cost another reboot.
 *
 * 68K, so it can `$$read` the same `.flt` the extension does and share one Rez input.
 */

#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Events.h>
#include <Devices.h>
#include <Disks.h>
#include <Files.h>
#include <Folders.h>
#include <Memory.h>
#include <OSUtils.h>
#include <Resources.h>
#include <Script.h>
#include <ToolUtils.h>

#include "cd_patch_shell.h"
#include "cd_blob_load.h"

#define kOptionKeyCode 0x3A
#define KeyIsDown(km, code) \
    ((((unsigned char *)(km))[(code) >> 3] & (1 << ((code) & 7))) != 0)

#define LM_DrvQHdr          ((QHdrPtr)0x0308)
#define LM_UTableBase       (*(Ptr *)0x011C)
#define LM_UnitEntryCount   (*(short *)0x01D2)

/* ---- minimal logging (this app is 68K and does not link the probe common) ---- */

static short gLogRef  = 0;
static short gLogVRef = 0;

static void LogOpen(void)
{
    long   dirID;
    FSSpec spec;
    long   eof;

    if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder,
                   &gLogVRef, &dirID) != noErr) return;
    if (FSMakeFSSpec(gLogVRef, dirID, "\pCD Patch Log", &spec) != noErr) {
        if (FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript) != noErr) return;
    }
    if (FSpOpenDF(&spec, fsRdWrPerm, &gLogRef) != noErr) { gLogRef = 0; return; }
    if (GetEOF(gLogRef, &eof) == noErr) SetFPos(gLogRef, fsFromStart, eof);
}

static void LogStr(const char *s)
{
    char buf[256];
    long len = 0;

    if (gLogRef == 0) return;
    while (s[len] != 0 && len < (long)(sizeof(buf) - 2)) { buf[len] = s[len]; len++; }
    buf[len++] = '\r';
    FSWrite(gLogRef, &len, buf);
    FlushVol(NULL, gLogVRef);
}

/* No stdio in this 68K app, so numbers are formatted by hand. */
static void AppendNum(char *dst, long v)
{
    char  tmp[16];
    short n = 0, i = 0;
    long  d = v;

    while (dst[i] != 0) i++;
    if (d < 0) { dst[i++] = '-'; d = -d; }
    if (d == 0) tmp[n++] = '0';
    while (d > 0) { tmp[n++] = (char)('0' + (d % 10)); d /= 10; }
    while (n > 0) dst[i++] = tmp[--n];
    dst[i] = 0;
}

static void AppendStr(char *dst, const char *s)
{
    short i = 0, j = 0;
    while (dst[i] != 0) i++;
    while (s[j] != 0) dst[i++] = s[j++];
    dst[i] = 0;
}

static void AppendPStr(char *dst, const unsigned char *p)
{
    short i = 0, j;
    while (dst[i] != 0) i++;
    for (j = 1; j <= p[0]; j++) dst[i++] = (char)p[j];
    dst[i] = 0;
}

/* ---- diagnosis: what does the machine actually look like right now? -------- */

static Boolean GetUnitDriverName(short refNum, unsigned char *out, short outMax)
{
    DCtlHandle     dceH;
    DCtlPtr        dce;
    unsigned char *base;
    DRVRHeaderPtr  hdr;
    short          len, i;

    dceH = GetDCtlEntry(refNum);
    if (dceH == NULL || *dceH == NULL) return false;
    dce = *dceH;

    if (dce->dCtlFlags & dRAMBasedMask) {
        Handle hh = (Handle)dce->dCtlDriver;
        if (hh == NULL || *hh == NULL) return false;
        base = (unsigned char *)(*hh);
    } else {
        base = (unsigned char *)dce->dCtlDriver;
    }
    if (base == NULL || ((unsigned long)base & 1) ||
        (unsigned long)base < 0x1000)
        return false;

    hdr = (DRVRHeaderPtr)base;
    len = hdr->drvrName[0];
    if (len < 1 || len > 31 || len >= outMax) return false;
    for (i = 1; i <= len; i++) {
        unsigned char c = hdr->drvrName[i];
        if (c < 0x20 || c > 0x7E) return false;
    }
    for (i = 0; i <= len; i++) out[i] = hdr->drvrName[i];
    return true;
}

static void Diagnose(void)
{
    Ptr   utable = LM_UTableBase;
    short count  = LM_UnitEntryCount;
    short i;
    char  line[256];

    line[0] = 0;
    AppendStr(line, "--- unit table: ");
    AppendNum(line, count);
    AppendStr(line, " entries ---");
    LogStr(line);

    if (utable != NULL && count > 0 && count <= 256) {
        for (i = 0; i < count; i++) {
            unsigned char name[36];
            if (((DCtlHandle *)utable)[i] == NULL) continue;
            if (!GetUnitDriverName((short)~i, name, sizeof(name))) continue;
            line[0] = 0;
            AppendStr(line, "  unit ");
            AppendNum(line, i);
            AppendStr(line, " refNum ");
            AppendNum(line, (short)~i);
            AppendStr(line, "  '");
            AppendPStr(line, name);
            AppendStr(line, "'");
            LogStr(line);
        }
    }

    LogStr("--- drive queue ---");
    {
        DrvQElPtr q = (DrvQElPtr)LM_DrvQHdr->qHead;
        while (q != NULL) {
            line[0] = 0;
            AppendStr(line, "  drive ");
            AppendNum(line, q->dQDrive);
            AppendStr(line, " refNum ");
            AppendNum(line, q->dQRefNum);
            LogStr(line);
            q = (DrvQElPtr)q->qLink;
        }
    }
}

/* ---- hex helper ----------------------------------------------------------- */

static void AppendHexByte(char *dst, unsigned char v)
{
    static const char *hx = "0123456789ABCDEF";
    short i = 0;
    while (dst[i] != 0) i++;
    dst[i++] = hx[(v >> 4) & 0x0F];
    dst[i++] = hx[v & 0x0F];
    dst[i]   = 0;
}

static void AppendHexLong(char *dst, unsigned long v)
{
    short i;
    AppendStr(dst, "0x");
    for (i = 3; i >= 0; i--) AppendHexByte(dst, (unsigned char)(v >> (i * 8)));
}

static void LogHexRun(const char *tag, const unsigned char *p, short n)
{
    char  line[256];
    short i;
    line[0] = 0;
    AppendStr(line, "  ");
    AppendStr(line, tag);
    AppendStr(line, ": ");
    for (i = 0; i < n; i++) { AppendHexByte(line, p[i]); AppendStr(line, " "); }
    LogStr(line);
}

/* ---- find the CD driver by the same passive name scan the blob uses -------- */

static Boolean NameIsAppleCD(const unsigned char *p)
{
    static const char *want = ".APPLECD";
    short i;
    if (p[0] != 8) return false;
    for (i = 0; i < 8; i++) {
        unsigned char c = p[1 + i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        if (c != (unsigned char)want[i]) return false;
    }
    return true;
}

static short FindCDRefNum(void)
{
    Ptr   utable = LM_UTableBase;
    short count  = LM_UnitEntryCount;
    short i;

    if (utable == NULL || count <= 0 || count > 256) return 0;
    for (i = 0; i < count; i++) {
        unsigned char name[36];
        if (((DCtlHandle *)utable)[i] == NULL) continue;
        if (!GetUnitDriverName((short)~i, name, sizeof(name))) continue;
        if (NameIsAppleCD(name)) return (short)~i;
    }
    return 0;
}

/* ---- DRY RUN: everything the blob would look at, without touching anything -- *
 * This exists because CDPatchInstall crashed into MacsBug the first time it tried to
 * patch the REAL ATAPI driver, and there was no log to say how far it got. Every
 * earlier "successful" install was against a different, earlier incarnation of the
 * driver seen during the extension parade, so the shape we are about to modify has
 * never actually been recorded at the moment of modification. This records it, and
 * cannot crash the machine doing so. */
static void InspectDriver(void)
{
    short           refNum;
    DCtlHandle      dceH;
    DCtlPtr         dce;
    unsigned char  *base;
    DRVRHeaderPtr   hdr;
    char            line[256];
    short           offs[5];
    const char     *names[5];
    short           i;

    LogStr("--- DRY RUN: the driver we would patch ---");

    refNum = FindCDRefNum();
    if (refNum == 0) {
        LogStr("  no unit named '.AppleCD' found; nothing to inspect");
        return;
    }

    line[0] = 0; AppendStr(line, "  refNum "); AppendNum(line, refNum);
    LogStr(line);

    dceH = GetDCtlEntry(refNum);
    if (dceH == NULL || *dceH == NULL) { LogStr("  no DCE"); return; }
    dce = *dceH;

    line[0] = 0;
    AppendStr(line, "  dCtlFlags "); AppendHexLong(line, (unsigned long)(unsigned short)dce->dCtlFlags);
    AppendStr(line, "  dRAMBased ");
    AppendStr(line, (dce->dCtlFlags & dRAMBasedMask) ? "SET (Handle!)" : "clear (Ptr)");
    AppendStr(line, "  dCtlDelay "); AppendNum(line, dce->dCtlDelay);
    LogStr(line);

    line[0] = 0;
    AppendStr(line, "  dCtlDriver "); AppendHexLong(line, (unsigned long)dce->dCtlDriver);
    AppendStr(line, "  dCtlStorage "); AppendHexLong(line, (unsigned long)dce->dCtlStorage);
    LogStr(line);

    if (dce->dCtlFlags & dRAMBasedMask) {
        Handle hh = (Handle)dce->dCtlDriver;
        base = (hh && *hh) ? (unsigned char *)(*hh) : NULL;
    } else {
        base = (unsigned char *)dce->dCtlDriver;
    }
    if (base == NULL || ((unsigned long)base & 1) || (unsigned long)base < 0x1000) {
        LogStr("  driver pointer implausible; stopping");
        return;
    }

    hdr = (DRVRHeaderPtr)base;
    LogHexRun("header 0x00", base, 16);
    LogHexRun("header 0x10", base + 16, 16);

    offs[0] = hdr->drvrOpen;  names[0] = "open  ";
    offs[1] = hdr->drvrPrime; names[1] = "prime ";
    offs[2] = hdr->drvrCtl;   names[2] = "ctl   ";
    offs[3] = hdr->drvrStatus;names[3] = "status";
    offs[4] = hdr->drvrClose; names[4] = "close ";

    for (i = 0; i < 5; i++) {
        unsigned char *e = base + (unsigned short)offs[i];
        unsigned short op;

        line[0] = 0;
        AppendStr(line, "  entry ");
        AppendStr(line, names[i]);
        AppendStr(line, " offset ");
        AppendHexLong(line, (unsigned long)(unsigned short)offs[i]);
        AppendStr(line, " -> ");
        AppendHexLong(line, (unsigned long)e);
        LogStr(line);

        if ((unsigned long)e < 0x1000 || ((unsigned long)e & 1)) {
            LogStr("    (implausible; not reading it)");
            continue;
        }
        LogHexRun("    bytes", e, 16);
        op = (unsigned short)((e[0] << 8) | e[1]);
        line[0] = 0;
        AppendStr(line, "    first word ");
        AppendHexLong(line, (unsigned long)op);
        AppendStr(line, op == 0xAAFE ? "  = 0xAAFE, Mixed Mode descriptor"
                                     : "  = NOT a Mixed Mode descriptor");
        LogStr(line);
        if (op == 0xAAFE) {
            line[0] = 0;
            AppendStr(line, "    ISA ");
            AppendHexLong(line, (unsigned long)e[13]);
            AppendStr(line, " (0=68K 1=PPC)  TVector ");
            AppendHexLong(line, ((unsigned long)e[16] << 24) |
                                ((unsigned long)e[17] << 16) |
                                ((unsigned long)e[18] << 8)  | e[19]);
            LogStr(line);
        }
    }

    LogHexRun("header tail 0x12", base + 0x12, 14);
    LogStr("  (that tail is name + padding + version; the shell copies it verbatim)");
    LogStr("--- END DRY RUN: nothing was modified ---");
}

/* ---- the install ---------------------------------------------------------- */

static const char *ResultText(short r)
{
    switch (r) {
        case kInstallOK:             return "INSTALLED";
        case kInstallNoDriver:       return "no CD driver found";
        case kInstallNoDCE:          return "no DCE";
        case kInstallBadDriverPtr:   return "dCtlDriver implausible";
        case kInstallNotDRVRShape:   return "not a DRVR shape, refused";
        case kInstallNoMemory:       return "out of system memory";
        case kInstallAlreadyPatched: return "already patched";
        case kInstallRAMBased:       return "driver is Handle-based, refused";
        case kInstallNotATAPIDriver: return "not the ATAPI driver yet (too early - install post-boot)";
        default:                     return "unknown result";
    }
}

static void ShowResult(const char *what)
{
    Str255 msg;
    short  i = 0;

    while (what[i] != 0 && i < 200) { msg[i + 1] = (unsigned char)what[i]; i++; }
    msg[0] = (unsigned char)i;
    ParamText(msg, "\p", "\p", "\p");
    NoteAlert(128, NULL);
}

int main(void)
{
    short   result;
    char    line[256];
    KeyMap  km;
    Boolean commit;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    GetKeys(km);
    commit = KeyIsDown(km, kOptionKeyCode);

    LogOpen();
    LogStr("");
    if (commit)
        LogStr("=== CDPatchInstall: OPTION HELD, will COMMIT the patch ===");
    else
        LogStr("=== CDPatchInstall: DRY RUN (hold option to actually patch) ===");

    Diagnose();
    InspectDriver();

    /* ★ The default is now a dry run. The first attempt to patch the real ATAPI driver
     * crashed into MacsBug with no log, so the safe action became the default one and
     * patching became the deliberate one. */
    if (!commit) {
        LogStr("dry run complete; nothing was modified");
        if (gLogRef != 0) { FSClose(gLogRef); gLogRef = 0; }
        ShowResult("DRY RUN complete - nothing was changed. Details are in "
                   "'CD Patch Log' in the System Folder. Hold OPTION when launching "
                   "to actually install the patch.");
        return 0;
    }

    /* Copy the blob into a system-heap block and run the copy. The earlier version
     * relied on SetZone(SystemZone()) + Get1Resource + DetachResource + HLockHi, and
     * that crashed the machine: 'CDpt' was marked `preload`, so it had already been
     * read into this application's own heap at launch, and the installed patch ended
     * up pointing at app-heap code that vanished when the app quit. See
     * cd_blob_load.h. */
    {
        Ptr   code = NULL;
        long  size = 0;
        short lerr = CDLoadBlobToSysHeap(&code, &size);

        if (lerr != kBlobLoadOK) {
            line[0] = 0;
            AppendStr(line, "could not load 'CDpt' 128 into the system heap, err ");
            AppendNum(line, lerr);
            LogStr(line);
            ShowResult("Could not load the patch blob into the system heap.");
            return 0;
        }

        line[0] = 0;
        AppendStr(line, "blob copied to system heap at 0x");
        AppendNum(line, (long)code);
        AppendStr(line, ", size ");
        AppendNum(line, size);
        LogStr(line);

        result = CDCallBlob(code);

        if (result != kInstallOK) {
            /* Nothing was patched, so nothing points into the copy. */
            DisposePtr(code);
        }
    }

    line[0] = 0;
    AppendStr(line, "install result: ");
    AppendStr(line, ResultText(result));
    LogStr(line);

    {
        char msg[256];
        msg[0] = 0;
        AppendStr(msg, "Patch install: ");
        AppendStr(msg, ResultText(result));
        AppendStr(msg, ".  Details in 'CD Patch Log' in the System Folder. "
                       "Now run CDTraceDump to read the trace.");
        ShowResult(msg);
    }

    if (gLogRef != 0) { FSClose(gLogRef); gLogRef = 0; }
    return 0;
}

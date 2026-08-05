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
 * Residency without an INIT works because the blob is detached and locked in the
 * SYSTEM heap: it belongs to the system, not to this application, and it survives
 * this app quitting. (Same reasoning as `reference_os9_init_resident_driver`'s note
 * that a held CFM connection does not survive but a system-owned installation does.)
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

#define kBlobType   FOUR_CHAR_CODE('CDpt')
#define kBlobID     128

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
    Handle blob;
    THz    saveZone;
    short  result;
    char   line[256];

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    LogOpen();
    LogStr("");
    LogStr("=== CDPatchInstall: installing from an application (post-boot) ===");

    Diagnose();

    saveZone = GetZone();
    SetZone(SystemZone());
    blob = Get1Resource(kBlobType, kBlobID);
    if (blob == NULL || *blob == NULL) {
        SetZone(saveZone);
        LogStr("patch blob resource 'CDpt' 128 missing from this app");
        ShowResult("The patch blob is missing from this application.");
        return 0;
    }
    DetachResource(blob);
    HLockHi(blob);
    SetZone(saveZone);

    /* Entry at offset 0, returns a status word, relocates itself on this one call. */
    result = (*(short (*)(void))(*blob))();

    line[0] = 0;
    AppendStr(line, "install result: ");
    AppendStr(line, ResultText(result));
    LogStr(line);

    if (result != kInstallOK) {
        HUnlock(blob);
        DisposeHandle(blob);
    }

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

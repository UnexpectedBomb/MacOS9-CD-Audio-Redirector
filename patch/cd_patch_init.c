/*
 * cd_patch_init.c — Phase 2a: the INIT that makes the patch resident.
 *
 * Deliberately thin. All it does is get the patch blob into the system heap and call
 * it once; every interesting decision lives in the blob, which is the half that has
 * to survive. INIT code is discarded when the INIT returns, so nothing that must
 * persist can live here.
 *
 * Residency: copy the 'CDpt' blob into a NewPtrSys block and call the copy, so the
 * code lives in memory we allocated in the system heap ourselves. The earlier
 * SetZone(SystemZone()) + Get1Resource + DetachResource + HLockHi recipe is not good
 * enough — it depends on which heap is current when the Resource Manager happens to
 * read the resource, and with `preload` set that had already happened. From the
 * installer app that put the resident code in the APP heap and crashed the machine as
 * soon as the app quit. See cd_blob_load.h.
 *
 * ★ Escape hatch: hold SHIFT at boot and nothing is installed. This is the standard
 * INIT convention and it is the only recovery path if a patched boot goes bad, so it
 * is checked before anything else happens.
 *
 * ★ Load order: extensions load in an undefined order, and if `.AppleCD` has not
 * loaded yet there is nothing to patch. The blob detects that and returns
 * kInstallNoDriver, which is a clean no-op rather than a broken machine. The fix is
 * to make this extension sort late — the shipped filename begins with "~", which is
 * 0x7E and therefore after every letter in the HFS sort.
 *
 * A one-line result goes into "CD Patch Log" in the System Folder. Logging at INIT
 * time is task level and is the documented way to localise a boot problem on this
 * project; each line is flushed, so a hang after this point still leaves the reason
 * on disc.
 */

#include <MacTypes.h>
#include <Files.h>
#include <Folders.h>
#include <Memory.h>
#include <OSUtils.h>
#include <Resources.h>
#include <Events.h>
#include <Script.h>         /* smSystemScript */
#include "Retro68Runtime.h"

#include "cd_patch_shell.h"
#include "cd_blob_load.h"

#define kShiftKeyCode 0x38
#define KeyIsDown(km, code) \
    ((((unsigned char *)(km))[(code) >> 3] & (1 << ((code) & 7))) != 0)

/* Append one line to "CD Patch Log" and flush it. Best effort throughout: a logging
 * failure must never be the reason a boot goes wrong. */
static void LogLine(const char *msg)
{
    short  vRefNum;
    long   dirID;
    FSSpec spec;
    short  ref;
    long   eof, len;
    char   buf[192];
    short  n = 0;

    while (msg[n] != 0 && n < (short)(sizeof(buf) - 2)) { buf[n] = msg[n]; n++; }
    buf[n++] = '\r';

    if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return;
    if (FSMakeFSSpec(vRefNum, dirID, "\pCD Patch Log", &spec) != noErr) {
        if (FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript) != noErr) return;
    }
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) return;
    if (GetEOF(ref, &eof) == noErr) SetFPos(ref, fsFromStart, eof);
    len = n;
    FSWrite(ref, &len, buf);
    FSClose(ref);
    FlushVol(NULL, vRefNum);
}

static const char *ResultText(short r)
{
    switch (r) {
        case kInstallOK:             return "CD Patch 2a: INSTALLED";
        case kInstallNoDriver:       return "CD Patch 2a: no CD driver yet "
                                            "(load order - rename to sort later)";
        case kInstallNoDCE:          return "CD Patch 2a: no DCE";
        case kInstallBadDriverPtr:   return "CD Patch 2a: dCtlDriver implausible";
        case kInstallNotDRVRShape:   return "CD Patch 2a: not a DRVR shape, refused";
        case kInstallNoMemory:       return "CD Patch 2a: out of system memory";
        case kInstallAlreadyPatched: return "CD Patch 2a: already patched";
        case kInstallRAMBased:       return "CD Patch 2a: driver is Handle-based, "
                                            "refused";
        default:                     return "CD Patch 2a: unknown result";
    }
}

void _start(void)
{
    KeyMap  km;
    short   result;

    RETRO68_RELOCATE();
    Retro68CallConstructors();

    GetKeys(km);
    if (KeyIsDown(km, kShiftKeyCode)) {
        LogLine("CD Patch 2a: shift held, not installed");
        Retro68FreeGlobals();
        return;
    }

    {
        Ptr   code = NULL;
        short lerr = CDLoadBlobToSysHeap(&code, NULL);

        if (lerr != kBlobLoadOK) {
            LogLine("CD Patch 2a: could not load the patch blob into the "
                    "system heap");
            Retro68FreeGlobals();
            return;
        }

        result = CDCallBlob(code);
        LogLine(ResultText(result));

        if (result != kInstallOK) {
            /* Nothing was patched, so nothing points into the copy. */
            DisposePtr(code);
        }
    }

    Retro68FreeGlobals();
}

/*
 * cd_blob_load.h — get the resident patch blob into the SYSTEM heap, reliably.
 *
 * ★ WHY THIS IS NOT JUST Get1Resource + DetachResource + HLockHi
 *
 * That was the first attempt and it crashed the machine into MacsBug. The 'CDpt'
 * resource was marked `preload` in the .r, and a preloaded resource is loaded when
 * the resource FILE is opened — for an application, at launch, into the APPLICATION
 * heap — long before any `SetZone(SystemZone())` in our code can influence anything.
 * So `Get1Resource` returned a handle that was already in the app heap,
 * `DetachResource` and `HLockHi` faithfully kept it there, and the installed patch
 * pointed at code inside the application's own heap. The instant the app quit, that
 * memory was freed while the CD driver's `dCtlDriver` still pointed into it, and the
 * next Control call jumped into freed memory.
 *
 * The `preload` attribute is gone now, but relying on "whatever zone is current when
 * the Resource Manager happens to read this" is a fragile thing to build residency
 * on: it depends on load order, on whether something else already faulted the
 * resource in, and on which container we are running from.
 *
 * So we stop depending on it. Copy the code into a block we allocated in the system
 * heap ourselves, and call the copy. `NewPtrSys` is unambiguous about where the
 * memory lives, a pointer-based block cannot be moved by the Memory Manager, and
 * nothing can free it out from under us.
 *
 * This is safe because the blob is position-independent in the two ways that matter:
 * its entry trampoline at offset 0 is `NOP / BSR / ADDI.L #delta,(SP) / RTS`, which
 * computes its target from the pushed return address, and `Retro68Relocate` derives
 * the relocation base from where the code is actually executing. Verified by
 * disassembling the linked blob.
 */

#ifndef CD_BLOB_LOAD_H
#define CD_BLOB_LOAD_H

#include <MacTypes.h>
#include <Memory.h>
#include <Resources.h>

#define kCDBlobType   FOUR_CHAR_CODE('CDpt')
#define kCDBlobID     128

/* Result codes distinct from the installer's, so a failure to even load the blob is
 * never mistaken for a failure to patch. */
enum {
    kBlobLoadOK       = 0,
    kBlobMissing      = -1,
    kBlobEmpty        = -2,
    kBlobNoSysMemory  = -3
};

/* Copies 'CDpt' 128 into a fresh system-heap block and returns it through *codeOut.
 * The resource handle is released before returning, so nothing is left tying the
 * resident code to the container file or to the caller's heap. */
static short CDLoadBlobToSysHeap(Ptr *codeOut, long *sizeOut)
{
    Handle res;
    Size   sz;
    Ptr    code;

    *codeOut = NULL;
    if (sizeOut) *sizeOut = 0;

    res = Get1Resource(kCDBlobType, kCDBlobID);
    if (res == NULL) return kBlobMissing;

    sz = GetResourceSizeOnDisk(res);
    if (sz <= 0) {
        LoadResource(res);
        if (*res == NULL) { ReleaseResource(res); return kBlobEmpty; }
        sz = GetHandleSize(res);
    }
    if (sz <= 0) { ReleaseResource(res); return kBlobEmpty; }

    LoadResource(res);
    if (*res == NULL) { ReleaseResource(res); return kBlobEmpty; }

    code = NewPtrSys(sz);
    if (code == NULL) { ReleaseResource(res); return kBlobNoSysMemory; }

    BlockMoveData(*res, code, sz);

    /* The copy is what will run, so the resource itself is no longer needed. */
    ReleaseResource(res);

    *codeOut = code;
    if (sizeOut) *sizeOut = (long)sz;
    return kBlobLoadOK;
}

/* Call the blob's entry, which is at offset 0 and returns a status word. It
 * relocates itself on this call, so it must be called exactly once per copy. */
static short CDCallBlob(Ptr code)
{
    return (*(short (*)(void))code)();
}

#endif /* CD_BLOB_LOAD_H */

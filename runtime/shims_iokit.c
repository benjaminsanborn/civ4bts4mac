/* IOKit: the game reads the GPU's PCI registry properties (model, VRAM, IDs)
 * and the CPU clock. We present one era-appropriate GPU the game knows, so its
 * vendor/model specific paths behave as on a supported 2008 Mac. */
#include "cf.h"

#include <string.h>

#define GPU_SERVICE 0x1001u
#define GPU_PCI_DEVICE 0x1002u

static const char kModel[] = "NVIDIA GeForce 8800 GT";
static const uint32_t kVendor = 0x10de, kDevice = 0x0611, kVRAM = 512u << 20;

SHIM(_CGDisplayIOServicePort) { RET(GPU_SERVICE); }
SHIM(_IORegistryEntryGetParentEntry) {
    /* (entry, plane, io_registry_entry_t *parent) */
    W32_(g_mem, ARG(2), ARG(0) == GPU_SERVICE ? GPU_PCI_DEVICE : 0);
    RET(ARG(0) == GPU_SERVICE ? 0 : 0xe00002c2 /* kIOReturnNotFound */);
}
SHIM(_IOObjectConformsTo) { RET(ARG(0) == GPU_PCI_DEVICE && !strcmp(CPTR(ARG(1)), "IOPCIDevice")); }
SHIM(_IORegistryEntryCreateCFProperty) {
    /* (entry, CFStringRef key, allocator, options) */
    char key[128] = "";
    CFStringGetCString(CFI(1), key, sizeof key, kCFStringEncodingUTF8);
    CFTypeRef v = NULL;
    if (!strcmp(key, "model")) v = CFDataCreate(NULL, (const UInt8 *)kModel, sizeof kModel);
    else if (!strcmp(key, "vendor-id")) v = CFDataCreate(NULL, (const UInt8 *)&kVendor, 4);
    else if (!strcmp(key, "device-id")) v = CFDataCreate(NULL, (const UInt8 *)&kDevice, 4);
    else if (!strcmp(key, "VRAM,totalsize") || !strcmp(key, "IOFBMemorySize") || !strcmp(key, "VRAM,totalMB")) {
        uint32_t vram = strcmp(key, "VRAM,totalMB") ? kVRAM : kVRAM >> 20;
        v = CFDataCreate(NULL, (const UInt8 *)&vram, 4);
    } else {
        rt_log("IORegistryEntryCreateCFProperty: %s (not provided)", key);
    }
    RETCF_OWNED(v);
}
/* The CPU-speed probe walks IOService matches; report none (Gestalt answers instead). */
SHIM(_IOMasterPort) { W32_(g_mem, ARG(1), 0); RET(0); }
SHIM(_IOServiceNameMatching) { RET(0); }
SHIM(_IOServiceGetMatchingServices) { W32_(g_mem, ARG(2), 0); RET(0xe00002c2); }
SHIM(_IOIteratorNext) { RET(0); }
SHIM(_IOObjectRelease) { RET(0); }

const ShimDef shims_iokit[] = {
    SHIM_ENTRY(_CGDisplayIOServicePort), SHIM_ENTRY(_IORegistryEntryGetParentEntry), SHIM_ENTRY(_IOObjectConformsTo),
    SHIM_ENTRY(_IORegistryEntryCreateCFProperty), SHIM_ENTRY(_IOMasterPort), SHIM_ENTRY(_IOServiceNameMatching),
    SHIM_ENTRY(_IOServiceGetMatchingServices), SHIM_ENTRY(_IOIteratorNext), SHIM_ENTRY(_IOObjectRelease),
    {0, 0},
};

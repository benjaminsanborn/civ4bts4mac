/* CoreGraphics images and QuickTime's JPEG exporter, as used by the game's
 * JPEG texture loader (Mac_LoadTextureFromJPEGMem) and screenshot writer
 * (Mac_SaveTextureToJPEG). Guest pixel buffers are host-addressable, so the
 * CG objects work on guest memory directly; they cross as CF proxies. */
#include "cf.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <stdlib.h>
#include <string.h>

#include "handles.h"

#define TAG_EXPORTER 0x47455850u /* 'GEXP' */
#define noErr 0
#define paramErr (-50)

static CFTypeRef cfi_or_null(uint32_t g) { return g ? cf_in(g) : NULL; }
static void release_arg(uint32_t g) {
    if (g) CFRelease(cf_in(g));
}

SHIM(_CGColorSpaceCreateWithName) { RETCF_OWNED(CGColorSpaceCreateWithName(CFI(0))); }
SHIM(_CGColorSpaceRelease) { release_arg(ARG(0)); }

SHIM(_CGBitmapContextCreate) {
    /* (data, width, height, bitsPerComponent, bytesPerRow, colorspace, bitmapInfo) */
    CGContextRef c = CGBitmapContextCreate(ARG(0) ? PTR(ARG(0)) : NULL, ARG(1), ARG(2), ARG(3), ARG(4),
                                           (CGColorSpaceRef)cfi_or_null(ARG(5)), ARG(6));
    RETCF_OWNED(c);
}
SHIM(_CGBitmapContextCreateImage) { RETCF_OWNED(CGBitmapContextCreateImage((CGContextRef)CFI(0))); }
SHIM(_CGContextRelease) { release_arg(ARG(0)); }
SHIM(_CGContextDrawImage) {
    /* (context, CGRect by value: 4 floats, image) */
    CGContextDrawImage((CGContextRef)CFI(0), CGRectMake(ARGF32(1), ARGF32(2), ARGF32(3), ARGF32(4)),
                       (CGImageRef)CFI(5));
}

SHIM(_CGDataProviderCreateWithData) {
    /* (info, data, size, releaseData): the guest keeps ownership of its buffer
     * and frees it after releasing the provider (Mac_LoadTextureFromJPEGMem). */
    RETCF_OWNED(CGDataProviderCreateWithData(NULL, PTR(ARG(1)), ARG(2), NULL));
}
SHIM(_CGDataProviderRelease) { release_arg(ARG(0)); }
SHIM(_CGImageCreateWithJPEGDataProvider) {
    /* (provider, const CGFloat *decode, bool shouldInterpolate, CGColorRenderingIntent) */
    RETCF_OWNED(CGImageCreateWithJPEGDataProvider((CGDataProviderRef)CFI(0), NULL, (bool)(ARG(2) & 0xff),
                                                  (CGColorRenderingIntent)ARG(3)));
}
SHIM(_CGImageGetWidth) { RET((uint32_t)CGImageGetWidth((CGImageRef)CFI(0))); }
SHIM(_CGImageGetHeight) { RET((uint32_t)CGImageGetHeight((CGImageRef)CFI(0))); }
SHIM(_CGImageGetBitsPerPixel) { RET((uint32_t)CGImageGetBitsPerPixel((CGImageRef)CFI(0))); }
SHIM(_CGImageRelease) { release_arg(ARG(0)); }

/* ---- QuickTime graphics exporter (JPEG only, written with ImageIO) ---- */
typedef struct {
    uint32_t g, subtype;
    CGImageRef image;
    char path[1024];
} Exporter;

SHIM(_OpenADefaultComponent) {
    /* (OSType type, OSType subtype, ComponentInstance *out) */
    if (ARG(0) != 'grex') {
        rt_log("OpenADefaultComponent: unsupported component '%c%c%c%c'", ARG(0) >> 24, ARG(0) >> 16 & 255,
               ARG(0) >> 8 & 255, ARG(0) & 255);
        W32_(g_mem, ARG(2), 0);
        RET(-2003 /* cantFindHandler */);
        return;
    }
    Exporter *e = calloc(1, sizeof *e);
    e->subtype = ARG(1);
    e->g = gh_new(TAG_EXPORTER, e);
    W32_(g_mem, ARG(2), e->g);
    RET(noErr);
}
SHIM(_CloseComponent) {
    Exporter *e = gh_get(ARG(0), TAG_EXPORTER);
    if (!e) { RET(noErr); return; }
    if (e->image) CGImageRelease(e->image);
    gh_free(e->g);
    free(e);
    RET(noErr);
}
SHIM(_GraphicsExportSetInputCGImage) {
    Exporter *e = gh_get(ARG(0), TAG_EXPORTER);
    if (!e) { RET(paramErr); return; }
    if (e->image) CGImageRelease(e->image);
    e->image = (CGImageRef)CGImageRetain((CGImageRef)CFI(1));
    RET(noErr);
}
SHIM(_GraphicsExportSetOutputDataReference) {
    /* (exporter, Handle dataRef, OSType dataRefType): our data references hold a host path (movies.m) */
    Exporter *e = gh_get(ARG(0), TAG_EXPORTER);
    if (!e || !ARG(1)) { RET(paramErr); return; }
    strlcpy(e->path, CPTR(R32_(g_mem, ARG(1))), sizeof e->path);
    RET(noErr);
}
SHIM(_GraphicsExportDoExport) {
    /* (exporter, unsigned long *actualSizeWritten) */
    Exporter *e = gh_get(ARG(0), TAG_EXPORTER);
    if (!e || !e->image || !e->path[0]) { RET(paramErr); return; }
    CFStringRef type = e->subtype == 'PNGf' ? CFSTR("public.png") : CFSTR("public.jpeg");
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)e->path, (CFIndex)strlen(e->path), false);
    CGImageDestinationRef d = url ? CGImageDestinationCreateWithURL(url, type, 1, NULL) : NULL;
    int ok = 0;
    if (d) {
        CGImageDestinationAddImage(d, e->image, NULL);
        ok = CGImageDestinationFinalize(d);
        CFRelease(d);
    }
    if (url) CFRelease(url);
    if (!ok) rt_log("GraphicsExportDoExport: could not write %s", e->path);
    if (ARG(1)) W32_(g_mem, ARG(1), 0);
    RET(ok ? noErr : -36 /* ioErr */);
}

void rt_cg_image_init(void) {
    uint32_t a = rt_hostdata("_kCGColorSpaceGenericRGB");
    if (a) W32_(g_mem, a, cf_out(kCGColorSpaceGenericRGB));
}

const ShimDef shims_cg_image[] = {
    SHIM_ENTRY(_CGColorSpaceCreateWithName), SHIM_ENTRY(_CGColorSpaceRelease), SHIM_ENTRY(_CGBitmapContextCreate),
    SHIM_ENTRY(_CGBitmapContextCreateImage), SHIM_ENTRY(_CGContextRelease), SHIM_ENTRY(_CGContextDrawImage),
    SHIM_ENTRY(_CGDataProviderCreateWithData), SHIM_ENTRY(_CGDataProviderRelease),
    SHIM_ENTRY(_CGImageCreateWithJPEGDataProvider), SHIM_ENTRY(_CGImageGetWidth), SHIM_ENTRY(_CGImageGetHeight),
    SHIM_ENTRY(_CGImageGetBitsPerPixel), SHIM_ENTRY(_CGImageRelease), SHIM_ENTRY(_OpenADefaultComponent),
    SHIM_ENTRY(_CloseComponent), SHIM_ENTRY(_GraphicsExportSetInputCGImage),
    SHIM_ENTRY(_GraphicsExportSetOutputDataReference), SHIM_ENTRY(_GraphicsExportDoExport),
    {0, 0},
};

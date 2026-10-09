/* CFXML (CFXMLTree / CFXMLNode / CFTree) and Carbon standard alerts.
 *
 * The game parses its XML through these deprecated CoreFoundation APIs, which
 * still exist on the host. Node "info" structs contain host pointers, so the
 * guest gets i386 copies holding CF proxies (cached per node). */
#include "cf.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define noErr 0

/* ---- node info structs (i386 layouts) ----
 * Document:      {CFURLRef sourceURL; CFStringEncoding encoding}
 * Element:       {CFDictionaryRef attributes; CFArrayRef attributeOrder; Boolean isEmpty; char pad[3]}
 * ProcInstr:     {CFStringRef dataString}
 * DocumentType:  {CFURLRef systemID; CFStringRef publicID}
 * EntityRef:     {CFXMLEntityTypeCode entityType} */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { CFXMLNodeRef node; uint32_t info; } *g_infos;
static size_t g_ninfo, g_capinfo;

static uint32_t guest_info(CFXMLNodeRef node) {
    const void *info = CFXMLNodeGetInfoPtr(node);
    if (!info) return 0;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_ninfo; i++)
        if (g_infos[i].node == node) {
            uint32_t g = g_infos[i].info;
            pthread_mutex_unlock(&g_lock);
            return g;
        }
    pthread_mutex_unlock(&g_lock);
    uint32_t g = gcalloc(1, 16);
    switch (CFXMLNodeGetTypeCode(node)) {
    case kCFXMLNodeTypeDocument: {
        const CFXMLDocumentInfo *d = info;
        W32_(g_mem, g, cf_out(d->sourceURL));
        W32_(g_mem, g + 4, d->encoding);
        break;
    }
    case kCFXMLNodeTypeElement: {
        const CFXMLElementInfo *e = info;
        W32_(g_mem, g, cf_out(e->attributes));
        W32_(g_mem, g + 4, cf_out(e->attributeOrder));
        W8_(g_mem, g + 8, e->isEmpty);
        break;
    }
    case kCFXMLNodeTypeProcessingInstruction:
        W32_(g_mem, g, cf_out(((const CFXMLProcessingInstructionInfo *)info)->dataString));
        break;
    case kCFXMLNodeTypeDocumentType: {
        const CFXMLDocumentTypeInfo *t = info;
        W32_(g_mem, g, cf_out(t->externalID.systemID));
        W32_(g_mem, g + 4, cf_out(t->externalID.publicID));
        break;
    }
    case kCFXMLNodeTypeEntityReference:
        W32_(g_mem, g, ((const CFXMLEntityReferenceInfo *)info)->entityType);
        break;
    default:
        break;
    }
    pthread_mutex_lock(&g_lock);
    if (g_ninfo == g_capinfo) {
        g_capinfo = g_capinfo ? g_capinfo * 2 : 1024;
        g_infos = realloc(g_infos, g_capinfo * sizeof *g_infos);
    }
    g_infos[g_ninfo].node = (CFXMLNodeRef)CFRetain(node);
    g_infos[g_ninfo++].info = g;
    pthread_mutex_unlock(&g_lock);
    return g;
}

SHIM(_CFXMLTreeCreateFromData) {
    /* (allocator, xmlData, dataSource, parseOptions, versionOfNodes) */
    CFXMLTreeRef t = CFXMLTreeCreateFromData(NULL, CFI(1), CFI(2), ARG(3), (CFIndex)(int32_t)ARG(4));
    if (getenv("CIV4_DEBUG_XML")) {
        CFDataRef d = CFI(1);
        CFIndex n = d ? CFDataGetLength(d) : -1;
        char head[64] = {0};
        if (n > 0) memcpy(head, CFDataGetBytePtr(d), n < 63 ? n : 63);
        for (char *c = head; *c; c++) if (*c < 32) *c = ' ';
        fprintf(stderr, "[xml] CreateFromData len %ld opts %x -> %p  \"%s\"\n", (long)n, ARG(3), (void *)t, head);
    }
    RETCF_OWNED(t);
}
SHIM(_CFXMLTreeCreateWithDataFromURL) {
    RETCF_OWNED(CFXMLTreeCreateWithDataFromURL(NULL, CFI(1), ARG(2), (CFIndex)(int32_t)ARG(3)));
}
SHIM(_CFXMLTreeGetNode) { RETCF(CFXMLTreeGetNode(CFI(0))); }
SHIM(_CFXMLTreeCreateWithNode) { RETCF_OWNED(CFXMLTreeCreateWithNode(NULL, CFI(1))); }
SHIM(_CFXMLTreeCreateXMLData) { RETCF_OWNED(CFXMLTreeCreateXMLData(NULL, CFI(1))); }
SHIM(_CFXMLNodeGetTypeCode) { RET(CFXMLNodeGetTypeCode(CFI(0))); }
SHIM(_CFXMLNodeGetString) { RETCF(CFXMLNodeGetString(CFI(0))); }
SHIM(_CFXMLNodeGetVersion) { RET(CFXMLNodeGetVersion(CFI(0))); }
SHIM(_CFXMLNodeGetInfoPtr) { RET(guest_info(CFI(0))); }
SHIM(_CFXMLNodeCreate) {
    /* (allocator, typeCode, string, const void *infoPtr, version) */
    CFXMLNodeTypeCode type = (CFXMLNodeTypeCode)ARG(1);
    uint32_t gi = ARG(3);
    union {
        CFXMLDocumentInfo doc;
        CFXMLElementInfo el;
        CFXMLProcessingInstructionInfo pi;
        CFXMLDocumentTypeInfo dt;
        CFXMLEntityReferenceInfo er;
    } info;
    memset(&info, 0, sizeof info);
    const void *ip = NULL;
    if (gi) {
        switch (type) {
        case kCFXMLNodeTypeDocument:
            info.doc.sourceURL = cf_in(R32_(g_mem, gi));
            info.doc.encoding = R32_(g_mem, gi + 4);
            ip = &info;
            break;
        case kCFXMLNodeTypeElement:
            info.el.attributes = cf_in(R32_(g_mem, gi));
            info.el.attributeOrder = cf_in(R32_(g_mem, gi + 4));
            info.el.isEmpty = R8_(g_mem, gi + 8);
            ip = &info;
            break;
        case kCFXMLNodeTypeProcessingInstruction:
            info.pi.dataString = cf_in(R32_(g_mem, gi));
            ip = &info;
            break;
        case kCFXMLNodeTypeDocumentType:
            info.dt.externalID.systemID = cf_in(R32_(g_mem, gi));
            info.dt.externalID.publicID = cf_in(R32_(g_mem, gi + 4));
            ip = &info;
            break;
        case kCFXMLNodeTypeEntityReference:
            info.er.entityType = R32_(g_mem, gi);
            ip = &info;
            break;
        default:
            break;
        }
    }
    RETCF_OWNED(CFXMLNodeCreate(NULL, type, CFI(2), ip, (CFIndex)(int32_t)ARG(4)));
}
SHIM(_CFXMLCreateStringByUnescapingEntities) {
    /* Old CF tolerated a NULL string here (empty element text). */
    CFStringRef str = CFI(1);
    if (!str) { RETCF_OWNED(CFStringCreateWithCString(NULL, "", kCFStringEncodingUTF8)); return; }
    RETCF_OWNED(CFXMLCreateStringByUnescapingEntities(NULL, str, CFI(2)));
}

/* ---- CFTree (CFXMLTree is a CFTree) ---- */
SHIM(_CFTreeGetChildCount) { RET(CFTreeGetChildCount(CFI(0))); }
SHIM(_CFTreeGetChildAtIndex) { RETCF(CFTreeGetChildAtIndex(CFI(0), (CFIndex)(int32_t)ARG(1))); }
SHIM(_CFTreeGetFirstChild) { RETCF(CFTreeGetFirstChild(CFI(0))); }
SHIM(_CFTreeGetNextSibling) { RETCF(CFTreeGetNextSibling(CFI(0))); }
SHIM(_CFTreeGetParent) { RETCF(CFTreeGetParent(CFI(0))); }
SHIM(_CFTreeFindRoot) { RETCF(CFTreeFindRoot(CFI(0))); }
SHIM(_CFTreeAppendChild) { CFTreeAppendChild(CFI(0), CFI(1)); }
SHIM(_CFTreeRemove) { CFTreeRemove(CFI(0)); }

/* ---- Carbon standard alerts: log the text, answer with the default button ---- */
SHIM(_GetStandardAlertDefaultParams) { memset(PTR(ARG(0)), 0, 32); RET(noErr); }
SHIM(_CreateStandardAlert) {
    /* (alertType, CFStringRef error, CFStringRef explanation, param*, DialogRef *out) */
    char a[1024] = "", b[1024] = "";
    if (ARG(1)) CFStringGetCString(cf_in(ARG(1)), a, sizeof a, kCFStringEncodingUTF8);
    if (ARG(2)) CFStringGetCString(cf_in(ARG(2)), b, sizeof b, kCFStringEncodingUTF8);
    rt_log("ALERT: %s -- %s", a, b);
    W32_(g_mem, ARG(4), 0x414c5254u);
    RET(noErr);
}
SHIM(_RunStandardAlert) { if (ARG(2)) W16_(g_mem, ARG(2), 1); RET(noErr); }
SHIM(_AutoSizeDialog) { RET(noErr); }

const ShimDef shims_cf_xml[] = {
    SHIM_ENTRY(_CFXMLTreeCreateFromData), SHIM_ENTRY(_CFXMLTreeCreateWithDataFromURL), SHIM_ENTRY(_CFXMLTreeGetNode),
    SHIM_ENTRY(_CFXMLTreeCreateWithNode), SHIM_ENTRY(_CFXMLTreeCreateXMLData), SHIM_ENTRY(_CFXMLNodeGetTypeCode),
    SHIM_ENTRY(_CFXMLNodeGetString), SHIM_ENTRY(_CFXMLNodeGetVersion), SHIM_ENTRY(_CFXMLNodeGetInfoPtr),
    SHIM_ENTRY(_CFXMLNodeCreate), SHIM_ENTRY(_CFXMLCreateStringByUnescapingEntities), SHIM_ENTRY(_CFTreeGetChildCount),
    SHIM_ENTRY(_CFTreeGetChildAtIndex), SHIM_ENTRY(_CFTreeGetFirstChild), SHIM_ENTRY(_CFTreeGetNextSibling),
    SHIM_ENTRY(_CFTreeGetParent), SHIM_ENTRY(_CFTreeFindRoot), SHIM_ENTRY(_CFTreeAppendChild),
    SHIM_ENTRY(_CFTreeRemove), SHIM_ENTRY(_GetStandardAlertDefaultParams), SHIM_ENTRY(_CreateStandardAlert),
    SHIM_ENTRY(_RunStandardAlert), SHIM_ENTRY(_AutoSizeDialog),
    {0, 0},
};

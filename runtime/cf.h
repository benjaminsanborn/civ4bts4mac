/* Bridge between guest CF object handles and host CoreFoundation objects. */
#ifndef CIV4_CF_H
#define CIV4_CF_H

#include "shim.h"

#include <CoreFoundation/CoreFoundation.h>

/* Guest handle -> host object. Accepts proxies and the game's constant CFStrings. */
CFTypeRef cf_in(uint32_t g);
/* Host object -> guest handle (stable per object; the proxy keeps it alive). */
uint32_t cf_out(CFTypeRef h);
/* Like cf_out for a +1 (Create/Copy rule) result: the guest owns that reference. */
uint32_t cf_out_owned(CFTypeRef h);

#define CFI(n) ((void *)cf_in(ARG(n)))
#define RETCF(h) RET(cf_out((CFTypeRef)(h)))
#define RETCF_OWNED(h) RET(cf_out_owned((CFTypeRef)(h)))

/* A guest address for calling a host shim by name (for function pointers
 * handed to the guest, e.g. CFBundleGetFunctionPointerForName). */
uint32_t rt_dynamic_import(const char *name);

void rt_cf_init(void);

#endif

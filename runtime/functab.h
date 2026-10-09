#ifndef CIV4_FUNCTAB_H
#define CIV4_FUNCTAB_H

#include "rt.h"

typedef struct {
    uint32_t addr;
    GuestFn fn;
} FuncEntry;

/* Generated: sorted by addr. */
extern const FuncEntry g_functab[];
extern const uint32_t g_functab_len;
extern const char *const g_import_names[];
extern const uint32_t g_import_stub[];
extern const uint32_t g_import_count;

#endif

#ifndef CIV4_FUNCTAB_H
#define CIV4_FUNCTAB_H

#include "rt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generated tables (functab.c). See tools/recomp/world.py for the layout. */
typedef struct {
    uint32_t addr;
    GuestFn fn;
} FuncEntry;

typedef struct {
    uint32_t addr;   /* stub address */
    uint32_t target; /* guest function address, or import id */
    uint32_t is_import;
} StubEntry;

typedef struct {
    const char *name;
    uint32_t addr;
} HostData;

extern const FuncEntry g_functab[]; /* sorted by addr */
extern const uint32_t g_functab_len;
extern const char *const g_import_names[];
extern const uint32_t g_import_count;
extern const StubEntry g_stubs[]; /* sorted by addr */
extern const uint32_t g_stubs_len;
extern const uint32_t g_init_funcs[];
extern const uint32_t g_init_count;
extern const HostData g_hostdata[];
extern const uint32_t g_hostdata_count;
extern const uint32_t g_main;
extern const uint32_t g_eh_frames[][2]; /* guest __eh_frame sections: {addr, size} */
extern const uint32_t g_eh_frame_count;

#ifdef __cplusplus
}
#endif

#endif

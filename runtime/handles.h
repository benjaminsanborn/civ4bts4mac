#ifndef CIV4_HANDLES_H
#define CIV4_HANDLES_H

#include "shim.h"

/* Tags for opaque guest handles. */
#define TAG_EVENT 0x45564e54u   /* 'EVNT' */
#define TAG_TARGET 0x54475420u  /* 'TGT ' */
#define TAG_HANDLER 0x484e444cu /* 'HNDL' */
#define TAG_CALLREF 0x43414c4cu /* 'CALL' */
#define TAG_TIMER 0x544d5220u   /* 'TMR ' */
#define TAG_WINDOW 0x57494e44u  /* 'WIND' */
#define TAG_PORT 0x504f5254u    /* 'PORT' */
#define TAG_AGLPIX 0x4150584cu  /* 'APXL' */
#define TAG_AGLCTX 0x41435458u  /* 'ACTX' */
#define TAG_PBUFFER 0x50425546u /* 'PBUF' */

uint32_t gh_new(uint32_t tag, void *host);
void *gh_get(uint32_t g, uint32_t tag); /* NULL if g is 0 or not a handle of this tag */
void gh_free(uint32_t g);

#endif

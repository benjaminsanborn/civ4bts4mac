/* Carbon Event Manager emulation: interface for the platform layer. */
#ifndef CIV4_EVENTS_H
#define CIV4_EVENTS_H

#include "shim.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* Event classes / kinds used by the platform layer. */
#define kEventClassMouse FOURCC('m', 'o', 'u', 's')
#define kEventClassKeyboard FOURCC('k', 'e', 'y', 'b')
#define kEventClassTextInput FOURCC('t', 'e', 'x', 't')
#define kEventClassApplication FOURCC('a', 'p', 'p', 'l')
#define kEventClassWindow FOURCC('w', 'i', 'n', 'd')
#define kEventClassCommand FOURCC('c', 'm', 'd', 's')

#define kEventMouseDown 1
#define kEventMouseUp 2
#define kEventMouseMoved 5
#define kEventMouseDragged 6
#define kEventMouseWheelMoved 10
#define kEventRawKeyDown 1
#define kEventRawKeyRepeat 2
#define kEventRawKeyUp 3
#define kEventRawKeyModifiersChanged 4
#define kEventTextInputUnicodeForKeyEvent 2
#define kEventAppActivated 1
#define kEventAppDeactivated 2
#define kEventWindowActivated 5
#define kEventWindowDeactivated 6
#define kEventWindowBoundsChanged 27
#define kEventWindowClose 72

/* Parameter names and types. */
#define kEventParamDirectObject FOURCC('-', '-', '-', '-')
#define kEventParamWindowRef FOURCC('w', 'i', 'n', 'd')
#define kEventParamMouseLocation FOURCC('m', 'l', 'o', 'c')
#define kEventParamWindowMouseLocation FOURCC('w', 'm', 'o', 'u')
#define kEventParamMouseButton FOURCC('m', 'b', 't', 'n')
#define kEventParamKeyModifiers FOURCC('k', 'm', 'o', 'd')
#define kEventParamClickCount FOURCC('c', 'c', 'n', 't')
#define kEventParamMouseWheelAxis FOURCC('m', 'w', 'a', 'x')
#define kEventParamMouseWheelDelta FOURCC('m', 'w', 'd', 'v')
#define kEventParamMouseDelta FOURCC('m', 'd', 't', 'a')
#define kEventParamKeyCode FOURCC('k', 'c', 'o', 'd')
#define kEventParamKeyMacCharCodes FOURCC('k', 'c', 'h', 'r')
#define kEventParamKeyUnicodes FOURCC('k', 'u', 'n', 'i')
#define kEventParamKeyboardType FOURCC('k', 'b', 'd', 't')
#define kEventParamTextInputSendText FOURCC('t', 's', 't', 'x')
#define kEventParamTextInputSendKeyboardEvent FOURCC('t', 's', 'k', 'e')
#define kEventParamCurrentBounds FOURCC('c', 'r', 'c', 't')

#define typeWildCard FOURCC('*', '*', '*', '*')
#define typeHIPoint FOURCC('h', 'i', 'p', 't')
#define typeQDPoint FOURCC('Q', 'D', 'p', 't')
#define typeMouseButton FOURCC('m', 'b', 't', 'n')
#define typeUInt32 FOURCC('m', 'a', 'g', 'n')
#define typeSInt32 FOURCC('l', 'o', 'n', 'g')
#define typeMouseWheelAxis FOURCC('m', 'w', 'a', 'x')
#define typeChar FOURCC('T', 'E', 'X', 'T')
#define typeUnicodeText FOURCC('u', 't', 'x', 't')
#define typeWindowRef FOURCC('w', 'i', 'n', 'd')
#define typeEventRef FOURCC('e', 'v', 'r', 'f')
#define typeQDRectangle FOURCC('q', 'd', 'r', 't')

#define eventNotHandledErr (-9874)
#define eventLoopTimedOutErr (-9875)
#define eventParameterNotFoundErr (-9870)

typedef struct HEvent HEvent;

/* Create a host-side event (refcount 1) and attach parameters. */
HEvent *ev_create(uint32_t cls, uint32_t kind);
void ev_set_param(HEvent *e, uint32_t name, uint32_t type, const void *data, uint32_t size);
void ev_release(HEvent *e);
/* Post to the main queue (takes a reference). Safe from the main thread only. */
void ev_post(HEvent *e);

/* Event targets the platform layer creates (one per window). */
uint32_t ev_new_target(uint32_t parent_target);
uint32_t ev_app_target(void);

/* Implemented by the platform layer: move pending OS input into the queue,
 * waiting up to `timeout` seconds for something to arrive. */
void plat_pump(double timeout);

double ev_now(void); /* EventTime: seconds since boot */

#endif

/* Overrides for Aspyr Support Library (ASL) startup UI the harness skips:
 * license/registration/preferences dialogs, the nib menu bar, GameRanger,
 * and modal error alerts (logged instead). */
#include "cf.h"

#include <stdlib.h>

static void log_cfstring(const char *what, uint32_t g) {
    char buf[1024] = "?";
    if (g) CFStringGetCString(cf_in(g), buf, sizeof buf, kCFStringEncodingUTF8);
    rt_log("%s: %s", what, buf);
}

/* ASL::CheckAspyrEULA(): accepted. */
OVERRIDE(__ZN3ASL14CheckAspyrEULAEv) {}
/* ASL::CheckRegistrationRequest(CFStringRef): never ask. */
OVERRIDE(__ZN3ASL24CheckRegistrationRequestEPK10__CFString) {}
/* Civ4::CheckPreferences(): skip the launcher/preferences dialog. */
OVERRIDE(__ZN4Civ416CheckPreferencesEv) {}
/* ASL::CreateMenuBar(): no Carbon menu bar. */
OVERRIDE(__ZN3ASL13CreateMenuBarEv) {}
/* InitGameRanger(): no GameRanger. */
OVERRIDE(__Z14InitGameRangerv) {}

/* ASL::ErrorAlert(CFStringRef key, ...): log and continue. */
OVERRIDE(__ZN3ASL10ErrorAlertEPK10__CFStringz) { log_cfstring("ASL::ErrorAlert", ARG(0)); }
/* ASL::ErrorAlertYesNo(msg, yes, no, defaultYes, ...): take the default answer. */
OVERRIDE(__ZN3ASL15ErrorAlertYesNoEPK10__CFStringS2_S2_bz) {
    log_cfstring("ASL::ErrorAlertYesNo", ARG(0));
    RET((uint8_t)ARG(3));
}
/* ASL::ErrorDie(CFStringRef key, ...): fatal. */
OVERRIDE(__ZN3ASL8ErrorDieEPK10__CFStringz) {
    log_cfstring("ASL::ErrorDie", ARG(0));
    rt_fatal("game called ASL::ErrorDie");
}

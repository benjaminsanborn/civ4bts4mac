/* Overrides for Aspyr Support Library (ASL) startup UI the harness skips:
 * license/registration/preferences dialogs, the nib menu bar, GameRanger,
 * and modal error alerts (logged instead). */
#include "cf.h"

#include <stdio.h>
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


/* ---- relaunching (loading a mod or BTS scenario restarts the game) ----
 * RelaunchSelf(cmdline) normally starts a new instance through Launch
 * Services and an Apple Event carrying the command line, then exits; the new
 * instance waits for that event in ASL::WaitForCommandLine. Start a new
 * harness process instead, passing the command line in the environment. */
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <unistd.h>
extern char **environ;
OVERRIDE(_RelaunchSelf) {
    const char *cmd = ARG(0) ? CPTR(ARG(0)) : "";
    rt_log("RelaunchSelf: restarting with command line \"%s\"", cmd);
    char exe[PATH_MAX];
    uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) != 0) rt_fatal("RelaunchSelf: no executable path");
    setenv("CIV4_COMMAND_LINE", cmd, 1);
    unsetenv("CIV4_INPUT"); /* a test script belongs to the first instance */
    char *argv[] = {exe, NULL};
    pid_t pid;
    if (posix_spawn(&pid, exe, NULL, NULL, argv, environ) != 0) {
        rt_log("RelaunchSelf: could not start %s", exe);
    } else if (getenv("CIV4_PIDFILE")) { /* test runs track the new instance */
        FILE *f = fopen(getenv("CIV4_PIDFILE"), "w");
        if (f) { fprintf(f, "%d\n", pid); fclose(f); }
    }
    exit(0);
}
/* ASL::WaitForCommandLine(ticks, char *buf, size): the relaunch command line, if any. */
OVERRIDE(__ZN3ASL18WaitForCommandLineEmPcm) {
    const char *cmd = getenv("CIV4_COMMAND_LINE");
    uint32_t buf = ARG(1), size = ARG(2);
    if (!size) { RET(0); return; }
    snprintf(PTR(buf), size, "%s%s", cmd && *cmd ? " " : "", cmd ? cmd : "");
    unsetenv("CIV4_COMMAND_LINE");
    RET(0);
}

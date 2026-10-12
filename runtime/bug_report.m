/* Bug reports: a GitHub issue filled in with the build, the Mac and the end of
 * the log, plus a screenshot and the log folder shown in Finder to attach.
 *   - in the game: Command-Option-B (platform_mac.m),
 *   - from the launcher: civ4 --report-bug [--log FILE] [--crash] (the setup
 *     window's button, and the offer after the game quit unexpectedly).
 * CIV4_REPORT_PRINT prints the issue instead of opening anything. */
#import <Cocoa/Cocoa.h>
#include <sys/sysctl.h>

#include "platform.h"

#define ISSUES_URL "https://github.com/benjaminsanborn/civ4bts4mac/issues/new"

void agl_request_screenshot(const char *path);
float agl_display_scale(void);

static NSString *sysctl_string(const char *name) {
    char buf[256];
    size_t n = sizeof buf;
    if (sysctlbyname(name, buf, &n, NULL, 0)) return @"?";
    return [NSString stringWithUTF8String:buf] ?: @"?";
}

/* $HOME, as the game and the launcher use it. */
static NSString *home(void) {
    return getenv("HOME") ? [NSString stringWithUTF8String:getenv("HOME")] : NSHomeDirectory();
}
static NSString *log_dir(void) {
    return [home() stringByAppendingPathComponent:@"Library/Logs/Civ4 BtS Native"];
}

/* "customassets", "mod" or "none", from the BUG Mod installer's manifest. */
static NSString *bug_mod(void) {
    NSString *path = [home() stringByAppendingPathComponent:
        @"Library/Application Support/Civ4 BtS Native/Civilization IV Beyond the Sword/civ4bts4mac-bugmod.json"];
    NSData *d = [NSData dataWithContentsOfFile:path];
    NSDictionary *m = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:NULL] : nil;
    return [m isKindOfClass:[NSDictionary class]] && m[@"mode"] ? m[@"mode"] : @"none";
}

/* The last lines of a log, at most `max` characters. */
static NSString *log_tail(NSString *path, NSUInteger max) {
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL]
                         ?: [NSString stringWithContentsOfFile:path encoding:NSISOLatin1StringEncoding error:NULL];
    if (!text.length) return nil;
    NSMutableArray *keep = [NSMutableArray array];
    NSUInteger len = 0;
    for (NSString *line in [[text componentsSeparatedByString:@"\n"] reverseObjectEnumerator]) {
        if (!line.length || [line hasPrefix:@"[rt] movie "] || [line containsString:@"CFBundleGetFunctionPointerForName"] ||
            [line containsString:@"NSIsSymbolNameDefined"] || [line containsString:@"IORegistryEntryCreateCFProperty"])
            continue; /* noise */
        NSString *l = line.length > 200 ? [[line substringToIndex:200] stringByAppendingString:@"…"] : line;
        if (len + l.length + 1 > max) break;
        [keep insertObject:l atIndex:0];
        len += l.length + 1;
    }
    return [keep componentsJoinedByString:@"\n"];
}

static NSString *issue_body(NSString *log, int crash, NSString *screenshot) {
    NSString *version = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    NSMutableString *b = [NSMutableString string];
    [b appendString:crash ? @"**The game quit unexpectedly.**\n\n" : @""];
    [b appendString:@"### What happened\n\n<!-- What did you do, what did you expect, and what happened instead? -->\n\n\n"
                    @"### Steps to reproduce\n\n1. \n\n"];
    [b appendFormat:@"### Attachments\n\n<!-- Drag these in from the Finder window that opened: %@%@, and a save "
                    @"game if it helps (in ~/Library/Application Support/Civ4 BtS Native/Civilization IV Beyond the "
                    @"Sword/Saves). -->\n\n",
                    log ? log.lastPathComponent : @"the log", screenshot ? @" and the screenshot" : @""];
    NSScreen *screen = [NSScreen mainScreen];
    NSSize pts = screen.frame.size;
    int mw = 0, mh = 0;
    if (!crash) plat_display_mode(&mw, &mh);
    [b appendString:@"### System\n\n"];
    [b appendFormat:@"- Civ4 BtS Native: %@\n", version ?: @"development build"];
    [b appendFormat:@"- macOS: %@\n", [[NSProcessInfo processInfo] operatingSystemVersionString]];
    [b appendFormat:@"- Mac: %@, %@, %llu GB\n", sysctl_string("hw.model"), sysctl_string("machdep.cpu.brand_string"),
                    [[NSProcessInfo processInfo] physicalMemory] >> 30];
    [b appendFormat:@"- Display: %.0fx%.0f points at %.0fx", pts.width, pts.height, screen.backingScaleFactor];
    if (mw) [b appendFormat:@", game resolution %dx%d", mw, mh];
    [b appendFormat:@"\n- BUG Mod: %@\n", bug_mod()];
    NSString *tail = log ? log_tail(log, 2500) : nil;
    if (tail.length) [b appendFormat:@"\n<details><summary>End of %@</summary>\n\n```\n%@\n```\n</details>\n",
                                     log.lastPathComponent, tail];
    return b;
}

static void open_issue(NSString *log, int crash, NSString *screenshot) {
    NSURLComponents *u = [NSURLComponents componentsWithString:@ISSUES_URL];
    u.queryItems = @[
        [NSURLQueryItem queryItemWithName:@"title" value:crash ? @"Crash: " : @""],
        [NSURLQueryItem queryItemWithName:@"body" value:issue_body(log, crash, screenshot)],
        [NSURLQueryItem queryItemWithName:@"labels" value:@"bug"],
    ];
    if (getenv("CIV4_REPORT_PRINT")) { /* tests: print it instead */
        printf("%s\n\n%s\n", u.URL.absoluteString.UTF8String, issue_body(log, crash, screenshot).UTF8String);
        fflush(stdout);
        return;
    }
    if (u.URL) [[NSWorkspace sharedWorkspace] openURL:u.URL];
}

static void show_files(NSArray<NSString *> *paths) {
    if (getenv("CIV4_REPORT_PRINT")) return;
    NSMutableArray *urls = [NSMutableArray array];
    for (NSString *p in paths)
        if ([[NSFileManager defaultManager] fileExistsAtPath:p]) [urls addObject:[NSURL fileURLWithPath:p]];
    if (urls.count) [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:urls];
}

/* In the game: screenshot the next frame, then open the issue and the log folder. */
void plat_report_bug(void) {
    static double last;
    if (CFAbsoluteTimeGetCurrent() - last < 5) return; /* one per keypress, not per repeat */
    last = CFAbsoluteTimeGetCurrent();
    NSString *dir = log_dir();
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL];
    NSDateFormatter *f = [[NSDateFormatter alloc] init];
    f.dateFormat = @"yyyy-MM-dd 'at' HH.mm.ss";
    NSString *shot = [dir stringByAppendingPathComponent:
        [NSString stringWithFormat:@"Screenshot %@.png", [f stringFromDate:[NSDate date]]]];
    agl_request_screenshot(shot.fileSystemRepresentation);
    NSString *log = [dir stringByAppendingPathComponent:@"game.log"];
    if (![[NSFileManager defaultManager] fileExistsAtPath:log]) log = nil; /* started from a terminal */
    rt_log("bug report: opening a GitHub issue; screenshot %s", shot.UTF8String);
    /* After the frame with the screenshot has been drawn. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 700 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        show_files(log ? @[log, shot] : @[shot]);
        open_issue(log, 0, shot);
    });
}

/* civ4 --report-bug [--log FILE] [--crash]: from the launcher, without the game. */
int plat_report_bug_main(int argc, char **argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSString *log = nil;
        int crash = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--log") && i + 1 < argc) log = [NSString stringWithUTF8String:argv[++i]];
            else if (!strcmp(argv[i], "--crash")) crash = 1;
        }
        if (!log) log = [log_dir() stringByAppendingPathComponent:@"game.log"];
        if (![[NSFileManager defaultManager] fileExistsAtPath:log]) log = nil;
        if (log) show_files(@[log]);
        open_issue(log, crash, nil);
    }
    return 0;
}

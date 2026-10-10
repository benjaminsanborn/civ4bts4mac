/* QuickTime Movie API on AVFoundation.
 *
 * The game's movies are Sorenson Video 3, which modern macOS can't decode, so
 * tools/transcode_movies.sh converts them to H.264 MP4 under build/movies/
 * (mirroring their path inside the game folder). A guest Movie is an AVPlayer;
 * MoviesTask draws the current frame into the movie's GWorld. */
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>

#include "cf.h"
#include <pthread.h>
#include "handles.h"

#define TAG_MOVIE 0x4d4f5649u /* 'MOVI' */
#define TIMESCALE 600
#define noErr 0
#define fnfErr (-43)
#define couldNotResolveDataRef (-2000)

CGContextRef qd_gworld_context(uint32_t port);
const char *rt_fsref_path(uint32_t g);

typedef struct {
    uint32_t g;
    AVPlayer *player;
    AVPlayerItem *item;
    AVPlayerItemVideoOutput *output;
    int width, height;
    int16_t box[4]; /* top, left, bottom, right */
    uint32_t gworld;
    uint32_t done_proc, done_refcon;
    double duration;
    float rate;       /* preferred rate */
    int16_t volume;   /* 8.8 fixed */
    int playing;
    int ended; /* end-of-movie callbacks have fired for this play-through */
} HMovie;

/* QuickTime callbacks (NewCallBack/CallMeWhen): only callBackAtExtremes is
 * used, to learn when music finishes. */
#define TAG_CALLBACK 0x51544342u /* 'QTCB' */
typedef struct {
    uint32_t g, movie; /* the movie doubles as its TimeBase */
    uint32_t type, proc, refcon, flags;
    int armed;
} HCallBack;
static NSMutableArray *g_callbacks; /* NSValue(HCallBack*) */

/* The game drives music from its own QuickTime thread (MoviesTask) while the
 * main thread creates and disposes movies: every movie call holds this lock. */
static pthread_mutex_t g_movie_lock;
static void movie_lock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_movie_lock, &a);
}
static int movie_lock(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, movie_lock_init);
    pthread_mutex_lock(&g_movie_lock);
    return 0;
}
static void movie_unlock(int *unused) { (void)unused; pthread_mutex_unlock(&g_movie_lock); }
#define MOVIE_LOCK int movie_lock_ __attribute__((cleanup(movie_unlock), unused)) = movie_lock()

static NSMutableArray *g_movies;

/* Guest callbacks found under the lock run after it is released. */
typedef struct { uint32_t proc, a, b; } PendingCall;
static PendingCall g_pending[64];
static int g_npending;
static void defer_call(uint32_t proc, uint32_t a, uint32_t b) {
    if (g_npending < 64) g_pending[g_npending++] = (PendingCall){proc, a, b};
} /* NSValue(HMovie*) of live movies, for MoviesTask(NULL) */

static HMovie *movie_of(uint32_t g) { return gh_get(g, TAG_MOVIE); }

/* ---- data references: a guest Handle holding the movie's host path ---- */
static uint32_t make_dataref(const char *path) {
    size_t n = strlen(path) + 1;
    uint32_t data = gmalloc((uint32_t)n), h = gmalloc(4);
    memcpy(PTR(data), path, n);
    W32_(g_mem, h, data);
    return h;
}

SHIM(_QTNewDataReferenceFromFSRef) { MOVIE_LOCK;
    /* (const FSRef *, flags, Handle *outDataRef, OSType *outDataRefType) */
    W32_(g_mem, ARG(2), make_dataref(rt_fsref_path(ARG(0))));
    if (ARG(3)) W32_(g_mem, ARG(3), 'alis');
    RET(noErr);
}
SHIM(_QTNewDataReferenceFromFullPathCFString) { MOVIE_LOCK;
    /* (CFStringRef path, pathStyle, flags, Handle *outDataRef, OSType *outDataRefType) */
    char path[PATH_MAX];
    CFStringGetCString(CFI(0), path, sizeof path, kCFStringEncodingUTF8);
    W32_(g_mem, ARG(3), make_dataref(path));
    if (ARG(4)) W32_(g_mem, ARG(4), 'alis');
    RET(noErr);
}

/* Map a movie inside the game folder to its transcoded MP4. Other media
 * (the MP3 soundtrack) plays from the original file. */
static NSURL *transcoded_url(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext || strcasecmp(ext, ".mov"))
        return access(path, R_OK) ? nil : [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    char link[PATH_MAX], game[PATH_MAX], real[PATH_MAX], out[PATH_MAX];
    if (!realpath(rt_build_path("game", link, sizeof link), game)) return nil;
    const char *p = realpath(path, real) ? real : path;
    size_t n = strlen(game);
    if (strncasecmp(p, game, n) || p[n] != '/') return nil;
    snprintf(out, sizeof out, "%s/%s.mp4", rt_build_path("movies", link, sizeof link), p + n + 1);
    if (access(out, R_OK)) {
        rt_log("movie not transcoded: %s (run tools/transcode_movies.sh)", out);
        return nil;
    }
    return [NSURL fileURLWithPath:[NSString stringWithUTF8String:out]];
}

SHIM(_NewMovieFromDataRef) { MOVIE_LOCK;
    /* (Movie *out, short flags, short *id, Handle dataRef, OSType dataRefType) */
    W32_(g_mem, ARG(0), 0);
    uint32_t h = ARG(3);
    const char *path = h ? CPTR(R32_(g_mem, h)) : NULL;
    NSURL *url = path ? transcoded_url(path) : nil;
    if (!url) {
        rt_log("movie: can't resolve %s", path ? path : "(null)");
        RET(couldNotResolveDataRef);
        return;
    }

    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
    AVAssetTrack *track = [[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
    HMovie *m = calloc(1, sizeof *m);
    CGSize size = track ? CGSizeApplyAffineTransform(track.naturalSize, track.preferredTransform) : CGSizeMake(0, 0);
    m->width = (int)fabs(size.width);
    m->height = (int)fabs(size.height);
    int16_t box[4] = {0, 0, (int16_t)m->height, (int16_t)m->width};
    memcpy(m->box, box, sizeof box);
    m->duration = CMTimeGetSeconds(asset.duration);
    m->rate = 1;
    m->volume = 0x100;
    m->item = [AVPlayerItem playerItemWithAsset:asset];
    NSDictionary *attrs = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32ARGB)};
    m->output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:attrs];
    [m->item addOutput:m->output];
    m->player = [AVPlayer playerWithPlayerItem:m->item];
    if (getenv("CIV4_MUTE")) m->player.muted = YES;
    m->player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
    m->g = gh_new(TAG_MOVIE, m);
    if (!g_movies) g_movies = [NSMutableArray new];
    [g_movies addObject:[NSValue valueWithPointer:m]];
    rt_log("movie %s: %dx%d, %.1fs", [[url lastPathComponent] UTF8String], m->width, m->height, m->duration);
    W32_(g_mem, ARG(0), m->g);
    RET(noErr);
}

SHIM(_DisposeMovie) { MOVIE_LOCK;
    HMovie *m = movie_of(ARG(0));
    if (!m) return;
    for (NSValue *v in g_callbacks) {
        HCallBack *cb = [v pointerValue];
        if (cb->movie == m->g) cb->armed = 0, cb->movie = 0;
    }
    [m->player pause];
    for (NSUInteger i = 0; i < [g_movies count]; i++)
        if ([g_movies[i] pointerValue] == m) { [g_movies removeObjectAtIndex:i]; break; }
    m->player = nil;
    m->item = nil;
    m->output = nil;
    gh_free(m->g);
    free(m);
}

/* ---- geometry and destination ---- */
SHIM(_SetMovieGWorld) { MOVIE_LOCK; HMovie *m = movie_of(ARG(0)); if (m) m->gworld = ARG(1); }
SHIM(_GetMovieGWorld) { MOVIE_LOCK;
    HMovie *m = movie_of(ARG(0));
    if (ARG(1)) W32_(g_mem, ARG(1), m ? m->gworld : 0);
    if (ARG(2)) W32_(g_mem, ARG(2), 0x6764);
}
SHIM(_GetMovieBox) { MOVIE_LOCK; HMovie *m = movie_of(ARG(0)); if (m) memcpy(PTR(ARG(1)), m->box, 8); }
SHIM(_SetMovieBox) { MOVIE_LOCK; HMovie *m = movie_of(ARG(0)); if (m) memcpy(m->box, PTR(ARG(1)), 8); }
SHIM(_SetMovieDrawingCompleteProc) { MOVIE_LOCK;
    /* (movie, flags, MovieDrawingCompleteUPP proc, long refCon) */
    HMovie *m = movie_of(ARG(0));
    if (m) { m->done_proc = ARG(2); m->done_refcon = ARG(3); }
}

/* ---- playback ---- */
static double movie_seconds(HMovie *m) { return CMTimeGetSeconds([m->player currentTime]); }
static void seek(HMovie *m, double t) {
    m->ended = 0;
    [m->player seekToTime:CMTimeMakeWithSeconds(t, TIMESCALE) toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
}
SHIM(_StartMovie) { MOVIE_LOCK;
    HMovie *m = movie_of(ARG(0));
    if (!m) return;
    m->playing = 1;
    m->ended = 0;
    m->player.rate = m->rate;
}
SHIM(_StopMovie) { MOVIE_LOCK;
    HMovie *m = movie_of(ARG(0));
    if (!m) return;
    m->playing = 0;
    [m->player pause];
}
SHIM(_GoToBeginningOfMovie) { MOVIE_LOCK; HMovie *m = movie_of(ARG(0)); if (m) seek(m, 0); }
SHIM(_GoToEndOfMovie) { MOVIE_LOCK; HMovie *m = movie_of(ARG(0)); if (m) seek(m, m->duration); }
SHIM(_SetMovieTimeValue) { MOVIE_LOCK; HMovie *m = movie_of(ARG(0)); if (m) seek(m, (double)(int32_t)ARG(1) / TIMESCALE); }
SHIM(_IsMovieDone) { MOVIE_LOCK; HMovie *m = movie_of(ARG(0)); RET(!m || movie_seconds(m) >= m->duration - 0.05); }
SHIM(_GetMovieTime) { MOVIE_LOCK;
    /* (movie, TimeRecord *out) -> TimeValue; TimeRecord {SInt64 value; TimeScale scale; TimeBase base} */
    HMovie *m = movie_of(ARG(0));
    int64_t v = m ? (int64_t)(movie_seconds(m) * TIMESCALE) : 0;
    if (ARG(1)) {
        W64_(g_mem, ARG(1), (uint64_t)v);
        W32_(g_mem, ARG(1) + 8, TIMESCALE);
        W32_(g_mem, ARG(1) + 12, ARG(0));
    }
    RET((uint32_t)v);
}
SHIM(_GetMovieTimeScale) { MOVIE_LOCK; RET(TIMESCALE); }
SHIM(_GetMovieDuration) { MOVIE_LOCK; HMovie *m = movie_of(ARG(0)); RET(m ? (uint32_t)(m->duration * TIMESCALE) : 0); }
SHIM(_GetMovieTimeBase) { MOVIE_LOCK; RET(ARG(0)); }
SHIM(_GetMoviePreferredRate) { MOVIE_LOCK; HMovie *m = movie_of(ARG(0)); RET(m ? (uint32_t)(m->rate * 65536) : 0x10000); }
SHIM(_SetMoviePreferredRate) { MOVIE_LOCK; HMovie *m = movie_of(ARG(0)); if (m) m->rate = (int32_t)ARG(1) / 65536.0f; }
SHIM(_GetMovieVolume) { MOVIE_LOCK; HMovie *m = movie_of(ARG(0)); RET(m ? (uint32_t)(uint16_t)m->volume : 0x100); }
SHIM(_SetMovieVolume) { MOVIE_LOCK;
    HMovie *m = movie_of(ARG(0));
    if (!m) return;
    m->volume = (int16_t)ARG(1);
    m->player.volume = m->volume <= 0 ? 0 : m->volume / 256.0f;
}
SHIM(_GetMovieAudioBalance) { MOVIE_LOCK; if (ARG(1)) WF32_(g_mem, ARG(1), 0); RET(noErr); }
SHIM(_SetMovieAudioBalance) { MOVIE_LOCK; RET(noErr); }
SHIM(_SetMovieActive) { MOVIE_LOCK;}
SHIM(_SetMovieActiveSegment) { MOVIE_LOCK;}
SHIM(_PrerollMovie) { MOVIE_LOCK; RET(noErr); }
SHIM(_LoadMovieIntoRam) { MOVIE_LOCK; RET(noErr); }

/* Draw the frame for "now" into the movie's GWorld, scaled into its box. */
static void draw_frame(CPU *cpu, HMovie *m) {
    CGContextRef ctx = qd_gworld_context(m->gworld);
    if (!ctx) return;
    CMTime t = [m->output itemTimeForHostTime:CACurrentMediaTime()];
    if (![m->output hasNewPixelBufferForItemTime:t]) return;
    CVPixelBufferRef pb = [m->output copyPixelBufferForItemTime:t itemTimeForDisplay:NULL];
    if (!pb) return;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    static CGColorSpaceRef cs;
    if (!cs) cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef src = CGBitmapContextCreate(CVPixelBufferGetBaseAddress(pb), CVPixelBufferGetWidth(pb),
                                             CVPixelBufferGetHeight(pb), 8, CVPixelBufferGetBytesPerRow(pb), cs,
                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Big);
    CGImageRef img = CGBitmapContextCreateImage(src);
    CGContextRelease(src);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease(pb);
    /* The GWorld context is y-down; flip locally so the image is upright. */
    CGRect r = CGRectMake(m->box[1], m->box[0], m->box[3] - m->box[1], m->box[2] - m->box[0]);
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, CGRectGetMaxY(r) + CGRectGetMinY(r));
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, r, img);
    CGContextRestoreGState(ctx);
    CGImageRelease(img);
    if (m->done_proc) defer_call(m->done_proc, m->g, m->done_refcon);
}

SHIM(_NewCallBack) { MOVIE_LOCK;
    /* (TimeBase, short callBackType) */
    HCallBack *cb = calloc(1, sizeof *cb);
    cb->movie = ARG(0);
    cb->type = (uint16_t)ARG(1);
    cb->g = gh_new(TAG_CALLBACK, cb);
    if (!g_callbacks) g_callbacks = [NSMutableArray new];
    [g_callbacks addObject:[NSValue valueWithPointer:cb]];
    RET(cb->g);
}
SHIM(_CallMeWhen) { MOVIE_LOCK;
    /* (QTCallBack, QTCallBackUPP proc, long refCon, long param1, long param2, long param3) */
    HCallBack *cb = gh_get(ARG(0), TAG_CALLBACK);
    if (!cb) { RET(-50); return; }
    cb->proc = ARG(1);
    cb->refcon = ARG(2);
    cb->flags = ARG(3);
    cb->armed = 1;
    RET(noErr);
}
SHIM(_CancelCallBack) { MOVIE_LOCK;
    HCallBack *cb = gh_get(ARG(0), TAG_CALLBACK);
    if (cb) cb->armed = 0;
}
SHIM(_DisposeCallBack) { MOVIE_LOCK;
    HCallBack *cb = gh_get(ARG(0), TAG_CALLBACK);
    if (!cb) return;
    [g_callbacks removeObject:[NSValue valueWithPointer:cb]];
    gh_free(cb->g);
    free(cb);
}

/* Fire one-shot end-of-movie callbacks (callBackAtExtremes, triggerAtStop). */
static void check_end(CPU *cpu, HMovie *m) {
    if (!m->playing || m->ended || movie_seconds(m) < m->duration - 0.05) return;
    m->ended = 1;
    for (NSValue *v in [g_callbacks copy]) {
        HCallBack *cb = [v pointerValue];
        if (cb->movie != m->g || !cb->armed || cb->type != 4 /* callBackAtExtremes */) continue;
        cb->armed = 0;
        defer_call(cb->proc, cb->g, cb->refcon);
    }
}

SHIM(_MoviesTask) {
    /* (movie or NULL for all, long maxMilliSecToUse) */
    PendingCall calls[64];
    int n;
    {
        MOVIE_LOCK;
        g_npending = 0;
        if (ARG(0)) {
            HMovie *m = movie_of(ARG(0));
            if (m) draw_frame(cpu, m), check_end(cpu, m);
        } else {
            for (NSValue *v in [g_movies copy]) {
                HMovie *m = [v pointerValue];
                draw_frame(cpu, m);
                check_end(cpu, m);
            }
        }
        n = g_npending;
        memcpy(calls, g_pending, sizeof calls[0] * (size_t)n);
        g_npending = 0;
    }
    for (int i = 0; i < n; i++) rt_call(cpu, calls[i].proc, 2, calls[i].a, calls[i].b);
}

const ShimDef shims_movies[] = {
    SHIM_ENTRY(_QTNewDataReferenceFromFSRef), SHIM_ENTRY(_QTNewDataReferenceFromFullPathCFString),
    SHIM_ENTRY(_NewMovieFromDataRef), SHIM_ENTRY(_DisposeMovie), SHIM_ENTRY(_SetMovieGWorld),
    SHIM_ENTRY(_GetMovieGWorld), SHIM_ENTRY(_GetMovieBox), SHIM_ENTRY(_SetMovieBox),
    SHIM_ENTRY(_SetMovieDrawingCompleteProc), SHIM_ENTRY(_StartMovie), SHIM_ENTRY(_StopMovie),
    SHIM_ENTRY(_GoToBeginningOfMovie), SHIM_ENTRY(_GoToEndOfMovie), SHIM_ENTRY(_SetMovieTimeValue),
    SHIM_ENTRY(_IsMovieDone), SHIM_ENTRY(_GetMovieTime), SHIM_ENTRY(_GetMovieTimeScale),
    SHIM_ENTRY(_GetMovieDuration), SHIM_ENTRY(_GetMovieTimeBase), SHIM_ENTRY(_GetMoviePreferredRate),
    SHIM_ENTRY(_SetMoviePreferredRate), SHIM_ENTRY(_GetMovieVolume), SHIM_ENTRY(_SetMovieVolume),
    SHIM_ENTRY(_GetMovieAudioBalance), SHIM_ENTRY(_SetMovieAudioBalance), SHIM_ENTRY(_SetMovieActive),
    SHIM_ENTRY(_SetMovieActiveSegment), SHIM_ENTRY(_PrerollMovie), SHIM_ENTRY(_LoadMovieIntoRam),
    SHIM_ENTRY(_MoviesTask), SHIM_ENTRY(_NewCallBack), SHIM_ENTRY(_CallMeWhen), SHIM_ENTRY(_CancelCallBack),
    SHIM_ENTRY(_DisposeCallBack),
    {0, 0},
};

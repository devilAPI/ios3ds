/*
 * ios3ds-tone: play a known test tone through the normal iOS audio path
 * (AudioQueue -> mediaserverd -> IOAudio2 -> AppleEmbeddedAudio -> the
 * shim's PL080/I2S emulation -> CSND), so the output can be checked: in QEMU
 * the board's CSND model records it (emu/qemu/hw/arm/ctr_csnd.c,
 * IOS3DS_CSND_WAV), on the 3DS you hear it and the shim logs the stream.
 *
 *   ios3ds-tone [count [interval_s [ms [hz [amplitude [delay_s]]]]]]
 *   defaults:    1      10          400  1000  6000      30
 *   ios3ds-tone rec [count [interval_s [ms [delay_s [dir [category]]]]]]  (record test, below)
 *   defaults:          1      10          2000  30          (dir: also save dir/recN.wav; "-" = no)
 *   category: plar (PlayAndRecord, default) or reca (RecordAudio, as Voice Memos)
 *
 * Record mode asks AudioSession for input (category PlayAndRecord), logs
 * whether input is available and the route, records `ms` of 16-bit mono
 * through AudioQueue and logs what arrived: frames, peak, RMS, the first
 * samples, a zero-crossing frequency estimate and a ramp check (QEMU's mic
 * model can play a 16-bit counter, IOS3DS_MIC=ramp: every step between two
 * recorded samples must then be 0 or +1, any other step is a corrupted or
 * dropped sample). Recording is only ever started by hand or by a test image,
 * never by the shipped launchd job.
 *
 * Left channel = the tone, right channel = the tone at half amplitude, so a
 * swapped or mono path shows in the recording. AudioToolbox is resolved with
 * dlopen (the stub SDK has no AudioToolbox headers); the declarations below
 * follow the iOS 4 SDK. Nothing here can hang the system: every failure is
 * logged once and the program exits.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>
void sync(void);

typedef int32_t OSStatus;
typedef void *AudioQueueRef;
typedef struct {
    double mSampleRate;
    uint32_t mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket;
    uint32_t mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved;
} AudioStreamBasicDescription;
typedef struct {
    const uint32_t mAudioDataBytesCapacity;
    void *const mAudioData;
    uint32_t mAudioDataByteSize;
    void *mUserData;
    const uint32_t mPacketDescriptionCapacity;
    void *const mPacketDescriptions;
    uint32_t mPacketDescriptionCount;
} AudioQueueBuffer, *AudioQueueBufferRef;
typedef void (*AQOutputCallback)(void *user, AudioQueueRef aq, AudioQueueBufferRef buf);
typedef void (*AQInputCallback)(void *user, AudioQueueRef aq, AudioQueueBufferRef buf,
                                const void *start, uint32_t npackets, const void *descs);

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define kAudioFormatLinearPCM               FOURCC('l', 'p', 'c', 'm')
#define kLinearPCMFlagsSignedPacked         ((1u << 2) | (1u << 3))
#define kAudioSessionProperty_AudioCategory FOURCC('a', 'c', 'a', 't')
#define kAudioSessionCategory_MediaPlayback FOURCC('m', 'e', 'd', 'i')
#define kAudioSessionCategory_PlayAndRecord FOURCC('p', 'l', 'a', 'r')
#define kAudioSessionCategory_RecordAudio   FOURCC('r', 'e', 'c', 'a')
#define kAudioSessionProperty_AudioInputAvailable FOURCC('a', 'i', 'a', 'v')
#define kAudioSessionProperty_AudioRoute    FOURCC('r', 'o', 'u', 't')
#define kAudioSessionProperty_AudioRouteChange FOURCC('r', 'o', 'c', 'h')
#define kCFStringEncodingUTF8               0x08000100u
#define kCFNumberSInt32Type                 3

static OSStatus (*AQNewOutput)(const AudioStreamBasicDescription *, AQOutputCallback, void *,
                               void *, void *, uint32_t, AudioQueueRef *);
static OSStatus (*AQAllocateBuffer)(AudioQueueRef, uint32_t, AudioQueueBufferRef *);
static OSStatus (*AQEnqueueBuffer)(AudioQueueRef, AudioQueueBufferRef, uint32_t, const void *);
static OSStatus (*AQStart)(AudioQueueRef, const void *);
static OSStatus (*AQStop)(AudioQueueRef, unsigned char);
static OSStatus (*AQDispose)(AudioQueueRef, unsigned char);
static OSStatus (*ASInitialize)(void *, void *, void *, void *);
static OSStatus (*ASSetProperty)(uint32_t, uint32_t, const void *);
static OSStatus (*ASSetActive)(unsigned char);
static OSStatus (*ASGetProperty)(uint32_t, uint32_t *, void *);
static OSStatus (*AQNewInput)(const AudioStreamBasicDescription *, AQInputCallback, void *,
                              void *, void *, uint32_t, AudioQueueRef *);
static unsigned char (*CFStringGetCStr)(const void *, char *, long, uint32_t);
typedef void (*ASPropertyListener)(void *user, uint32_t id, uint32_t size, const void *data);
static OSStatus (*ASAddListener)(uint32_t, ASPropertyListener, void *);
static int32_t (*CFRunLoopRunInMode)(const void *mode, double seconds, unsigned char once);
static const void **kCFRunLoopDefaultMode;
static const void *(*CFDictionaryGetValue)(const void *, const void *);
static const void *(*CFStringCreateWithCString)(const void *, const char *, uint32_t);
static unsigned char (*CFNumberGetValue)(const void *, long, void *);

#define RATE    44100
#define NBUF    3
#define FRAMES  1024

static struct {
    double phase, step;
    int amp;
    uint32_t left;          /* frames still to play */
    volatile int drained;   /* buffers that came back after the tone ended */
    volatile int paused;    /* the route went away under us (headphones unplugged) */
} T;

static void say(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    int n, fd;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    fd = open("/dev/console", O_WRONLY);
    if (write(fd < 0 ? 2 : fd, line, (size_t)n) < 0) { /* nothing to do */ }
    if (fd >= 0) close(fd);
}

static void fill(AudioQueueRef aq, AudioQueueBufferRef b)
{
    int16_t *p = b->mAudioData;
    uint32_t n = 0;
    if (!T.left) {
        T.drained++;
        return;                                 /* not re-queued: the queue runs dry */
    }
    while (n < FRAMES && T.left) {
        int s = (int)(sin(T.phase) * T.amp);
        p[2 * n] = (int16_t)s;
        p[2 * n + 1] = (int16_t)(s / 2);
        T.phase += T.step;
        if (T.phase > 2 * M_PI) T.phase -= 2 * M_PI;
        n++;
        T.left--;
    }
    b->mAudioDataByteSize = n * 4;
    AQEnqueueBuffer(aq, b, 0, NULL);
}

static void callback(void *user, AudioQueueRef aq, AudioQueueBufferRef b)
{
    (void)user;
    fill(aq, b);
}

static int load(void)
{
    void *h = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW);
    if (!h) {
        say("tone: no AudioToolbox: %s", dlerror());
        return 0;
    }
#define SYM(v, n) if (!(*(void **)&v = dlsym(h, n))) { say("tone: no %s", n); return 0; }
    SYM(AQNewOutput, "AudioQueueNewOutput");
    SYM(AQAllocateBuffer, "AudioQueueAllocateBuffer");
    SYM(AQEnqueueBuffer, "AudioQueueEnqueueBuffer");
    SYM(AQStart, "AudioQueueStart");
    SYM(AQStop, "AudioQueueStop");
    SYM(AQDispose, "AudioQueueDispose");
    SYM(ASInitialize, "AudioSessionInitialize");
    SYM(ASSetProperty, "AudioSessionSetProperty");
    SYM(ASSetActive, "AudioSessionSetActive");
    SYM(ASGetProperty, "AudioSessionGetProperty");
    SYM(AQNewInput, "AudioQueueNewInput");
    *(void **)&ASAddListener = dlsym(h, "AudioSessionAddPropertyListener");
    h = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    if (h) {
        *(void **)&CFStringGetCStr = dlsym(h, "CFStringGetCString");
        *(void **)&CFRunLoopRunInMode = dlsym(h, "CFRunLoopRunInMode");
        *(void **)&kCFRunLoopDefaultMode = dlsym(h, "kCFRunLoopDefaultMode");
        *(void **)&CFDictionaryGetValue = dlsym(h, "CFDictionaryGetValue");
        *(void **)&CFStringCreateWithCString = dlsym(h, "CFStringCreateWithCString");
        *(void **)&CFNumberGetValue = dlsym(h, "CFNumberGetValue");
    }
#undef SYM
    return 1;
}

/* wait on the main run loop, where AudioSession delivers its listeners */
static void nap(int ms)
{
    if (CFRunLoopRunInMode && kCFRunLoopDefaultMode) CFRunLoopRunInMode(*kCFRunLoopDefaultMode, ms / 1000.0, 0);
    else usleep((useconds_t)ms * 1000);
}

static const char *route_name(char *name, int len)
{
    void *route = NULL;
    uint32_t sz = sizeof route;
    if (!ASGetProperty(kAudioSessionProperty_AudioRoute, &sz, &route) && route && CFStringGetCStr)
        CFStringGetCStr(route, name, len, kCFStringEncodingUTF8);
    return name;
}

/* kAudioSessionProperty_AudioRouteChange: log the reason and the new route;
   reason 2 (kAudioSessionRouteChangeReason_OldDeviceUnavailable, headphones
   unplugged) pauses the tone, as the iPod app pauses its music */
static void route_changed(void *user, uint32_t id, uint32_t size, const void *data)
{
    int32_t reason = -1;
    char name[64] = "?", old[64] = "?";
    (void)user; (void)size;
    if (id != kAudioSessionProperty_AudioRouteChange) return;
    if (data && CFDictionaryGetValue && CFStringCreateWithCString && CFNumberGetValue) {
        const void *k = CFStringCreateWithCString(NULL, "OutputDeviceDidChange_Reason", kCFStringEncodingUTF8);
        const void *v = k ? CFDictionaryGetValue(data, k) : NULL;
        if (v) CFNumberGetValue(v, kCFNumberSInt32Type, &reason);
        k = CFStringCreateWithCString(NULL, "OutputDeviceDidChange_OldRoute", kCFStringEncodingUTF8);
        v = k ? CFDictionaryGetValue(data, k) : NULL;
        if (v && CFStringGetCStr) CFStringGetCStr(v, old, sizeof old, kCFStringEncodingUTF8);
    }
    route_name(name, sizeof name);
    say("tone: route change, reason %d%s: \"%s\" -> \"%s\"%s", (int)reason,
        reason == 1 ? " (new device available)" : reason == 2 ? " (old device unavailable)" : "",
        old, name, reason == 2 && T.left ? ": pausing, as the iPod app does on unplug" : "");
    if (reason == 2) T.paused = 1;
}

static int play(int ms, int hz, int amp)
{
    AudioStreamBasicDescription f;
    AudioQueueRef aq = NULL;
    AudioQueueBufferRef b[NBUF];
    OSStatus st;
    int i, waited;

    memset(&f, 0, sizeof f);
    f.mSampleRate = RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFlagsSignedPacked;
    f.mBytesPerPacket = f.mBytesPerFrame = 4;
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    st = AQNewOutput(&f, callback, NULL, NULL, NULL, 0, &aq);
    if (st) return st;
    T.phase = 0;
    T.step = 2 * M_PI * hz / RATE;
    T.amp = amp;
    T.left = (uint32_t)((int64_t)RATE * ms / 1000);
    T.drained = 0;
    for (i = 0; i < NBUF; i++) {
        st = AQAllocateBuffer(aq, FRAMES * 4, &b[i]);
        if (st) goto out;
        fill(aq, b[i]);
    }
    st = AQStart(aq, NULL);
    if (st) goto out;
    T.paused = 0;
    for (waited = 0; T.drained < NBUF && !T.paused && waited < ms + 5000; waited += 20) nap(20);
    AQStop(aq, T.paused);
    if (T.paused) say("tone: paused after %d ms (route went away)", waited);
out:
    AQDispose(aq, 1);
    return st;
}

/* ---- record mode ------------------------------------------------------------ */
#define REC_MAX (RATE * 4)
static int16_t rec_buf[REC_MAX];
static struct {
    uint32_t frames, want, nonzero, peak, steps_ok, steps_bad, zc, bufs;
    double sq;
    int16_t prev, first[8], mid[24];
    int nmid;
    int32_t dval[6];
    uint32_t dcnt[6];
    int have_prev, nfirst;
    volatile int done;
} R;

static void rec_callback(void *user, AudioQueueRef aq, AudioQueueBufferRef b, const void *ts,
                         uint32_t np, const void *pd)
{
    const int16_t *p = b->mAudioData;
    uint32_t n = b->mAudioDataByteSize / 2, i;
    (void)user; (void)ts; (void)np; (void)pd;
    R.bufs++;
    for (i = 0; i < n && R.frames < R.want; i++, R.frames++) {
        int16_t s = p[i];
        uint32_t mag = (uint32_t)(s < 0 ? -s : s);
        if (R.frames < REC_MAX) rec_buf[R.frames] = s;
        if (s) R.nonzero++;
        if (mag > R.peak) R.peak = mag;
        R.sq += (double)s * s;
        if (R.nfirst < 8) R.first[R.nfirst++] = s;
        if (R.frames >= R.want / 2 && R.nmid < 24) R.mid[R.nmid++] = s;
        if (R.have_prev) {
            int32_t dv = s - R.prev;
            int k;
            for (k = 0; k < 6 && R.dcnt[k] && R.dval[k] != dv; k++) {}
            if (k < 6) { R.dval[k] = dv; R.dcnt[k]++; }
        }
        if (R.have_prev) {
            uint16_t d = (uint16_t)(s - R.prev);
            if (d <= 1) R.steps_ok++; else R.steps_bad++;
            if ((R.prev < 0) != (s < 0)) R.zc++;
        }
        R.prev = s;
        R.have_prev = 1;
    }
    if (R.frames >= R.want) { R.done = 1; return; }
    AQEnqueueBuffer(aq, b, 0, NULL);
}

/* Least-squares sine fit over the middle half of the recording: the best
   frequency (searched around the zero-crossing estimate), its amplitude, and
   what is left (noise, distortion, clicks from late or stale input data). */
static void sine_fit(void)
{
    uint32_t n0 = R.frames / 4, n = R.frames / 2 > RATE / 2 ? RATE / 2 : R.frames / 2, i;
    double f0 = R.zc / 2.0 * RATE / R.frames, best_f = 0, best_amp = 0, best_res = 1e30, energy = 0;
    double f, lo = f0 * 0.98, hi = f0 * 1.02;
    if (f0 < 20) return;
    for (i = 0; i < n; i++) energy += (double)rec_buf[n0 + i] * rec_buf[n0 + i];
    for (int pass = 0; pass < 3; pass++) {
        double step = (hi - lo) / 16;
        for (f = lo; f <= hi; f += step) {
            double w = 2 * M_PI * f / RATE, a = 0, b = 0, res = 0;
            for (i = 0; i < n; i++) {
                a += rec_buf[n0 + i] * cos(w * i);
                b += rec_buf[n0 + i] * sin(w * i);
            }
            a *= 2.0 / n;
            b *= 2.0 / n;
            for (i = 0; i < n; i++) {
                double e = rec_buf[n0 + i] - a * cos(w * i) - b * sin(w * i);
                res += e * e;
            }
            if (res < best_res) { best_res = res; best_f = f; best_amp = sqrt(a * a + b * b); }
        }
        lo = best_f - step;
        hi = best_f + step;
    }
    say("tone: rec sine fit: %.2f Hz, amplitude %.1f, residual %.1f dB below the signal (rms %.1f)",
        best_f, best_amp, 10 * log10((energy > best_res ? energy - best_res : 1) / (best_res > 1 ? best_res : 1)),
        sqrt(best_res / n));
}

static void log_route(const char *when)
{
    uint32_t avail = 0, sz = sizeof avail;
    void *route = NULL;
    char name[64] = "?";
    OSStatus a = ASGetProperty(kAudioSessionProperty_AudioInputAvailable, &sz, &avail), r;
    sz = sizeof route;
    r = ASGetProperty(kAudioSessionProperty_AudioRoute, &sz, &route);
    if (!r && route && CFStringGetCStr) CFStringGetCStr(route, name, sizeof name, kCFStringEncodingUTF8);
    say("tone: rec %s: input available %u (status %d), route \"%s\" (status %d)", when, avail, (int)a, name, (int)r);
}

static void save_wav(const char *dir, int n)
{
    char path[256];
    uint8_t h[44];
    uint32_t bytes = (R.frames < REC_MAX ? R.frames : REC_MAX) * 2;
    FILE *f;
    snprintf(path, sizeof path, "%s/rec%d.wav", dir, n);
    f = fopen(path, "wb");
    if (!f) { say("tone: rec cannot write %s", path); return; }
#define LE32(o, v) (h[o] = (uint8_t)(v), h[o + 1] = (uint8_t)((v) >> 8), h[o + 2] = (uint8_t)((v) >> 16), h[o + 3] = (uint8_t)((v) >> 24))
    memcpy(h, "RIFF", 4); LE32(4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8); LE32(16, 16);
    h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0; LE32(24, RATE); LE32(28, RATE * 2); h[32] = 2; h[33] = 0;
    h[34] = 16; h[35] = 0; memcpy(h + 36, "data", 4); LE32(40, bytes);
#undef LE32
    if (fwrite(h, 1, 44, f) != 44 || fwrite(rec_buf, 1, bytes, f) != bytes) say("tone: rec short write %s", path);
    fclose(f);
    sync();
    say("tone: rec saved %s", path);
}

static int record(int ms)
{
    AudioStreamBasicDescription f;
    AudioQueueRef aq = NULL;
    AudioQueueBufferRef b[NBUF];
    OSStatus st;
    int i, waited;

    memset(&f, 0, sizeof f);
    f.mSampleRate = RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFlagsSignedPacked;
    f.mBytesPerPacket = f.mBytesPerFrame = 2;
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = 1;
    f.mBitsPerChannel = 16;
    memset(&R, 0, sizeof R);
    R.want = (uint32_t)((int64_t)RATE * ms / 1000);
    st = AQNewInput(&f, rec_callback, NULL, NULL, NULL, 0, &aq);
    if (st) return st;
    for (i = 0; i < NBUF; i++) {
        st = AQAllocateBuffer(aq, FRAMES * 2, &b[i]);
        if (st) goto out;
        st = AQEnqueueBuffer(aq, b[i], 0, NULL);
        if (st) goto out;
    }
    st = AQStart(aq, NULL);
    if (st) goto out;
    for (waited = 0; !R.done && waited < ms + 5000; waited += 20) usleep(20000);
    AQStop(aq, T.paused);
    say("tone: rec %u of %u frames in %u buffers: nonzero %u, peak %u, rms %.0f, ~%.0f Hz, "
        "ramp steps ok %u bad %u, first %d %d %d %d %d %d %d %d",
        R.frames, R.want, R.bufs, R.nonzero, R.peak, R.frames ? sqrt(R.sq / R.frames) : 0.0,
        R.frames ? R.zc / 2.0 * RATE / R.frames : 0.0, R.steps_ok, R.steps_bad,
        R.first[0], R.first[1], R.first[2], R.first[3], R.first[4], R.first[5], R.first[6], R.first[7]);
    if (R.frames >= 8192) sine_fit();
    say("tone: rec middle samples %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
        R.mid[0], R.mid[1], R.mid[2], R.mid[3], R.mid[4], R.mid[5], R.mid[6], R.mid[7], R.mid[8], R.mid[9],
        R.mid[10], R.mid[11], R.mid[12], R.mid[13], R.mid[14], R.mid[15], R.mid[16], R.mid[17], R.mid[18],
        R.mid[19], R.mid[20], R.mid[21], R.mid[22], R.mid[23]);
    say("tone: rec first step sizes seen: %d x%u, %d x%u, %d x%u, %d x%u, %d x%u, %d x%u",
        (int)R.dval[0], R.dcnt[0], (int)R.dval[1], R.dcnt[1], (int)R.dval[2], R.dcnt[2],
        (int)R.dval[3], R.dcnt[3], (int)R.dval[4], R.dcnt[4], (int)R.dval[5], R.dcnt[5]);
out:
    AQDispose(aq, 1);
    return st;
}

static int rec_main(int argc, char **argv)
{
    int count = argc > 2 ? atoi(argv[2]) : 1;
    int interval = argc > 3 ? atoi(argv[3]) : 10;
    int ms = argc > 4 ? atoi(argv[4]) : 2000;
    int delay = argc > 5 ? atoi(argv[5]) : 30;
    const char *dir = argc > 6 && strcmp(argv[6], "-") ? argv[6] : NULL;
    uint32_t cat = argc > 7 && !strcmp(argv[7], "reca") ? kAudioSessionCategory_RecordAudio
                                                        : kAudioSessionCategory_PlayAndRecord;
    OSStatus st;
    int i, tries;

    sleep((unsigned)delay);
    for (tries = 0; (st = ASInitialize(NULL, NULL, NULL, NULL)) && tries < 60; tries++) sleep(2);
    if (st) say("tone: AudioSessionInitialize %d (carrying on)", (int)st);
    st = ASSetProperty(kAudioSessionProperty_AudioCategory, sizeof cat, &cat);
    if (st) say("tone: rec category %d (carrying on)", (int)st);
    st = ASSetActive(1);
    if (st) say("tone: AudioSessionSetActive %d (carrying on)", (int)st);
    for (i = 0; i < count; i++) {
        if (i) sleep((unsigned)interval);
        log_route(i ? "again" : "start");
        say("tone: rec #%d: %d ms, 44100 Hz mono, category %s", i + 1, ms,
            cat == kAudioSessionCategory_RecordAudio ? "RecordAudio" : "PlayAndRecord");
        st = record(ms);
        if (st) {
            say("tone: rec AudioQueue error %d: giving up", (int)st);
            return 1;
        }
        if (dir) save_wav(dir, i + 1);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int count = argc > 1 ? atoi(argv[1]) : 1;
    int interval = argc > 2 ? atoi(argv[2]) : 10;
    int ms = argc > 3 ? atoi(argv[3]) : 400;
    int hz = argc > 4 ? atoi(argv[4]) : 1000;
    int amp = argc > 5 ? atoi(argv[5]) : 6000;
    int delay = argc > 6 ? atoi(argv[6]) : 30;
    uint32_t cat = kAudioSessionCategory_MediaPlayback;
    OSStatus st;
    int i, tries;

#ifdef TONE_OLD
    say("tone: start (3.x/2.x build), %d argument(s)", argc - 1);   /* bring-up trace */
#endif
    if (!load()) return 1;
#ifdef TONE_OLD
    say("tone: AudioToolbox loaded");
#endif
    if (argc > 1 && !strcmp(argv[1], "rec")) return rec_main(argc, argv);
    sleep((unsigned)delay);                         /* let SpringBoard and mediaserverd settle */
    for (tries = 0; (st = ASInitialize(NULL, NULL, NULL, NULL)) && tries < 60; tries++) sleep(2);
    if (st) say("tone: AudioSessionInitialize %d (carrying on)", (int)st);
    st = ASSetProperty(kAudioSessionProperty_AudioCategory, sizeof cat, &cat);
    if (st) say("tone: audio category %d (carrying on)", (int)st);
    st = ASSetActive(1);
    if (st) say("tone: AudioSessionSetActive %d (carrying on)", (int)st);
    if (ASAddListener && (st = ASAddListener(kAudioSessionProperty_AudioRouteChange, route_changed, NULL)))
        say("tone: route change listener %d (carrying on)", (int)st);
    for (i = 0; i < count; i++) {
        char name[64] = "?";
        if (i) nap(interval * 1000);
        say("tone: #%d %d Hz, %d ms, amplitude %d (L), %d (R), route \"%s\"", i + 1, hz, ms, amp, amp / 2,
            route_name(name, sizeof name));
        st = play(ms, hz, amp);
        if (st) {
            say("tone: AudioQueue error %d: giving up", (int)st);
            return 1;
        }
        say("tone: #%d done", i + 1);
    }
    return 0;
}

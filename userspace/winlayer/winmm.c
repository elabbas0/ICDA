/* winmm: waveOut sound, multimedia timers, and no joysticks or MIDI.
 *
 * waveOut plays through ICDA's sound mixer (an audio stream per device, 16-
 * bit PCM); 8-bit and float samples are converted.  A buffer is "done" once
 * the mixer has taken it, so the program refills while the mixer still
 * plays.  With no sound (headless, or no audio device), buffers are timed
 * as if played, so programs that wait for their sound keep their pace.
 * The done callbacks come from a thread of ours that is set up to call
 * Windows code, as Windows calls them from its own. */
#define _GNU_SOURCE
#include "wingui.h"
#include "../icda_sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define MMSYSERR_NOERROR     0
#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_INVALHANDLE 5
#define MMSYSERR_NODRIVER    6
#define MMSYSERR_INVALPARAM  11
#define WAVERR_BADFORMAT     32
#define WAVERR_STILLPLAYING  33

#define WHDR_DONE     0x1
#define WHDR_PREPARED 0x2
#define WHDR_INQUEUE  0x10

#define WOM_OPEN  0x3BB
#define WOM_CLOSE 0x3BC
#define WOM_DONE  0x3BD

typedef struct wavehdr {
    char           *lpData;
    DWORD           dwBufferLength, dwBytesRecorded;
    DWORD_PTR       dwUser;
    DWORD           dwFlags, dwLoops;
    struct wavehdr *lpNext;
    DWORD_PTR       reserved;
} WAVEHDR;

typedef struct {
    WORD  wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD  nBlockAlign, wBitsPerSample, cbSize;
} __attribute__((packed)) WAVEFORMATEX;

typedef void (WCALLBACK *waveproc_t)(HANDLE, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);

typedef struct {
    int             magic;
    WAVEFORMATEX    fmt;
    int             is_float;
    DWORD           cb_kind;
    DWORD_PTR       cb, inst;
    pthread_t       th;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    WAVEHDR        *head, *tail;     /* queued, oldest first */
    int             stop, reset, paused;
    long            stream;          /* ICDA audio stream, 0: none */
    uint64_t        played;          /* bytes */
    DWORD           volume;
} wout_t;

#define WOUT_MAGIC 0x574F5554

BOOL WINAPI SetEvent(HANDLE h);
BOOL WINAPI PostMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp);
BOOL WINAPI PostThreadMessageW(DWORD tid, UINT m, WPARAM wp, LPARAM lp);

static void notify(wout_t *w, UINT msg, DWORD_PTR p1) {
    switch (w->cb_kind) {
    case 0x30000: ((waveproc_t)w->cb)((HANDLE)w, msg, w->inst, p1, 0); break;   /* CALLBACK_FUNCTION */
    case 0x50000: SetEvent((HANDLE)w->cb); break;                                 /* CALLBACK_EVENT */
    case 0x10000: PostMessageW((HWND)w->cb, msg, (WPARAM)w, (LPARAM)p1); break;   /* CALLBACK_WINDOW */
    case 0x20000: PostThreadMessageW((DWORD)w->cb, msg, (WPARAM)w, (LPARAM)p1); break;   /* CALLBACK_THREAD */
    default: break;
    }
}

/* the samples as 16-bit, scaled by the volume (0..0xFFFF per channel) */
static size_t to_s16(const wout_t *w, const uint8_t *in, size_t n, int16_t *out) {
    size_t count = 0;
    int vl = (int)(w->volume & 0xFFFF), vr = (int)(w->volume >> 16);
    if (w->is_float && w->fmt.wBitsPerSample == 32) {
        const float *f = (const float *)in;
        count = n / 4;
        for (size_t i = 0; i < count; i++) {
            float v = f[i] * 32767.0f;
            out[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
        }
    } else if (w->fmt.wBitsPerSample == 8) {
        count = n;
        for (size_t i = 0; i < count; i++) out[i] = (int16_t)(((int)in[i] - 128) << 8);
    } else if (w->fmt.wBitsPerSample == 16) {
        count = n / 2;
        memcpy(out, in, count * 2);
    } else if (w->fmt.wBitsPerSample == 32) {
        const int32_t *s = (const int32_t *)in;
        count = n / 4;
        for (size_t i = 0; i < count; i++) out[i] = (int16_t)(s[i] >> 16);
    } else if (w->fmt.wBitsPerSample == 24) {
        count = n / 3;
        for (size_t i = 0; i < count; i++) out[i] = (int16_t)(in[i * 3 + 1] | (in[i * 3 + 2] << 8));
    }
    if (vl != 0xFFFF || vr != 0xFFFF)
        for (size_t i = 0; i < count; i++) out[i] = (int16_t)((int)out[i] * ((w->fmt.nChannels == 2 && (i & 1)) ? vr : vl) / 0xFFFF);
    return count;
}

static void play(wout_t *w, WAVEHDR *h) {
    size_t bytes = h->dwBufferLength;
    if (w->stream > 0) {
        size_t max = bytes * 2 + 16;
        int16_t *pcm = malloc(max);
        size_t n = to_s16(w, (const uint8_t *)h->lpData, bytes, pcm) * 2, done = 0;
        int idle = 0;
        while (done < n && !w->reset && !w->stop) {
            long took = icda_audio_stream_write(w->stream, (const int16_t *)((const uint8_t *)pcm + done), n - done);
            if (took < 0) break;
            if (took == 0) {
                struct timespec ts = { 0, 2000000 };
                if (++idle > 3000) break;        /* the device went away */
                nanosleep(&ts, 0);
                continue;
            }
            idle = 0;
            done += (size_t)took;
        }
        free(pcm);
    } else {
        /* no sound: as long as it would have played */
        uint64_t ns = w->fmt.nAvgBytesPerSec ? (uint64_t)bytes * 1000000000ull / w->fmt.nAvgBytesPerSec : 0;
        struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
        nanosleep(&ts, 0);
    }
    w->played += bytes;
}

static void *player(void *arg) {
    wout_t *w = arg;
    win_thread_setup();
    pthread_mutex_lock(&w->lock);
    for (;;) {
        WAVEHDR *h;
        while (!w->stop && (!w->head || w->paused)) pthread_cond_wait(&w->cond, &w->lock);
        if (w->stop) break;
        h = w->head;
        pthread_mutex_unlock(&w->lock);
        if (!w->reset) play(w, h);
        pthread_mutex_lock(&w->lock);
        /* done: off the queue, then the program is told */
        if (w->head == h) {
            w->head = h->lpNext;
            if (!w->head) w->tail = 0;
        }
        h->lpNext = 0;
        h->dwFlags = (h->dwFlags & ~(DWORD)WHDR_INQUEUE) | WHDR_DONE;
        pthread_mutex_unlock(&w->lock);
        notify(w, WOM_DONE, (DWORD_PTR)h);
        pthread_mutex_lock(&w->lock);
        pthread_cond_broadcast(&w->cond);
    }
    pthread_mutex_unlock(&w->lock);
    return 0;
}

static wout_t *wout_get(HANDLE h) {
    wout_t *w = h;
    return w && w->magic == WOUT_MAGIC ? w : 0;
}

static const uint8_t KSDATAFORMAT_SUBTYPE_IEEE_FLOAT[16] = { 3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };

static UINT WINAPI waveOutOpen(HANDLE *out, UINT id, const WAVEFORMATEX *f, DWORD_PTR cb, DWORD_PTR inst, DWORD flags) {
    wout_t *w;
    int is_float = f->wFormatTag == 3;
    (void)id;
    if (f->wFormatTag == 0xFFFE && f->cbSize >= 22)         /* WAVE_FORMAT_EXTENSIBLE */
        is_float = !memcmp((const uint8_t *)f + sizeof(WAVEFORMATEX) + 6, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 16);
    if (!(f->wFormatTag == 1 || f->wFormatTag == 3 || f->wFormatTag == 0xFFFE) || f->nChannels < 1 || f->nChannels > 2 ||
        !(f->wBitsPerSample == 8 || f->wBitsPerSample == 16 || f->wBitsPerSample == 24 || f->wBitsPerSample == 32))
        return WAVERR_BADFORMAT;
    if (flags & 1) return MMSYSERR_NOERROR;                 /* WAVE_FORMAT_QUERY */
    w = calloc(1, sizeof(*w));
    w->magic = WOUT_MAGIC;
    w->fmt = *f;
    w->is_float = is_float;
    w->cb_kind = flags & 0x70000;
    w->cb = cb;
    w->inst = inst;
    w->volume = 0xFFFFFFFF;
    pthread_mutex_init(&w->lock, 0);
    pthread_cond_init(&w->cond, 0);
    if (!bk_headless()) {
        long s = icda_audio_stream_open(f->nSamplesPerSec, f->nChannels);
        w->stream = s > 0 ? s : 0;
        if (w->stream) (void)icda_audio_stream_control(w->stream, 0, 256);
    }
    if (wtrace) wlog("waveOut: %u Hz, %u channels, %u bits%s, %s", f->nSamplesPerSec, f->nChannels, f->wBitsPerSample,
                     is_float ? " float" : "", w->stream ? "playing" : "silent");
    pthread_create(&w->th, 0, player, w);
    *out = w;
    notify(w, WOM_OPEN, 0);
    return MMSYSERR_NOERROR;
}

static UINT WINAPI waveOutPrepareHeader(HANDLE h, WAVEHDR *hdr, UINT size) {
    (void)h, (void)size;
    hdr->dwFlags |= WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

static UINT WINAPI waveOutUnprepareHeader(HANDLE h, WAVEHDR *hdr, UINT size) {
    (void)h, (void)size;
    if (hdr->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
    hdr->dwFlags &= ~(DWORD)WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

static UINT WINAPI waveOutWrite(HANDLE h, WAVEHDR *hdr, UINT size) {
    wout_t *w = wout_get(h);
    (void)size;
    if (!w) return MMSYSERR_INVALHANDLE;
    pthread_mutex_lock(&w->lock);
    hdr->dwFlags = (hdr->dwFlags & ~(DWORD)WHDR_DONE) | WHDR_INQUEUE;
    hdr->lpNext = 0;
    if (w->tail) w->tail->lpNext = hdr;
    else w->head = hdr;
    w->tail = hdr;
    w->reset = 0;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
    return MMSYSERR_NOERROR;
}

/* all queued buffers come back, unplayed */
static UINT WINAPI waveOutReset(HANDLE h) {
    wout_t *w = wout_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    pthread_mutex_lock(&w->lock);
    w->reset = 1;
    pthread_cond_broadcast(&w->cond);
    while (w->head) pthread_cond_wait(&w->cond, &w->lock);
    w->reset = 0;
    pthread_mutex_unlock(&w->lock);
    return MMSYSERR_NOERROR;
}

static UINT WINAPI waveOutClose(HANDLE h) {
    wout_t *w = wout_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    waveOutReset(h);
    pthread_mutex_lock(&w->lock);
    w->stop = 1;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->th, 0);
    if (w->stream) icda_audio_stream_close(w->stream);
    notify(w, WOM_CLOSE, 0);
    w->magic = 0;
    free(w);
    return MMSYSERR_NOERROR;
}

static UINT WINAPI waveOutPause(HANDLE h) {
    wout_t *w = wout_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    w->paused = 1;
    if (w->stream) icda_audio_stream_control(w->stream, 1, 256);
    return MMSYSERR_NOERROR;
}

static UINT WINAPI waveOutRestart(HANDLE h) {
    wout_t *w = wout_get(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    pthread_mutex_lock(&w->lock);
    w->paused = 0;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
    if (w->stream) icda_audio_stream_control(w->stream, 0, 256);
    return MMSYSERR_NOERROR;
}

typedef struct {
    UINT wType;
    union { DWORD ms, sample, cb, ticks; uint8_t pad[8]; } u;
} MMTIME;

static UINT WINAPI waveOutGetPosition(HANDLE h, MMTIME *t, UINT size) {
    wout_t *w = wout_get(h);
    (void)size;
    if (!w) return MMSYSERR_INVALHANDLE;
    switch (t->wType) {
    case 1: t->u.ms = (DWORD)(w->fmt.nAvgBytesPerSec ? w->played * 1000 / w->fmt.nAvgBytesPerSec : 0); break;
    case 2: t->u.sample = (DWORD)(w->fmt.nBlockAlign ? w->played / w->fmt.nBlockAlign : 0); break;
    default: t->wType = 4, t->u.cb = (DWORD)w->played; break;     /* TIME_BYTES */
    }
    return MMSYSERR_NOERROR;
}

static UINT WINAPI waveOutGetVolume(HANDLE h, DWORD *v) {
    wout_t *w = wout_get(h);
    *v = w ? w->volume : 0xFFFFFFFF;
    return MMSYSERR_NOERROR;
}
static UINT WINAPI waveOutSetVolume(HANDLE h, DWORD v) {
    wout_t *w = wout_get(h);
    if (w) w->volume = v;
    return MMSYSERR_NOERROR;
}
static UINT WINAPI waveOutGetNumDevs(void) { return 1; }

typedef struct {
    WORD  wMid, wPid;
    UINT  vDriverVersion;
    WCHAR szPname[32];
    DWORD dwFormats;
    WORD  wChannels, wReserved1;
    DWORD dwSupport;
} WAVEOUTCAPSW;

static UINT WINAPI waveOutGetDevCapsW(UINT_PTR id, WAVEOUTCAPSW *c, UINT size) {
    (void)id;
    if (size < sizeof(*c)) return MMSYSERR_INVALPARAM;
    memset(c, 0, sizeof(*c));
    c->wMid = 1;
    c->wPid = 1;
    c->vDriverVersion = 0x100;
    utf8_to_w("ICDA Sound", -1, c->szPname, 32);
    c->dwFormats = 0xFFFFF;
    c->wChannels = 2;
    c->dwSupport = 0xC;      /* volume, left/right volume */
    return MMSYSERR_NOERROR;
}

static UINT WINAPI waveOutGetDevCapsA(UINT_PTR id, void *c, UINT size) {
    WAVEOUTCAPSW w;
    uint8_t *o = c;
    if (size < 52) return MMSYSERR_INVALPARAM;
    waveOutGetDevCapsW(id, &w, sizeof(w));
    memset(o, 0, 52);
    memcpy(o, &w, 8);
    snprintf((char *)o + 8, 32, "ICDA Sound");
    memcpy(o + 40, &w.dwFormats, 12);
    return MMSYSERR_NOERROR;
}

static UINT WINAPI waveOutGetErrorTextW(UINT e, WCHAR *t, UINT n) {
    char m[64];
    snprintf(m, sizeof(m), "Sound error %u", e);
    utf8_to_w(m, -1, t, (int)n);
    return MMSYSERR_NOERROR;
}
static UINT WINAPI waveOutGetErrorTextA(UINT e, char *t, UINT n) {
    snprintf(t, n, "Sound error %u", e);
    return MMSYSERR_NOERROR;
}
static UINT WINAPI waveOutMessage(HANDLE h, UINT m, DWORD_PTR a, DWORD_PTR b) {
    (void)h, (void)m, (void)a, (void)b;
    return MMSYSERR_NOERROR;
}
static UINT WINAPI waveOutGetID(HANDLE h, UINT *id) {
    (void)h;
    *id = 0;
    return MMSYSERR_NOERROR;
}

/* no capture, joysticks or MIDI */
static UINT WINAPI wmm_zero(void) { return 0; }
static UINT WINAPI wmm_nodriver(void) { return MMSYSERR_NODRIVER; }
static UINT WINAPI wmm_baddev(void) { return MMSYSERR_BADDEVICEID; }
static UINT WINAPI joy_unplugged(void) { return 167; }    /* JOYERR_UNPLUGGED */

/* ---- timers --------------------------------------------------------------------------------- */

static UINT WINAPI timeBeginPeriod(UINT p) {
    (void)p;
    return 0;
}
static UINT WINAPI timeEndPeriod(UINT p) {
    (void)p;
    return 0;
}
static DWORD WINAPI timeGetTime(void) { return (DWORD)(mono_ns() / 1000000ull); }

typedef struct { UINT wPeriodMin, wPeriodMax; } TIMECAPS;
static UINT WINAPI timeGetDevCaps(TIMECAPS *c, UINT size) {
    (void)size;
    c->wPeriodMin = 1;
    c->wPeriodMax = 1000000;
    return 0;
}
static UINT WINAPI timeGetSystemTime(MMTIME *t, UINT size) {
    (void)size;
    t->wType = 1;
    t->u.ms = timeGetTime();
    return 0;
}

typedef void (WCALLBACK *TIMECALLBACK)(UINT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);

typedef struct {
    int          used, stop;
    UINT         id, delay;
    int          periodic;
    TIMECALLBACK fn;
    DWORD_PTR    user;
    UINT         flags;
    pthread_t    th;
} mmtimer_t;

static mmtimer_t mmtimers[16];
static pthread_mutex_t mmlock = PTHREAD_MUTEX_INITIALIZER;

static void *mmtimer_thread(void *arg) {
    mmtimer_t *t = arg;
    uint64_t next = mono_ns();
    win_thread_setup();
    do {
        uint64_t now;
        struct timespec ts;
        next += (uint64_t)t->delay * 1000000ull;
        now = mono_ns();
        if (next > now) {
            ts.tv_sec = (time_t)((next - now) / 1000000000ull);
            ts.tv_nsec = (long)((next - now) % 1000000000ull);
            nanosleep(&ts, 0);
        }
        if (t->stop) break;
        if (t->flags & 0x10) SetEvent((HANDLE)t->fn);         /* TIME_CALLBACK_EVENT_SET */
        else if (t->fn) t->fn(t->id, 0, t->user, 0, 0);
    } while (t->periodic && !t->stop);
    t->used = 0;
    return 0;
}

static UINT WINAPI timeSetEvent(UINT delay, UINT res, TIMECALLBACK fn, DWORD_PTR user, UINT flags) {
    (void)res;
    pthread_mutex_lock(&mmlock);
    for (int i = 0; i < 16; i++) {
        if (mmtimers[i].used) continue;
        mmtimers[i].used = 1;
        mmtimers[i].stop = 0;
        mmtimers[i].id = (UINT)i + 1;
        mmtimers[i].delay = delay ? delay : 1;
        mmtimers[i].periodic = (flags & 1) != 0;              /* TIME_PERIODIC */
        mmtimers[i].fn = fn;
        mmtimers[i].user = user;
        mmtimers[i].flags = flags;
        pthread_create(&mmtimers[i].th, 0, mmtimer_thread, &mmtimers[i]);
        pthread_detach(mmtimers[i].th);
        pthread_mutex_unlock(&mmlock);
        return (UINT)i + 1;
    }
    pthread_mutex_unlock(&mmlock);
    return 0;
}

static UINT WINAPI timeKillEvent(UINT id) {
    if (id < 1 || id > 16) return 97;   /* TIMERR_NOCANDO */
    mmtimers[id - 1].stop = 1;
    return 0;
}

static BOOL WINAPI PlaySoundW(const WCHAR *s, HMODULE m, DWORD f) {
    (void)s, (void)m, (void)f;
    return FALSE;
}

static const wexport_t mm[] = {
    WEXP(waveOutOpen), WEXP(waveOutPrepareHeader), WEXP(waveOutUnprepareHeader), WEXP(waveOutWrite), WEXP(waveOutReset),
    WEXP(waveOutClose), WEXP(waveOutPause), WEXP(waveOutRestart), WEXP(waveOutGetPosition), WEXP(waveOutGetVolume),
    WEXP(waveOutSetVolume), WEXP(waveOutGetNumDevs), WEXP(waveOutGetDevCapsW), WEXP(waveOutGetDevCapsA),
    WEXP(waveOutGetErrorTextW), WEXP(waveOutGetErrorTextA), WEXP(waveOutMessage), WEXP(waveOutGetID),
    WEXPN("waveInGetNumDevs", wmm_zero), WEXPN("waveInOpen", wmm_baddev), WEXPN("waveInClose", wmm_zero),
    WEXPN("waveInStart", wmm_zero), WEXPN("waveInStop", wmm_zero), WEXPN("waveInReset", wmm_zero),
    WEXPN("waveInAddBuffer", wmm_zero), WEXPN("waveInPrepareHeader", wmm_zero),
    WEXPN("waveInUnprepareHeader", wmm_zero), WEXPN("waveInGetDevCapsW", wmm_baddev),
    WEXPN("waveInGetDevCapsA", wmm_baddev), WEXPN("joyGetNumDevs", wmm_zero), WEXPN("joyGetPosEx", joy_unplugged),
    WEXPN("joyGetPos", joy_unplugged), WEXPN("joyGetDevCapsW", joy_unplugged), WEXPN("joyGetDevCapsA", joy_unplugged),
    WEXPN("midiOutGetNumDevs", wmm_zero), WEXPN("midiOutOpen", wmm_baddev), WEXPN("midiInGetNumDevs", wmm_zero),
    WEXPN("mixerGetNumDevs", wmm_zero), WEXPN("mixerOpen", wmm_baddev), WEXPN("auxGetNumDevs", wmm_zero),
    WEXPN("mciSendStringW", wmm_nodriver), WEXPN("mciSendStringA", wmm_nodriver),
    WEXPN("mciSendCommandW", wmm_nodriver), WEXPN("mciSendCommandA", wmm_nodriver), WEXP(timeBeginPeriod),
    WEXP(timeEndPeriod), WEXP(timeGetTime), WEXP(timeGetDevCaps), WEXP(timeGetSystemTime), WEXP(timeSetEvent),
    WEXP(timeKillEvent), WEXP(PlaySoundW), WEXPN("PlaySoundA", PlaySoundW), WEXPN("sndPlaySoundW", PlaySoundW),
    WEXPN("sndPlaySoundA", PlaySoundW),     /* PlaySoundW plays nothing and reads no argument */
};
const wdll_t dll_winmm = { "winmm.dll", mm, sizeof(mm) / sizeof(mm[0]) };

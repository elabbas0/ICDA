/* icdasink: a GStreamer audio sink that plays through ICDA's mixer.
 *
 * WebKit's media (video and audio elements, Web Audio) decodes with
 * GStreamer; this sink hands the sound to ICDA's kernel mixer (a stream
 * per sink, 16-bit interleaved PCM at the source's own rate) through the
 * kernel's gateway for Linux programs.  Its rank puts it ahead of the other
 * audio sinks, so autoaudiosink (which WebKit uses) picks it.
 *
 *   - At most LEAD_MS of sound waits in the mixer (its ring holds two
 *     seconds, and filled, a pause was heard two seconds late).
 *   - Pause and resume hold the stream in the mixer and let it go on, so
 *     nothing queued is lost and the pause is heard at once; a seek or
 *     flush (reset) drops what is queued.
 *   - Reset runs on another thread than write, which may be waiting for
 *     room: write sees the reset and returns, and the two never use the
 *     stream at once.
 *   - With no sound device, or every mixer stream taken, the sound is
 *     played to nowhere at its own pace and the stream is tried again every
 *     second: the video still plays, silent, instead of failing with an
 *     error (WebKit gives up on a page's media when its sink cannot open).
 *
 * Built into the Linux root as /usr/lib/gstreamer-1.0/libgsticda.so. */
#include <gst/gst.h>
#include <gst/audio/gstaudiosink.h>
#include <time.h>
#include "icda_sys.h"

#define GST_TYPE_ICDA_SINK (gst_icda_sink_get_type())
G_DECLARE_FINAL_TYPE(GstIcdaSink, gst_icda_sink, GST, ICDA_SINK, GstAudioSink)

#define LEAD_MS 400

struct _GstIcdaSink {
    GstAudioSink parent;
    GMutex       lock;           /* the stream: write against reset, pause and unprepare */
    long         stream;         /* 0: none (silent, at the sound's pace) */
    int          rate, channels, bpf;
    int          paused;
    gint         resets;         /* bumped by reset: a write waiting for room returns */
    gint64       retry_us;       /* when to try for a stream again */
    gint64       silent_until;   /* silent playback: when what was written would have played */
};

G_DEFINE_TYPE(GstIcdaSink, gst_icda_sink, GST_TYPE_AUDIO_SINK)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, format = (string) S16LE, layout = (string) interleaved, "
                    "rate = (int) [ 8000, 96000 ], channels = (int) [ 1, 2 ]"));

static void nap(void) {
    struct timespec t = { 0, 5 * 1000 * 1000 };
    nanosleep(&t, NULL);
}

/* with the lock held */
static void stream_open(GstIcdaSink *self) {
    long id = icda_audio_stream_open((uint32_t)self->rate, (uint32_t)self->channels);
    self->stream = id > 0 ? id : 0;
    if (self->stream) {
        (void)icda_audio_stream_control(self->stream, self->paused, 256);
    } else {
        self->retry_us = g_get_monotonic_time() + G_USEC_PER_SEC;
        GST_WARNING_OBJECT(self, "no mixer stream (no sound device, or all in use): playing silently");
    }
}

static void stream_close(GstIcdaSink *self) {
    if (self->stream) icda_audio_stream_close(self->stream);
    self->stream = 0;
}

static gboolean sink_open(GstAudioSink *s) {
    (void)s;
    return TRUE;
}

static gboolean sink_prepare(GstAudioSink *s, GstAudioRingBufferSpec *spec) {
    GstIcdaSink *self = GST_ICDA_SINK(s);
    g_mutex_lock(&self->lock);
    self->rate = GST_AUDIO_INFO_RATE(&spec->info);
    self->channels = GST_AUDIO_INFO_CHANNELS(&spec->info);
    self->bpf = GST_AUDIO_INFO_BPF(&spec->info);
    self->paused = 0;
    self->silent_until = 0;
    stream_open(self);
    g_mutex_unlock(&self->lock);
    return TRUE;
}

/* takes the whole buffer, waiting while LEAD_MS is queued */
static gint sink_write(GstAudioSink *s, gpointer data, guint length) {
    GstIcdaSink *self = GST_ICDA_SINK(s);
    const uint8_t *pcm = (const uint8_t *)data;
    gint resets = g_atomic_int_get(&self->resets);
    guint done = 0;
    int stuck = 0;
    while (done < length) {
        long took = 0, queued, lead;
        if (g_atomic_int_get(&self->resets) != resets) break;          /* a seek or flush: what is left goes */
        g_mutex_lock(&self->lock);
        if (!self->stream && g_get_monotonic_time() >= self->retry_us) stream_open(self);
        if (!self->stream) {
            /* nowhere to play it: at its own pace, as if it played */
            gint64 now = g_get_monotonic_time(), dur;
            g_mutex_unlock(&self->lock);
            if (!self->rate || !self->bpf) return (gint)length;
            if (self->silent_until < now) self->silent_until = now;
            dur = (gint64)(length - done) / self->bpf * G_USEC_PER_SEC / self->rate;
            self->silent_until += dur;
            while (g_get_monotonic_time() + LEAD_MS * 1000 < self->silent_until &&
                   g_atomic_int_get(&self->resets) == resets)
                nap();
            return (gint)length;
        }
        lead = (long)self->rate * LEAD_MS / 1000;
        queued = icda_audio_stream_queued(self->stream);
        if (queued >= 0 && queued < lead && !self->paused) {
            long room = (lead - queued) * self->bpf, want = (long)(length - done);
            if (want > room) want = room - room % self->bpf;
            if (want > 0) took = icda_audio_stream_write(self->stream, (const int16_t *)(pcm + done), (uint64_t)want);
        }
        if (took < 0 || queued < 0) {                                   /* the stream went away: open another */
            stream_close(self);
            self->retry_us = 0;
            took = 0;
        }
        g_mutex_unlock(&self->lock);
        if (took > 0) {
            done += (guint)took;
            stuck = 0;
        } else {
            if (!self->paused && ++stuck > 1000) break;                 /* 5 s with no room: the mixer is gone */
            nap();
        }
    }
    return (gint)length;
}

static guint sink_delay(GstAudioSink *s) {
    GstIcdaSink *self = GST_ICDA_SINK(s);
    long q = 0;
    g_mutex_lock(&self->lock);
    if (self->stream) {
        q = icda_audio_stream_queued(self->stream);
    } else if (self->rate) {
        gint64 left = self->silent_until - g_get_monotonic_time();
        q = left > 0 ? (long)(left * self->rate / G_USEC_PER_SEC) : 0;
    }
    g_mutex_unlock(&self->lock);
    return q > 0 ? (guint)q : 0;
}

/* a seek or flush: what is queued goes */
static void sink_reset(GstAudioSink *s) {
    GstIcdaSink *self = GST_ICDA_SINK(s);
    g_atomic_int_inc(&self->resets);
    g_mutex_lock(&self->lock);
    self->silent_until = 0;
    if (self->stream && icda_audio_stream_queued(self->stream) > 0) {
        stream_close(self);                       /* the mixer has no flush: a fresh stream */
        stream_open(self);
    }
    g_mutex_unlock(&self->lock);
}

static void sink_pause(GstAudioSink *s) {
    GstIcdaSink *self = GST_ICDA_SINK(s);
    g_atomic_int_inc(&self->resets);              /* a write waiting for room returns */
    g_mutex_lock(&self->lock);
    self->paused = 1;
    if (self->stream) (void)icda_audio_stream_control(self->stream, 1, 256);
    g_mutex_unlock(&self->lock);
}

static void sink_resume(GstAudioSink *s) {
    GstIcdaSink *self = GST_ICDA_SINK(s);
    g_mutex_lock(&self->lock);
    self->paused = 0;
    if (self->stream) (void)icda_audio_stream_control(self->stream, 0, 256);
    g_mutex_unlock(&self->lock);
}

static void sink_stop(GstAudioSink *s) {
    sink_reset(s);
}

static gboolean sink_unprepare(GstAudioSink *s) {
    GstIcdaSink *self = GST_ICDA_SINK(s);
    g_atomic_int_inc(&self->resets);
    g_mutex_lock(&self->lock);
    stream_close(self);
    self->rate = 0;
    g_mutex_unlock(&self->lock);
    return TRUE;
}

static gboolean sink_close(GstAudioSink *s) {
    (void)s;
    return TRUE;
}

static void sink_finalize(GObject *o) {
    GstIcdaSink *self = GST_ICDA_SINK(o);
    stream_close(self);
    g_mutex_clear(&self->lock);
    G_OBJECT_CLASS(gst_icda_sink_parent_class)->finalize(o);
}

static void gst_icda_sink_class_init(GstIcdaSinkClass *klass) {
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    GstElementClass *ec = GST_ELEMENT_CLASS(klass);
    GstAudioSinkClass *ac = GST_AUDIO_SINK_CLASS(klass);
    oc->finalize = sink_finalize;
    gst_element_class_set_static_metadata(ec, "ICDA audio sink", "Sink/Audio", "Plays sound through ICDA's mixer", "ICDA");
    gst_element_class_add_static_pad_template(ec, &sink_template);
    ac->open = sink_open;
    ac->prepare = sink_prepare;
    ac->write = sink_write;
    ac->delay = sink_delay;
    ac->reset = sink_reset;
    ac->pause = sink_pause;
    ac->resume = sink_resume;
    ac->stop = sink_stop;
    ac->unprepare = sink_unprepare;
    ac->close = sink_close;
}

static void gst_icda_sink_init(GstIcdaSink *self) {
    g_mutex_init(&self->lock);
    self->stream = 0;
}

static gboolean plugin_init(GstPlugin *plugin) {
    return gst_element_register(plugin, "icdasink", GST_RANK_PRIMARY + 10, GST_TYPE_ICDA_SINK);
}

#define PACKAGE "icda"
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, icda, "ICDA sound output", plugin_init, "1.0", "LGPL", "ICDA",
                  "https://github.com/elabbas0/ICDA")

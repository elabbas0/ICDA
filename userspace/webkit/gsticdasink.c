/* icdasink: a GStreamer audio sink that plays through ICDA's mixer.
 *
 * WebKit's media (video and audio elements, Web Audio) decodes with
 * GStreamer; this sink hands the sound to ICDA's kernel mixer (a stream
 * per sink, 16-bit interleaved PCM at the source's own rate) through the
 * kernel's gateway for Linux programs.  Its rank puts it ahead of the other
 * audio sinks, so autoaudiosink (which WebKit uses) picks it.
 *
 * Built into the Linux root as /usr/lib/gstreamer-1.0/libgsticda.so. */
#include <gst/gst.h>
#include <gst/audio/gstaudiosink.h>
#include <time.h>
#include "icda_sys.h"

#define GST_TYPE_ICDA_SINK (gst_icda_sink_get_type())
G_DECLARE_FINAL_TYPE(GstIcdaSink, gst_icda_sink, GST, ICDA_SINK, GstAudioSink)

struct _GstIcdaSink {
    GstAudioSink parent;
    long         stream;
    int          rate, channels, bpf;
};

G_DEFINE_TYPE(GstIcdaSink, gst_icda_sink, GST_TYPE_AUDIO_SINK)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, format = (string) S16LE, layout = (string) interleaved, "
                    "rate = (int) [ 8000, 192000 ], channels = (int) [ 1, 2 ]"));

static void nap(void) {
    struct timespec t = { 0, 4 * 1000 * 1000 };
    nanosleep(&t, NULL);
}

static gboolean sink_open(GstAudioSink *s) {
    (void)s;
    return TRUE;
}

static gboolean sink_prepare(GstAudioSink *s, GstAudioRingBufferSpec *spec) {
    GstIcdaSink *self = GST_ICDA_SINK(s);
    self->rate = GST_AUDIO_INFO_RATE(&spec->info);
    self->channels = GST_AUDIO_INFO_CHANNELS(&spec->info);
    self->bpf = GST_AUDIO_INFO_BPF(&spec->info);
    self->stream = icda_audio_stream_open((uint32_t)self->rate, (uint32_t)self->channels);
    if (self->stream <= 0) {
        GST_ELEMENT_ERROR(s, RESOURCE, OPEN_WRITE, ("ICDA has no sound output"), (NULL));
        self->stream = 0;
        return FALSE;
    }
    (void)icda_audio_stream_control(self->stream, 0, 256);
    return TRUE;
}

/* takes the whole buffer, waiting while the mixer's queue is full */
static gint sink_write(GstAudioSink *s, gpointer data, guint length) {
    GstIcdaSink *self = GST_ICDA_SINK(s);
    const int16_t *pcm = (const int16_t *)data;
    guint done = 0;
    int idle = 0;
    if (!self->stream) return (gint)length;
    while (done < length) {
        long took = icda_audio_stream_write(self->stream, (const int16_t *)((const uint8_t *)pcm + done), length - done);
        if (took < 0) return -1;
        if (took == 0) {
            if (++idle > 2000) break;                         /* the device went away: drop it */
            nap();
            continue;
        }
        idle = 0;
        done += (guint)took;
    }
    return (gint)length;
}

static guint sink_delay(GstAudioSink *s) {
    GstIcdaSink *self = GST_ICDA_SINK(s);
    long q = self->stream ? icda_audio_stream_queued(self->stream) : 0;
    return q > 0 ? (guint)q : 0;
}

/* a seek or flush: what is queued goes */
static void sink_reset(GstAudioSink *s) {
    GstIcdaSink *self = GST_ICDA_SINK(s);
    if (!self->stream) return;
    icda_audio_stream_close(self->stream);
    self->stream = icda_audio_stream_open((uint32_t)self->rate, (uint32_t)self->channels);
    if (self->stream < 0) self->stream = 0;
}

static gboolean sink_unprepare(GstAudioSink *s) {
    GstIcdaSink *self = GST_ICDA_SINK(s);
    if (self->stream) icda_audio_stream_close(self->stream);
    self->stream = 0;
    return TRUE;
}

static gboolean sink_close(GstAudioSink *s) {
    (void)s;
    return TRUE;
}

static void gst_icda_sink_class_init(GstIcdaSinkClass *klass) {
    GstElementClass *ec = GST_ELEMENT_CLASS(klass);
    GstAudioSinkClass *ac = GST_AUDIO_SINK_CLASS(klass);
    gst_element_class_set_static_metadata(ec, "ICDA audio sink", "Sink/Audio", "Plays sound through ICDA's mixer", "ICDA");
    gst_element_class_add_static_pad_template(ec, &sink_template);
    ac->open = sink_open;
    ac->prepare = sink_prepare;
    ac->write = sink_write;
    ac->delay = sink_delay;
    ac->reset = sink_reset;
    ac->unprepare = sink_unprepare;
    ac->close = sink_close;
}

static void gst_icda_sink_init(GstIcdaSink *self) {
    self->stream = 0;
}

static gboolean plugin_init(GstPlugin *plugin) {
    return gst_element_register(plugin, "icdasink", GST_RANK_PRIMARY + 10, GST_TYPE_ICDA_SINK);
}

#define PACKAGE "icda"
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, icda, "ICDA sound output", plugin_init, "1.0", "LGPL", "ICDA",
                  "https://github.com/elabbas0/ICDA")

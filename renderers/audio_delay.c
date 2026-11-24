/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_delay.h"
#include <string.h>

typedef struct {
    GMutex mutex;
    guint8 *samples;
    gsize size, position;
} audio_delay_t;

static void reset_locked(audio_delay_t *delay) {
    memset(delay->samples, 0, delay->size);
    delay->position = 0;
}

static void destroy_delay(gpointer data) {
    audio_delay_t *delay = data;
    g_mutex_clear(&delay->mutex);
    g_free(delay->samples);
    g_free(delay);
}

static GstPadProbeReturn delay_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    (void) pad;
    audio_delay_t *delay = data;
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        GstEventType type = GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info));
        if (type == GST_EVENT_STREAM_START || type == GST_EVENT_FLUSH_STOP || type == GST_EVENT_SEGMENT) {
            g_mutex_lock(&delay->mutex);
            reset_locked(delay);
            g_mutex_unlock(&delay->mutex);
        }
        return GST_PAD_PROBE_OK;
    }
    GstBuffer *buffer = gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info));
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    if (!buffer) return GST_PAD_PROBE_DROP;
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READWRITE)) return GST_PAD_PROBE_DROP;
    if (map.size % 4) {
        gst_buffer_unmap(buffer, &map);
        return GST_PAD_PROBE_DROP;
    }
    g_mutex_lock(&delay->mutex);
    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)) reset_locked(delay);
    /* Delay sample content, not delivery timestamps. Initial silence occupies
     * the delay interval, then output follows at the incoming sample cadence.
     * No sleeps, growing queues or changes to the sink synchronization policy. */
    for (gsize i = 0; i < map.size; ++i) {
        guint8 input = map.data[i];
        map.data[i] = delay->samples[delay->position];
        delay->samples[delay->position] = input;
        if (++delay->position == delay->size) delay->position = 0;
    }
    g_mutex_unlock(&delay->mutex);
    /* A silent input buffer may now contain previously delayed sound. */
    GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_GAP);
    gst_buffer_unmap(buffer, &map);
    return GST_PAD_PROBE_OK;
}

void audio_delay_attach(GstElement *pipeline, unsigned milliseconds) {
    if (!milliseconds) return;
    GstElement *element = gst_bin_get_by_name(GST_BIN(pipeline), "audio_delay");
    g_return_if_fail(element != NULL);
    GstPad *pad = gst_element_get_static_pad(element, "src");
    audio_delay_t *delay = g_new0(audio_delay_t, 1);
    g_mutex_init(&delay->mutex);
    /* Round to the nearest complete stereo sample (four bytes). */
    delay->size = ((44100ULL * milliseconds + 500) / 1000) * 4;
    delay->samples = g_malloc0(delay->size);
    g_object_set_data(G_OBJECT(pad), "uxplay-audio-delay", delay);
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
                     GST_PAD_PROBE_TYPE_EVENT_FLUSH, delay_buffer, delay, destroy_delay);
    gst_object_unref(pad);
    gst_object_unref(element);
}

void audio_delay_reset(GstElement *pipeline) {
    if (!pipeline) return;
    GstElement *element = gst_bin_get_by_name(GST_BIN(pipeline), "audio_delay");
    if (!element) return;
    GstPad *pad = gst_element_get_static_pad(element, "src");
    audio_delay_t *delay = g_object_get_data(G_OBJECT(pad), "uxplay-audio-delay");
    if (delay) {
        g_mutex_lock(&delay->mutex);
        reset_locked(delay);
        g_mutex_unlock(&delay->mutex);
    }
    gst_object_unref(pad);
    gst_object_unref(element);
}

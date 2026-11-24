/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../renderers/audio_delay.h"
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

static guint8 pattern(gsize frame, unsigned byte) {
    return (guint8)(1 + (frame * 7 + byte * 31) % 254);
}

static void exercise(unsigned ms) {
    GError *error = NULL;
    gchar *description = g_strdup_printf(
        "appsrc name=source is-live=true format=time caps=audio/x-raw,format=S16LE,rate=44100,channels=2,layout=interleaved ! %s"
        "tee name=split ! queue ! appsink name=local sync=false async=false "
        "split. ! queue ! appsink name=remote sync=false async=false",
        ms ? AUDIO_DELAY_PIPELINE : "");
    GstElement *pipe = gst_parse_launch(description, &error);
    g_assert_no_error(error);
    g_free(description);
    audio_delay_attach(pipe, ms);
    GstElement *source = gst_bin_get_by_name(GST_BIN(pipe), "source");
    GstElement *local = gst_bin_get_by_name(GST_BIN(pipe), "local");
    GstElement *remote = gst_bin_get_by_name(GST_BIN(pipe), "remote");
    g_assert(gst_element_set_state(pipe, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    gsize frames = (44100ULL * ms + 500) / 1000;
    /* Uneven packet lengths exercise ring wrap and sub-packet delays. Verify
     * both outputs, timestamp preservation, reset and pipeline restart. */
    for (int pass = 0; pass < 3; ++pass) {
        if (pass == 1) audio_delay_reset(pipe);
        if (pass == 2) {
            gst_element_set_state(pipe, GST_STATE_NULL);
            gst_element_set_state(pipe, GST_STATE_PLAYING);
        }
        gsize sent = 0;
        while (sent < frames + 2205) {
            gsize count = sent % 2 ? 352 : 441;
            GstBuffer *input = gst_buffer_new_allocate(NULL, count * 4, NULL);
            GstMapInfo map;
            g_assert(gst_buffer_map(input, &map, GST_MAP_WRITE));
            for (gsize i = 0; i < count * 4; ++i) map.data[i] = pattern(sent + i / 4, i % 4);
            gst_buffer_unmap(input, &map);
            GST_BUFFER_PTS(input) = gst_util_uint64_scale(sent, GST_SECOND, 44100);
            GstClockTime expected_pts = GST_BUFFER_PTS(input);
            GST_BUFFER_DURATION(input) = gst_util_uint64_scale(count, GST_SECOND, 44100);
            g_assert_cmpint(gst_app_src_push_buffer(GST_APP_SRC(source), input), ==, GST_FLOW_OK);
            GstElement *sinks[] = {local, remote};
            for (unsigned j = 0; j < 2; ++j) {
                GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sinks[j]), GST_SECOND);
                g_assert_nonnull(sample);
                GstBuffer *out = gst_sample_get_buffer(sample);
                g_assert_cmpuint(GST_BUFFER_PTS(out), ==, expected_pts);
                g_assert(gst_buffer_map(out, &map, GST_MAP_READ));
                g_assert_cmpuint(map.size, ==, count * 4);
                for (gsize i = 0; i < map.size; ++i) {
                    gsize at = sent + i / 4;
                    guint8 expected = at < frames ? 0 : pattern(at - frames, i % 4);
                    g_assert_cmpuint(map.data[i], ==, expected);
                }
                gst_buffer_unmap(out, &map);
                gst_sample_unref(sample);
            }
            sent += count;
        }
    }
    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(source); gst_object_unref(local); gst_object_unref(remote); gst_object_unref(pipe);
    g_print("PASS %u ms: both output branches, sample accuracy, wrap, flush and restart\n", ms);
}
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    unsigned delays[] = {0, 1, 150, 1000, 5000};
    for (unsigned i = 0; i < G_N_ELEMENTS(delays); ++i) exercise(delays[i]);
    return 0;
}

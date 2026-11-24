// SPDX-License-Identifier: GPL-3.0-or-later
// Explicit synthetic source for provider protocol tests; never advertises AirPlay.
#include "controlfloor.h"
#include <cstdlib>
#include <cstdio>
#include <string>
#ifndef _WIN32
#include <glib-unix.h>
#include <signal.h>
static gboolean test_interrupt(gpointer loop) {
    g_main_loop_quit(static_cast<GMainLoop *>(loop)); return G_SOURCE_CONTINUE;
}
#endif
extern "C" int cf_run_test_source() {
    GError *error = NULL;
    const char *test_pipeline = getenv("CF_TEST_H264")
        ? "videotestsrc is-live=true pattern=ball ! video/x-raw,format=I420,width=608,height=1080,framerate=30/1 ! x264enc tune=zerolatency ! h264parse ! decodebin ! videoconvert ! appsink name=cf_capture sync=false max-buffers=1 drop=true"
        : "videotestsrc is-live=true pattern=ball ! video/x-raw,width=720,height=1280,framerate=30/1 ! appsink name=cf_capture sync=false max-buffers=1 drop=true";
    if (getenv("CF_TEST_GL")) test_pipeline = "videotestsrc is-live=true pattern=ball ! video/x-raw,width=720,height=1280,framerate=30/1 ! glupload ! glcolorconvert ! video/x-raw(memory:GLMemory),format=NV12 ! appsink name=cf_capture sync=false max-buffers=1 drop=true";
    if (getenv("CF_TEST_STATIC")) test_pipeline = "videotestsrc is-live=true num-buffers=1 ! video/x-raw,width=720,height=1280,framerate=30/1 ! appsink name=cf_capture sync=false max-buffers=1 drop=true";
    // Exercise the same shared decoded-frame tee and bounded capture queue
    // as the desktop renderer, using a headless local sink for tests.
    std::string test_launch(test_pipeline);
    size_t capture_pos = test_launch.find(" ! appsink name=cf_capture");
    test_launch.insert(capture_pos, " ! tee name=cf_tee ! queue ! fakesink sync=false async=false cf_tee. ! queue leaky=downstream max-size-buffers=1 max-size-bytes=0 max-size-time=0");
    if (getenv("CF_TEST_AUDIO")) {
        test_launch += " audiotestsrc is-live=true samplesperbuffer=441 volume=0.5 wave=";
        test_launch += std::string(getenv("CF_TEST_AUDIO"))=="silence" ? "silence" : "sine";
        test_launch += " ! audio/x-raw,format=S16LE,rate=44100,channels=2,layout=interleaved ! "
            "tee name=test_audio ! queue ! fakesink sync=false async=false "
            "test_audio. ! queue leaky=downstream max-size-buffers=16 max-size-bytes=0 max-size-time=200000000 ! "
            "appsink name=cf_audio_capture sync=false async=false max-buffers=8 drop=true";
    }
    GstElement *test = gst_parse_launch(test_launch.c_str(), &error);
    if (error) { fprintf(stderr, "%s\n", error->message); g_error_free(error); cf_stop(); return 1; }
    cf_attach(test);
    cf_attach_audio(test);
    cf_client("synthetic-airplay-id", "Synthetic phone", "Synthetic video");
    gst_element_set_state(test, GST_STATE_PLAYING);
    GMainLoop *test_loop = g_main_loop_new(NULL, FALSE);
#ifndef _WIN32
    guint test_int = g_unix_signal_add(SIGINT, (GSourceFunc) test_interrupt, test_loop);
    guint test_term = g_unix_signal_add(SIGTERM, (GSourceFunc) test_interrupt, test_loop);
#endif
    g_main_loop_run(test_loop);
#ifndef _WIN32
    g_source_remove(test_int); g_source_remove(test_term);
#endif
    gst_element_set_state(test, GST_STATE_NULL); gst_object_unref(test);
    cf_stop(); g_main_loop_unref(test_loop); return 0;
}

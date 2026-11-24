/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gst/gst.h>
#ifdef __cplusplus
extern "C" {
#endif
int cf_option(int argc, char **argv, int *index);
void cf_help(void);
/* Returns 1 for registration-only completion, -1 on error, 0 to continue. */
int cf_prepare(void);
int cf_start(void);
void cf_connection_open(void);
int cf_enabled(void);
int cf_test_source(void);
int cf_run_test_source(unsigned audio_delay_ms);
void cf_stop(void);
void cf_client(const char *identifier, const char *name, const char *model);
void cf_disconnected(void);
/* Attach the bounded decoded-frame tap to a renderer or synthetic pipeline. */
void cf_attach(GstElement *pipeline);
/* Bounded decoded PCM tap; start/stopAudio only affect remote forwarding. */
void cf_attach_audio(GstElement *pipeline);
void cf_audio_flush(void);
#ifdef __cplusplus
}
#endif

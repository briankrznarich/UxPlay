/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UXPLAY_AUDIO_DELAY_H
#define UXPLAY_AUDIO_DELAY_H
#include <gst/gst.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Insert after decoding and before any output tee, only for a nonzero delay. */
#define AUDIO_DELAY_PIPELINE "audioconvert ! audioresample ! audio/x-raw,format=S16LE,rate=44100,channels=2,layout=interleaved ! identity name=audio_delay ! "
void audio_delay_attach(GstElement *pipeline, unsigned milliseconds);
void audio_delay_reset(GstElement *pipeline);
#ifdef __cplusplus
}
#endif
#endif

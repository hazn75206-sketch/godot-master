/**************************************************************************/
/*  mp4_android_jni.h                                                     */
/*                                                                        */
/*  JNI bridge to the Android MediaCodec based MP4 decoder.               */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/templates/vector.h"

struct MP4VideoDecoderHandle;

MP4VideoDecoderHandle *mp4_decoder_create(const String &p_path, String *r_error);
void mp4_decoder_destroy(MP4VideoDecoderHandle *p_handle);

void mp4_decoder_play(MP4VideoDecoderHandle *p_handle);
void mp4_decoder_stop(MP4VideoDecoderHandle *p_handle);
void mp4_decoder_set_paused(MP4VideoDecoderHandle *p_handle, bool p_paused);
bool mp4_decoder_is_playing(MP4VideoDecoderHandle *p_handle);
void mp4_decoder_seek(MP4VideoDecoderHandle *p_handle, double p_seconds);

double mp4_decoder_get_duration(MP4VideoDecoderHandle *p_handle);
double mp4_decoder_get_position(MP4VideoDecoderHandle *p_handle);
int mp4_decoder_get_width(MP4VideoDecoderHandle *p_handle);
int mp4_decoder_get_height(MP4VideoDecoderHandle *p_handle);
int mp4_decoder_get_channels(MP4VideoDecoderHandle *p_handle);
int mp4_decoder_get_sample_rate(MP4VideoDecoderHandle *p_handle);

bool mp4_decoder_update(MP4VideoDecoderHandle *p_handle, double p_delta);
const uint8_t *mp4_decoder_get_frame(MP4VideoDecoderHandle *p_handle, int *r_width, int *r_height, int *r_rotation, int *r_serial);
int mp4_decoder_mix_audio(MP4VideoDecoderHandle *p_handle, float *p_out, int p_frames);

bool mp4_get_thumbnail(const String &p_path, int p_max_width, int p_max_height, Vector<uint8_t> &r_rgba, int &r_width, int &r_height);

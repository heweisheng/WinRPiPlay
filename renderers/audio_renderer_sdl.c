/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include "audio_renderer.h"

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "fdk-aac/libAACdec/include/aacdecoder_lib.h"
#include <SDL.h>

#ifndef WIN32
#include <unistd.h>
#endif
#define MAXCACHE 10

typedef struct audio_frame_s {
	uint8_t* buffer;
	uint8_t* reader;
	size_t len;
}audio_frame_t;

typedef struct audio_renderer_sdl_s {
    audio_renderer_t base;
	audio_frame_t frames[MAXCACHE];
	size_t framenum;
	size_t readnum;
	SDL_mutex* mutex;
	HANDLE_AACDECODER audio_decoder;
	SDL_AudioDeviceID deviceid;
} audio_renderer_sdl_t;

static const audio_renderer_funcs_t audio_renderer_sdl_funcs;

static void audio_renderer_sdl_destroy_decoder(audio_renderer_sdl_t *renderer) {
	aacDecoder_Close(renderer->audio_decoder);
}

static int audio_renderer_sdl_init_decoder(audio_renderer_sdl_t *renderer) {
	int ret = 0;
	renderer->audio_decoder = aacDecoder_Open(TT_MP4_RAW, 1);
	if (renderer->audio_decoder == NULL) {
		logger_log(renderer->base.logger, LOGGER_ERR, "aacDecoder open faild!");
		return -1;
	}
	/* ASC config binary data */
	UCHAR eld_conf[] = { 0xF8, 0xE8, 0x50, 0x00 };
	UCHAR *conf[] = { eld_conf };
	static UINT conf_len = sizeof(eld_conf);
	ret = aacDecoder_ConfigRaw(renderer->audio_decoder, conf, &conf_len);
	if (ret != AAC_DEC_OK) {
		logger_log(renderer->base.logger, LOGGER_ERR, "Unable to set configRaw");
		return -2;
	}
	CStreamInfo *aac_stream_info = aacDecoder_GetStreamInfo(renderer->audio_decoder);
	if (aac_stream_info == NULL) {
		logger_log(renderer->base.logger, LOGGER_ERR, "aacDecoder_GetStreamInfo failed!");
		return -3;
	}

	logger_log(renderer->base.logger, LOGGER_DEBUG, "> stream info: channel = %d\tsample_rate = %d\tframe_size = %d\taot = %d\tbitrate = %d", \
		aac_stream_info->channelConfig, aac_stream_info->aacSampleRate,
		aac_stream_info->aacSamplesPerFrame, aac_stream_info->aot, aac_stream_info->bitRate);
	return 1;
}

void SDLCALL audio_renderer_sdl_callback(void * userdata, Uint8 * stream, int len)
{
	audio_renderer_sdl_t *renderer=(audio_renderer_sdl_t*)userdata;
	SDL_LockMutex(renderer->mutex);
	int write = 0;
	while (write < len)
	{
		if (renderer->readnum < renderer->framenum)
		{
			audio_frame_t* frame = &renderer->frames[renderer->readnum%MAXCACHE];
			int canread = frame->len - (frame->reader - frame->buffer);
			if (canread > len - write)
			{
				memcpy(stream+write, frame->reader, len - write);
				frame->reader += len - write;
				write = len;
			}
			else {
				memcpy(stream+write, frame->reader, canread);
				free(frame->buffer);
				renderer->readnum++;
				write += canread;
			}
		}
		else {
			memset(stream + write, 0, len - write);
			write = len;
		}
	}
	SDL_UnlockMutex(renderer->mutex);
}

audio_renderer_t *audio_renderer_sdl_init(logger_t *logger, video_renderer_t *video_renderer, audio_renderer_config_t const *config) {
    audio_renderer_sdl_t *renderer;
    renderer = calloc(1, sizeof(audio_renderer_sdl_t));
    if (!renderer) {
        return NULL;
    }



	memset(renderer, 0, sizeof(audio_renderer_sdl_t));
    renderer->base.logger = logger;
    renderer->base.funcs = &audio_renderer_sdl_funcs;
    renderer->base.type = AUDIO_RENDERER_SDL;
	if (audio_renderer_sdl_init_decoder(renderer) != 1) {
		free(renderer);
		renderer = NULL;
	}

	renderer->mutex = SDL_CreateMutex();
	SDL_AudioSpec wantspec;
	SDL_AudioSpec dstspec;
	wantspec.callback = audio_renderer_sdl_callback;
	wantspec.channels = 2;
	wantspec.format = AUDIO_S16SYS;
	wantspec.freq = 44100;
	wantspec.samples = 480;
	wantspec.userdata = renderer;
	wantspec.silence = 0;
	SDL_Init(SDL_INIT_AUDIO);
	renderer->deviceid = SDL_OpenAudioDevice(NULL, 0, &wantspec, &dstspec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
	SDL_PauseAudioDevice(renderer->deviceid, 0);
	return &renderer->base;
}

static void audio_renderer_sdl_start(audio_renderer_t *renderer) {
}

static void audio_renderer_sdl_render_buffer(audio_renderer_t *renderer, raop_ntp_t *ntp, unsigned char *data, int data_len, uint64_t pts) {
	audio_renderer_sdl_t* r = (audio_renderer_sdl_t*)renderer;
	AAC_DECODER_ERROR error = 0;

	UCHAR *p_buffer[1] = { data };
	UINT buffer_size = data_len;
	UINT bytes_valid = data_len;
	error = aacDecoder_Fill(r->audio_decoder, p_buffer, &buffer_size, &bytes_valid);
	if (error != AAC_DEC_OK) {
		logger_log(renderer->logger, LOGGER_ERR, "aacDecoder_Fill error : %x", error);
	}

	INT time_data_size = 4 * 480;
	INT_PCM *p_time_data = malloc(time_data_size); // The buffer for the decoded AAC frames
	error = aacDecoder_DecodeFrame(r->audio_decoder, p_time_data, time_data_size, 0);
	if (error != AAC_DEC_OK) {
		logger_log(renderer->logger, LOGGER_ERR, "aacDecoder_DecodeFrame error : 0x%x", error);
	}
	if (r->framenum - r->readnum < MAXCACHE)
	{
		SDL_LockMutex(r->mutex);
		r->frames[r->framenum%MAXCACHE].buffer = r->frames[r->framenum%MAXCACHE].reader = (uint8_t*)p_time_data;
		r->frames[r->framenum%MAXCACHE].len = time_data_size;
		r->framenum++;
		SDL_UnlockMutex(r->mutex);

	}
	else {
		free(p_time_data);
	}
}

static void audio_renderer_sdl_set_volume(audio_renderer_t *renderer, float volume) {
}

static void audio_renderer_sdl_flush(audio_renderer_t *renderer) {
}

static void audio_renderer_sdl_destroy(audio_renderer_t *renderer) {
	audio_renderer_sdl_t *r=(audio_renderer_sdl_t*)renderer;
    if (renderer) {
		SDL_DestroyMutex(r->mutex);
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		SDL_CloseAudioDevice(r->deviceid);
		audio_renderer_sdl_destroy_decoder(r);
		for (; r->framenum > r->readnum; r->readnum++)
		{
			free(r->frames[r->readnum%MAXCACHE].buffer);
		}
		free(renderer);
    }
}

static const audio_renderer_funcs_t audio_renderer_sdl_funcs = {
    .start = audio_renderer_sdl_start,
    .render_buffer = audio_renderer_sdl_render_buffer,
    .set_volume = audio_renderer_sdl_set_volume,
    .flush = audio_renderer_sdl_flush,
    .destroy = audio_renderer_sdl_destroy,
};

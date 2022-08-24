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
//#include "fdk-aac/libAACdec/include/aacdecoder_lib.h"
#include <libavcodec/avcodec.h>
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
	AVCodecContext* aacctx;
	SDL_AudioDeviceID deviceid;
} audio_renderer_sdl_t;

static const audio_renderer_funcs_t audio_renderer_sdl_funcs;

static void audio_renderer_sdl_destroy_decoder(audio_renderer_sdl_t *renderer) {
	avcodec_free_context(&renderer->aacctx);
}

static int audio_renderer_sdl_init_decoder(audio_renderer_sdl_t *renderer) {
	int ret = 0;
	const AVCodec* aacdecode = avcodec_find_decoder(AV_CODEC_ID_AAC);
	renderer->aacctx = avcodec_alloc_context3(aacdecode);
	if (!renderer->aacctx)
	{
		ret = AVERROR(ENOMEM);
	}
	else {
		/* ASC config binary data */
		uint8_t eld_conf[] = { 0xF8, 0xE8, 0x50, 0x00 };
		renderer->aacctx->extradata_size = sizeof(eld_conf);
		renderer->aacctx->extradata = av_mallocz(renderer->aacctx->extradata_size);
		memcpy(renderer->aacctx->extradata, eld_conf, renderer->aacctx->extradata_size);
		ret = avcodec_open2(renderer->aacctx, aacdecode, NULL);
	}
	return ret;
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
	if (audio_renderer_sdl_init_decoder(renderer) != 0) {
		free(renderer);
		renderer = NULL;
	}

	renderer->mutex = SDL_CreateMutex();
	SDL_AudioSpec wantspec;
	SDL_AudioSpec dstspec;
	wantspec.callback = audio_renderer_sdl_callback;
	wantspec.channels = 2;
	wantspec.format = AUDIO_F32SYS;
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

	audio_renderer_sdl_t *r = (audio_renderer_sdl_t*)renderer;
	AVFrame* pFrame = av_frame_alloc();
	AVPacket* packet = av_packet_alloc();
	packet->pts = pts;
	int i = 0;
	av_new_packet(packet, data_len);
	memcpy(packet->data, data, data_len);
	avcodec_send_packet(r->aacctx, packet);
	if (avcodec_receive_frame(r->aacctx, pFrame) == 0)
	{
		if (r->framenum - r->readnum < MAXCACHE)
		{
			size_t len = pFrame->nb_samples * sizeof(float) * pFrame->channels;
			float* data = malloc(len);
			int i ,c;
			//sdl不支持planar,要么用swr要么手动处理下
			for (i = 0; i < pFrame->nb_samples; i++)
			{
				for (c = 0; c < pFrame->channels; c++)
				{
					data[pFrame->channels * i + c] = ((float*)pFrame->extended_data[c])[i];
				}
			}

			SDL_LockMutex(r->mutex);
			r->frames[r->framenum%MAXCACHE].buffer = r->frames[r->framenum%MAXCACHE].reader = (uint8_t*)data;
			r->frames[r->framenum%MAXCACHE].len = len;
			r->framenum++;
			SDL_UnlockMutex(r->mutex);
		}
	}
	av_frame_free(&pFrame);
	av_packet_free(&packet);

	
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

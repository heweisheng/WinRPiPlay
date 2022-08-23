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

#include "video_renderer.h"

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>
#ifndef WIN32
#include <unistd.h>
#endif
typedef struct video_renderer_sdl_s {
    video_renderer_t base;
	AVCodecContext* h264ctx;
	SDL_Thread* renderthread;
	AVFrame* renderframe;
	bool endrender;
	SDL_mutex* mutex;
} video_renderer_sdl_t;

static const video_renderer_funcs_t video_renderer_sdl_funcs;

int video_render_sdl_init_decoder(video_renderer_sdl_t* render)
{
	int ret = 0;
	const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	render->h264ctx = avcodec_alloc_context3(codec);
	
	if (!render->h264ctx)
	{
		ret = AVERROR(ENOMEM);
	}
	else {
		ret = avcodec_open2(render->h264ctx, codec, NULL);
	}
	render->renderframe = av_frame_alloc();

	return ret;
}

int SDLCALL video_renderer_sdl_thread (void *data)
{
	video_renderer_sdl_t* renderer = data;
	AVFrame* renderframe=NULL;
	SDL_Window* sdlwnd=NULL;
	SDL_Renderer* sdlrender=NULL;
	SDL_Texture* sdltexture=NULL;
	SDL_Event event;
	SDL_Init(SDL_INIT_VIDEO);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
	sdlwnd= SDL_CreateWindow("RPiPlay", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 720, SDL_WINDOW_RESIZABLE);
	sdlrender= SDL_CreateRenderer(sdlwnd, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!sdlrender)
	{
		sdlrender= SDL_CreateRenderer(sdlwnd, -1, 0);
	}
	int width = 0;
	int height = 0;
	int sdlwidth = 1280;
	int sdlheight = 720;
	int64_t lasttime = 0;
	bool flush = false;
	while (!renderer->endrender)
	{
		SDL_LockMutex(renderer->mutex);
		renderframe = av_frame_clone(renderer->renderframe);
		av_frame_unref(renderer->renderframe);
		SDL_UnlockMutex(renderer->mutex);

		SDL_PumpEvents();
		while (!SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT))
		{
			if (renderer->endrender)
				break;
			int64_t nowtime = av_gettime_relative();
			if ( nowtime - lasttime > AV_TIME_BASE/100)
			{
				flush = true;
				break;
			}
			SDL_PumpEvents();
		}
		switch (event.type)
		{
			case SDL_WINDOWEVENT:
			{
				switch (event.window.event)
				{
					case SDL_WINDOWEVENT_SIZE_CHANGED:
						sdlwidth = event.window.data1;
						sdlheight = event.window.data2;
					case SDL_WINDOWEVENT_EXPOSED:
						flush = true;
						break;
					default:
						break;
				}
				break;
			}
			default:
				break;
		}


		if (renderframe)
		{
			if (renderframe->width != width || renderframe->height != height)
			{
				width = renderframe->width;
				height = renderframe->height;
				if (sdltexture)
				{
					SDL_DestroyTexture(sdltexture);
				}
				sdltexture = SDL_CreateTexture(sdlrender, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height);
			}
			SDL_UpdateYUVTexture(sdltexture, NULL, renderframe->data[0], renderframe->linesize[0],
				renderframe->data[1], renderframe->linesize[1],
				renderframe->data[2], renderframe->linesize[2]
			);

			av_frame_free(&renderframe);
		}

		if (flush)
		{
			SDL_SetRenderDrawColor(sdlrender, 0, 0, 0, 255);
			SDL_RenderClear(sdlrender);
			if (sdltexture)
			{
				SDL_RenderCopy(sdlrender, sdltexture, NULL, NULL);
			}
			SDL_RenderPresent(sdlrender);
		}
	}
	SDL_DestroyTexture(sdltexture);
	SDL_DestroyRenderer(sdlrender);
	SDL_DestroyWindow(sdlwnd);
	av_frame_free(&renderframe);
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	return 0;
}

video_renderer_t *video_renderer_sdl_init(logger_t *logger, video_renderer_config_t const *config) {
    video_renderer_sdl_t *renderer;
    renderer = calloc(1, sizeof(video_renderer_sdl_t));
	memset(renderer, 0, sizeof(video_renderer_sdl_t));
    if (!renderer) {
        return NULL;
    }
    renderer->base.logger = logger;
    renderer->base.funcs = &video_renderer_sdl_funcs;
    renderer->base.type = VIDEO_RENDERER_SDL;
	renderer->mutex = SDL_CreateMutex();
	renderer->endrender = false;
	video_render_sdl_init_decoder(renderer);
	renderer->renderthread = SDL_CreateThread(video_renderer_sdl_thread, "sdl_renderthread", renderer);
    return &renderer->base;
}

static void video_renderer_sdl_start(video_renderer_t *renderer) {
	video_renderer_sdl_t *r = (video_renderer_sdl_t*)renderer;

}

static void video_renderer_sdl_render_buffer(video_renderer_t *renderer, raop_ntp_t *ntp, unsigned char *data, int data_len, uint64_t pts, int type) {
	video_renderer_sdl_t *r = (video_renderer_sdl_t*)renderer;


		AVFrame* pFrame = av_frame_alloc();
		AVPacket* packet = av_packet_alloc();
		packet->pts = pts;
		av_new_packet(packet, data_len);
		memcpy(packet->data, data, data_len);
		avcodec_send_packet(r->h264ctx, packet);
		if (avcodec_receive_frame(r->h264ctx, pFrame)==0)
		{
			SDL_LockMutex(r->mutex);
			av_frame_unref(r->renderframe);
			av_frame_move_ref(r->renderframe, pFrame);
			SDL_UnlockMutex(r->mutex);
		}
		av_frame_free(&pFrame);
		av_packet_free(&packet);
	

}

static void video_renderer_sdl_flush(video_renderer_t *renderer) {
}

static void video_renderer_sdl_destroy(video_renderer_t *renderer) {
	video_renderer_sdl_t *r = (video_renderer_sdl_t *)renderer;
	int state;
	if (renderer) {

		avcodec_free_context(&r->h264ctx);
		r->endrender = true;
		SDL_WaitThread(r->renderthread,&state);
        free(renderer);
    }
}

static void video_renderer_sdl_update_background(video_renderer_t *renderer, int type) {

}

static const video_renderer_funcs_t video_renderer_sdl_funcs = {
    .start = video_renderer_sdl_start,
    .render_buffer = video_renderer_sdl_render_buffer,
    .flush = video_renderer_sdl_flush,
    .destroy = video_renderer_sdl_destroy,
    .update_background = video_renderer_sdl_update_background,
};

/*
obs-ndi
Copyright (C) 2016-2018 St�phane Lepin <steph  name of author

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; If not, see <https://www.gnu.org/licenses/>
*/

#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>

#include "obs-ndi.h"
#include "Config.h"
#include "PreviewSceneChangedWatcher.h"

struct preview_output {
	bool enabled;
	obs_source_t* current_source;
	obs_output_t* output;

	video_t* video_queue;
	gs_texrender_t* texrender;
	gs_stagesurf_t* stagesurface;
	uint8_t* video_data;
	uint32_t video_linesize;

	obs_video_info ovi;

	PreviewSceneChangedWatcher* dirtyHack;
};

static struct preview_output context = {0};

void on_preview_scene_changed(enum obs_frontend_event event, void* param);
void render_preview_source(void* param, uint32_t cx, uint32_t cy);

void preview_output_start(const char* output_name)
{
	if (context.enabled) return;
	obs_get_video_info(&context.ovi);

	uint32_t width = context.ovi.base_width;
	uint32_t height = context.ovi.base_height;

	obs_enter_graphics();
	context.texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	context.stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
	obs_leave_graphics();

	video_output_info vi = { 0 };
	vi.format = VIDEO_FORMAT_BGRA;
	vi.width = width;
	vi.height = height;
	vi.fps_den = context.ovi.fps_den;
	vi.fps_num = context.ovi.fps_num;
	vi.cache_size = 16;
	vi.colorspace = VIDEO_CS_DEFAULT;
	vi.range = VIDEO_RANGE_DEFAULT;
	vi.name = output_name;

	video_output_open(&context.video_queue, &vi);

	obs_frontend_add_event_callback(on_preview_scene_changed, &context);
	context.dirtyHack = new PreviewSceneChangedWatcher(on_preview_scene_changed, &context);
	if (obs_frontend_preview_program_mode_active()) {
		context.current_source = obs_frontend_get_current_preview_scene();
	} else {
		context.current_source = obs_frontend_get_current_scene();
	}
	obs_add_main_render_callback(render_preview_source, &context);

	obs_output_release(context.output);

	obs_data_t* output_settings = obs_data_create();
	obs_data_set_string(output_settings, "ndi_name", output_name);
	obs_data_set_bool(output_settings, "ndi_async_sending", false);
	obs_data_set_int(output_settings, "ndi_output_flags", OBS_OUTPUT_VIDEO);
	obs_data_set_bool(output_settings, "ndi_is_bgra", true);

	context.output = obs_output_create("ndi_output", "main_preview_output", output_settings, nullptr);
	obs_output_set_media(context.output, context.video_queue, nullptr);
	obs_data_release(output_settings);

	obs_output_start(context.output);
	context.enabled = true;
}

void preview_output_stop()
{	
	if (!context.enabled) return;

	obs_output_stop(context.output);
	video_output_stop(context.video_queue);

	obs_remove_main_render_callback(render_preview_source, &context);
	obs_frontend_remove_event_callback(on_preview_scene_changed, &context);
	delete context.dirtyHack;

	obs_source_release(context.current_source);

	obs_enter_graphics();
	gs_stagesurface_destroy(context.stagesurface);
	gs_texrender_destroy(context.texrender);
	obs_leave_graphics();

	obs_output_release(context.output);
	video_output_close(context.video_queue);

	context.enabled = false;
}

bool preview_output_is_enabled()
{
	return context.enabled;
}

void on_preview_scene_changed(enum obs_frontend_event event, void* param)
{
	struct preview_output* ctx = (struct preview_output*)param;
	switch (event) {
		case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
		case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
			obs_source_release(ctx->current_source);
			ctx->current_source = obs_frontend_get_current_preview_scene();
			break;
		case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
			obs_source_release(ctx->current_source);
			ctx->current_source = obs_frontend_get_current_scene();
			break;
		case OBS_FRONTEND_EVENT_SCENE_CHANGED:
			if (!obs_frontend_preview_program_mode_active()) {
				obs_source_release(ctx->current_source);
				ctx->current_source = obs_frontend_get_current_scene();
			}
			break;
		default:
			break;
	}
}

void render_preview_source(void* param, uint32_t cx, uint32_t cy)
{
	struct preview_output* ctx = (struct preview_output*)param;

	if (!ctx->current_source) return;

	uint32_t width = obs_source_get_base_width(ctx->current_source);
	uint32_t height = obs_source_get_base_height(ctx->current_source);

	gs_texrender_reset(ctx->texrender);

	if (gs_texrender_begin(ctx->texrender, width, height)) {
		struct vec4 background;
		vec4_zero(&background);

		gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		obs_source_video_render(ctx->current_source);

		gs_blend_state_pop();
		gs_texrender_end(ctx->texrender);

		struct video_frame output_frame;
		if (video_output_lock_frame(ctx->video_queue,
			&output_frame, 1, os_gettime_ns()))
		{
			gs_stage_texture(ctx->stagesurface, gs_texrender_get_texture(ctx->texrender));

			if (gs_stagesurface_map(ctx->stagesurface, &ctx->video_data, &ctx->video_linesize)) {
				uint32_t linesize = output_frame.linesize[0];
				for (uint32_t i = 0; i < ctx->ovi.base_height; i++) {
					uint32_t dst_offset = linesize * i;
					uint32_t src_offset = ctx->video_linesize * i;
					memcpy(output_frame.data[0] + dst_offset,
						ctx->video_data + src_offset,
						linesize);
				}

				gs_stagesurface_unmap(ctx->stagesurface);
				ctx->video_data = nullptr;
			}

			video_output_unlock_frame(ctx->video_queue);
		}
	}
}
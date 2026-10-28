/**************************************************************************/
/*  tsr.cpp                                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tsr.h"

#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererRD;

TSREffect::TSREffect() {
	Vector<String> modes = { "", "\n#define MODE_SHARPEN\n" };
	shader.initialize(modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
	pipeline_sharpen = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 1));
	RD::SamplerState ss;
	ss.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	ss.min_filter = RD::SAMPLER_FILTER_LINEAR;
	ss.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	ss.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	linear_sampler = RD::get_singleton()->sampler_create(ss);
}

TSREffect::~TSREffect() {
	RD::get_singleton()->free_rid(linear_sampler);
	shader.version_free(shader_version);
}

void TSREffect::copy_opaque(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_color, Size2i p_internal_size, RD::DataFormat p_format) {
	static const StringName context = SNAME("TSR");
	static const StringName name = SNAME("opaque");
	if (!p_render_buffers->has_texture(context, name)) {
		p_render_buffers->create_texture(context, name, p_format, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, p_internal_size);
	}
	// The internal colour texture cannot be copied with texture_copy (no CAN_COPY_FROM), so go through the copy shader.
	CopyEffects::get_singleton()->copy_to_rect(p_color, p_render_buffers->get_texture(context, name), Rect2i(Point2i(), p_internal_size));
}

void TSREffect::process(Ref<RenderSceneBuffersRD> p_render_buffers, const Parameters &p_params) {
	static const StringName context = SNAME("TSR");
	static const StringName names[2] = { SNAME("history0"), SNAME("history1") };
	bool reset = p_params.reset;
	if (!p_render_buffers->has_texture(context, names[0])) {
		for (int i = 0; i < 2; i++) {
			p_render_buffers->create_texture(context, names[i], RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT, RD::TEXTURE_SAMPLES_1, p_params.target_size);
			RD::get_singleton()->texture_clear(p_render_buffers->get_texture(context, names[i]), Color(0, 0, 0, 0), 0, 1, 0, 1);
		}
		reset = true;
	}
	frame++;
	RID history_in = p_render_buffers->get_texture(context, names[frame & 1]);
	RID history_out = p_render_buffers->get_texture(context, names[(frame + 1) & 1]);

	RID shader_rid = shader.version_get_shader(shader_version, 0);
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear_sampler, p_params.color }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ linear_sampler, p_params.depth }));
	RD::Uniform u_velocity(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ p_params.velocity }));
	RD::Uniform u_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ linear_sampler, history_in }));
	RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 4, Vector<RID>({ p_params.output }));
	RD::Uniform u_history_out(RD::UNIFORM_TYPE_IMAGE, 5, Vector<RID>({ history_out }));
	static const StringName opaque_name = SNAME("opaque");
	bool has_opaque = p_render_buffers->has_texture(context, opaque_name);
	RD::Uniform u_opaque(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ linear_sampler, has_opaque ? p_render_buffers->get_texture(context, opaque_name) : p_params.color }));
	RID uniform_set = cache->get_cache(shader_rid, 0, u_color, u_depth, u_velocity, u_history, u_output, u_history_out, u_opaque);

	PushConstant pc;
	pc.internal_size[0] = p_params.internal_size.x;
	pc.internal_size[1] = p_params.internal_size.y;
	pc.target_size[0] = p_params.target_size.x;
	pc.target_size[1] = p_params.target_size.y;
	pc.jitter[0] = p_params.jitter.x;
	pc.jitter[1] = p_params.jitter.y;
	pc.reset = reset ? 1.0f : 0.0f;
	pc.sharpness = p_params.sharpness;
	RendererRD::MaterialStorage::store_camera(p_params.reprojection, pc.reprojection);
	pc.reactive = has_opaque ? p_params.reactive : 0.0f;

	RD::ComputeListID cl = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(cl, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(cl, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(cl, &pc, sizeof(pc));
	RD::get_singleton()->compute_list_dispatch(cl, Math::division_round_up(uint32_t(p_params.target_size.x), 8u), Math::division_round_up(uint32_t(p_params.target_size.y), 8u), 1);
	if (p_params.sharpness < 2.0f) {
		// RCAS on the accumulated frame: reads history_out, writes the final output.
		RD::get_singleton()->compute_list_add_barrier(cl);
		RID sharpen_shader = shader.version_get_shader(shader_version, 1);
		RD::Uniform u_accum(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear_sampler, history_out }));
		RD::Uniform u_unused(RD::UNIFORM_TYPE_IMAGE, 5, Vector<RID>({ history_in }));
		RID sharpen_set = cache->get_cache(sharpen_shader, 0, u_accum, u_depth, u_velocity, u_history, u_output, u_unused, u_opaque);
		RD::get_singleton()->compute_list_bind_compute_pipeline(cl, pipeline_sharpen);
		RD::get_singleton()->compute_list_bind_uniform_set(cl, sharpen_set, 0);
		RD::get_singleton()->compute_list_set_push_constant(cl, &pc, sizeof(pc));
		RD::get_singleton()->compute_list_dispatch(cl, Math::division_round_up(uint32_t(p_params.target_size.x), 8u), Math::division_round_up(uint32_t(p_params.target_size.y), 8u), 1);
	}
	RD::get_singleton()->compute_list_end();
}

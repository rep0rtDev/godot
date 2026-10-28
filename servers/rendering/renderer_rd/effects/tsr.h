/**************************************************************************/
/*  tsr.h                                                                 */
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

#pragma once

#include "core/math/projection.h"
#include "servers/rendering/renderer_rd/shaders/effects/tsr.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {

// Temporal super resolution: upscales the jittered internal-resolution frame to the target size with a history kept in
// the render buffers (context "TSR", textures "history0/1").
class TSREffect {
public:
	struct Parameters {
		RID color;
		RID depth;
		RID velocity;
		RID output;
		Size2i internal_size;
		Size2i target_size;
		Vector2 jitter; // Internal pixels.
		float sharpness = 0.0f;
		float reactive = 1.0f; // Weight of the transparency reactive mask (needs copy_opaque before the transparent pass).
		Projection reprojection; // Clip space of this frame to clip space of the previous one, for static geometry.
		bool reset = false; // Camera cut: drop the history instead of reprojecting it.
	};

	TSREffect();
	~TSREffect();

	void copy_opaque(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_color, Size2i p_internal_size, RD::DataFormat p_format);
	void process(Ref<RenderSceneBuffersRD> p_render_buffers, const Parameters &p_params);

private:
	struct PushConstant {
		float internal_size[2];
		float target_size[2];
		float jitter[2];
		float reset;
		float sharpness;
		float reprojection[16];
		float reactive;
		float pad[3];
	};

	TsrShaderRD shader;
	RID shader_version;
	RID pipeline;
	RID pipeline_sharpen;
	RID linear_sampler;
	uint64_t frame = 0;
};

} // namespace RendererRD

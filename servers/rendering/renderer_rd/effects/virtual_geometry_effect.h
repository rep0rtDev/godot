/**************************************************************************/
/*  virtual_geometry_effect.h                                             */
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

#include "servers/rendering/renderer_rd/shaders/effects/virtual_geometry_select.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

class VirtualGeometryEffect {
public:
	static constexpr uint32_t COMMAND_STRIDE = sizeof(uint32_t) * 5;
	static constexpr uint32_t GROUP_SIZE = 64;
	static constexpr uint32_t FLAG_ORTHOGONAL = 1;

	struct PushConstant {
		float modelview[12];
		float projection[16];
		uint32_t cluster_count;
		float lod_scale;
		uint32_t flags;
		uint32_t pad;
	};
	static_assert(sizeof(PushConstant) == 128);

private:
	VirtualGeometrySelectShaderRD shader;
	RID shader_version;
	RID pipeline;
	float pixel_error = 1.0f;

public:
	VirtualGeometryEffect();
	~VirtualGeometryEffect();

	RID get_shader() const;
	RID get_pipeline() const { return pipeline; }
	float get_pixel_error() const { return pixel_error; }

	// Command buffer with two draw ranges (camera and shadow); both start as the full-detail leaf clusters.
	RID create_command_buffer(const Vector<uint8_t> &p_cluster_data, uint32_t p_leaf_index_count) const;
	RID create_uniform_set(RID p_cluster_buffer, RID p_command_buffer) const;
};

} // namespace RendererRD

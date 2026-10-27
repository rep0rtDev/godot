/**************************************************************************/
/*  virtual_geometry_effect.cpp                                           */
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

#include "virtual_geometry_effect.h"

#include "core/config/project_settings.h"
#include "servers/rendering/virtual_geometry.h"

using namespace RendererRD;

VirtualGeometryEffect::VirtualGeometryEffect() {
	pixel_error = GLOBAL_GET("rendering/virtual_geometry/pixel_error");

	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
}

VirtualGeometryEffect::~VirtualGeometryEffect() {
	shader.version_free(shader_version);
}

RID VirtualGeometryEffect::get_shader() const {
	return const_cast<VirtualGeometrySelectShaderRD &>(shader).version_get_shader(shader_version, 0);
}

RID VirtualGeometryEffect::create_command_buffer(const Vector<uint8_t> &p_cluster_data, uint32_t p_leaf_index_count) const {
	const uint32_t cluster_count = p_cluster_data.size() / sizeof(VirtualGeometryCluster);
	const VirtualGeometryCluster *clusters = reinterpret_cast<const VirtualGeometryCluster *>(p_cluster_data.ptr());

	Vector<uint8_t> data;
	data.resize(cluster_count * 2 * COMMAND_STRIDE);
	uint32_t *w = reinterpret_cast<uint32_t *>(data.ptrw());
	for (uint32_t range = 0; range < 2; range++) {
		for (uint32_t i = 0; i < cluster_count; i++) {
			uint32_t *cmd = w + (range * cluster_count + i) * 5;
			cmd[0] = clusters[i].index_count;
			cmd[1] = clusters[i].index_offset < p_leaf_index_count ? 1 : 0;
			cmd[2] = clusters[i].index_offset;
			cmd[3] = 0;
			cmd[4] = 0;
		}
	}
	return RD::get_singleton()->storage_buffer_create(data.size(), data, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
}

RID VirtualGeometryEffect::create_uniform_set(RID p_cluster_buffer, RID p_command_buffer) const {
	Vector<RD::Uniform> uniforms;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 0;
		u.append_id(p_cluster_buffer);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 1;
		u.append_id(p_command_buffer);
		uniforms.push_back(u);
	}
	return RD::get_singleton()->uniform_set_create(uniforms, get_shader(), 0);
}

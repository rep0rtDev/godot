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
#include "core/object/callable_mp.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/virtual_geometry.h"

using namespace RendererRD;

SafeNumeric<uint64_t> VirtualGeometryEffect::stats[STAT_MAX];

void VirtualGeometryEffect::_stats_received(const Vector<uint8_t> &p_data) {
	ERR_FAIL_COND(p_data.size() < int(sizeof(uint32_t) * STAT_MAX));
	const uint32_t *r = reinterpret_cast<const uint32_t *>(p_data.ptr());
	for (int i = 0; i < STAT_MAX; i++) {
		stats[i].set(r[i]);
	}
}

void VirtualGeometryEffect::begin_frame_stats(uint64_t p_frame) {
	if (!collect_stats || p_frame == stats_frame) {
		return;
	}
	if (stats_frame != 0) {
		RD::get_singleton()->buffer_get_data_async(stats_buffer, callable_mp_static(&VirtualGeometryEffect::_stats_received));
	}
	RD::get_singleton()->buffer_clear(stats_buffer, 0, sizeof(uint32_t) * STAT_MAX);
	stats_frame = p_frame;
}

VirtualGeometryEffect::VirtualGeometryEffect() {
	pixel_error = GLOBAL_GET("rendering/virtual_geometry/pixel_error");
	use_indirect_count = RD::get_singleton()->has_feature(RD::SUPPORTS_DRAW_INDIRECT_COUNT);
	collect_stats = GLOBAL_GET("rendering/virtual_geometry/debug/collect_statistics");
	supports_software_raster = use_indirect_count && RD::get_singleton()->has_feature(RD::SUPPORTS_BUFFER_INT64_ATOMICS);
	software_raster = supports_software_raster && bool(GLOBAL_GET("rendering/virtual_geometry/software_raster"));
	software_raster_max_pixels = GLOBAL_GET("rendering/virtual_geometry/software_raster_max_pixels");
	print_verbose(vformat("Virtual geometry: indirect count draws %s, buffer int64 atomics %s, statistics %s.", use_indirect_count ? "enabled" : "disabled", supports_software_raster ? "supported" : "unsupported", collect_stats ? "enabled" : "disabled"));

	Vector<uint8_t> zero;
	zero.resize_initialized(sizeof(uint32_t) * STAT_MAX);
	stats_buffer = RD::get_singleton()->storage_buffer_create(zero.size(), zero);

	occlusion_culling = GLOBAL_GET("rendering/virtual_geometry/occlusion_culling");
	shadow_window_culling = GLOBAL_GET("rendering/virtual_geometry/shadow_window_culling");

	Vector<String> modes;
	modes.push_back("");
	modes.push_back("\n#define MODE_OCCLUSION\n");
	modes.push_back("\n#define MODE_TRAVERSE\n");
	modes.push_back("\n#define MODE_INSTANCE\n#define MODE_OCCLUSION\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}

	if (supports_software_raster) {
		Vector<String> raster_modes;
		raster_modes.push_back("");
		raster_shader.initialize(raster_modes);
		raster_shader_version = raster_shader.version_create();
		raster_pipeline = RD::get_singleton()->compute_pipeline_create(raster_shader.version_get_shader(raster_shader_version, 0));
		resolve_shader.initialize(raster_modes);
		resolve_shader_version = resolve_shader.version_create();
		Vector<String> tiles_modes;
		tiles_modes.push_back("\n#define MODE_COUNT\n");
		tiles_modes.push_back("\n#define MODE_PREFIX\n");
		tiles_modes.push_back("\n#define MODE_FILL\n");
		tiles_shader.initialize(tiles_modes);
		tiles_shader_version = tiles_shader.version_create();
		for (int i = 0; i < TILES_MAX; i++) {
			tiles_pipelines[i] = RD::get_singleton()->compute_pipeline_create(tiles_shader.version_get_shader(tiles_shader_version, i));
		}
	}

	Vector<String> hzb_modes;
	hzb_modes.push_back("\n#define MODE_FROM_DEPTH\n");
	hzb_modes.push_back("");
	hzb_shader.initialize(hzb_modes);
	hzb_shader_version = hzb_shader.version_create();
	for (int i = 0; i < 2; i++) {
		hzb_pipelines[i] = RD::get_singleton()->compute_pipeline_create(hzb_shader.version_get_shader(hzb_shader_version, i));
	}
}

RID VirtualGeometryEffect::build_hzb(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_depth) {
	static const StringName context = SNAME("VirtualGeometry");
	static const StringName name = SNAME("hzb");

	const Size2i depth_size = p_render_buffers->get_internal_size();
	Size2i hzb_size(1, 1);
	while (hzb_size.x * 2 <= depth_size.x) {
		hzb_size.x *= 2;
	}
	while (hzb_size.y * 2 <= depth_size.y) {
		hzb_size.y *= 2;
	}
	uint32_t mipmaps = 1;
	while ((MAX(hzb_size.x, hzb_size.y) >> mipmaps) > 0) {
		mipmaps++;
	}
	if (!p_render_buffers->has_texture(context, name)) {
		p_render_buffers->create_texture(context, name, RD::DATA_FORMAT_R32_SFLOAT, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, hzb_size, 1, mipmaps);
	}

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	Size2i source_size = depth_size;
	for (uint32_t mip = 0; mip < mipmaps; mip++) {
		const Size2i dest_size(MAX(hzb_size.x >> mip, 1), MAX(hzb_size.y >> mip, 1));
		const int mode = mip == 0 ? 0 : 1;
		RID shader_rid = hzb_shader.version_get_shader(hzb_shader_version, mode);
		RD::Uniform u_source;
		if (mip == 0) {
			u_source = RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, p_depth }));
		} else {
			u_source = RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ p_render_buffers->get_texture_slice(context, name, 0, mip - 1) }));
		}
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ p_render_buffers->get_texture_slice(context, name, 0, mip) }));

		HzbPushConstant push_constant = { { source_size.x, source_size.y }, { dest_size.x, dest_size.y } };
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, hzb_pipelines[mode]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_source), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_dest), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(push_constant));
		RD::get_singleton()->compute_list_dispatch(compute_list, Math::division_round_up(dest_size.x, 8), Math::division_round_up(dest_size.y, 8), 1);
		if (mip + 1 < mipmaps) {
			RD::get_singleton()->compute_list_add_barrier(compute_list);
		}
		source_size = dest_size;
	}
	RD::get_singleton()->compute_list_end();

	return p_render_buffers->get_texture(context, name);
}

RID VirtualGeometryEffect::get_occlusion_uniform_set(RID p_hzb, ShaderMode p_mode) {
	RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::Uniform u_hzb(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, p_hzb }));
	return UniformSetCacheRD::get_singleton()->get_cache(get_shader(p_mode), 1, u_hzb);
}

RID VirtualGeometryEffect::create_visibility_buffer(uint32_t p_cluster_count) const {
	Vector<uint8_t> zero;
	zero.resize_initialized(sizeof(uint32_t) * MAX(p_cluster_count, 1u));
	return RD::get_singleton()->storage_buffer_create(zero.size(), zero);
}

VirtualGeometryEffect::~VirtualGeometryEffect() {
	for (const KeyValue<RD::FramebufferFormatID, RID> &E : resolve_pipelines) {
		RD::get_singleton()->free_rid(E.value);
	}
	if (visibility_buffer.is_valid()) {
		RD::get_singleton()->free_rid(visibility_buffer);
	}
	if (entry_buffer.is_valid()) {
		RD::get_singleton()->free_rid(entry_buffer);
	}
	if (tile_command_buffer.is_valid()) {
		RD::get_singleton()->free_rid(tile_command_buffer);
	}
	if (tile_buffer.is_valid()) {
		RD::get_singleton()->free_rid(tile_buffer);
	}
	if (raster_shader_version.is_valid()) {
		raster_shader.version_free(raster_shader_version);
		resolve_shader.version_free(resolve_shader_version);
		tiles_shader.version_free(tiles_shader_version);
	}
	hzb_shader.version_free(hzb_shader_version);
	RD::get_singleton()->free_rid(stats_buffer);
	shader.version_free(shader_version);
}

RID VirtualGeometryEffect::get_shader(ShaderMode p_mode) const {
	return const_cast<VirtualGeometrySelectShaderRD &>(shader).version_get_shader(shader_version, p_mode);
}

RID VirtualGeometryEffect::create_work_buffer(uint32_t p_node_count, uint32_t p_cluster_count) const {
	Vector<uint8_t> zero;
	// Header, traversal queue, candidates, then the early and late software raster lists.
	zero.resize_initialized(WORK_HEADER_SIZE + sizeof(uint32_t) * (p_node_count + 3 * p_cluster_count));
	return RD::get_singleton()->storage_buffer_create(zero.size(), zero, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
}

RID VirtualGeometryEffect::create_command_buffer(const Vector<uint8_t> &p_cluster_data, uint32_t p_leaf_index_count) const {
	uint32_t cluster_count = 0;
	uint32_t node_count = 0;
	ERR_FAIL_COND_V(!VirtualGeometry::unpack_header(p_cluster_data, cluster_count, node_count), RID());
	const VirtualGeometryCluster *clusters = reinterpret_cast<const VirtualGeometryCluster *>(p_cluster_data.ptr() + VirtualGeometry::get_cluster_data_offset());

	// Leaves are packed first so the initial contents are valid for both the compacted and the per-slot layout.
	Vector<uint8_t> data;
	data.resize(HEADER_SIZE + cluster_count * COMMAND_RANGES * COMMAND_STRIDE);
	memset(data.ptrw(), 0, data.size());
	uint32_t *header = reinterpret_cast<uint32_t *>(data.ptrw());
	uint32_t *w = reinterpret_cast<uint32_t *>(data.ptrw() + HEADER_SIZE);
	uint32_t leaf_clusters = 0;
	for (uint32_t i = 0; i < cluster_count; i++) {
		if (clusters[i].self_error != 0.0f) {
			continue; // Only the finest clusters have no simplification error.
		}
		for (uint32_t range = 0; range < 2; range++) {
			// Non-indexed draw pulling its vertices: [vertex_count, instance_count, first_vertex = cluster * 384, first_instance].
			uint32_t *cmd = w + (range * cluster_count + leaf_clusters) * 5;
			cmd[0] = (clusters[i].page_and_triangles & 0xFF) * 3;
			cmd[1] = 1;
			cmd[2] = i * 384;
			cmd[3] = 0;
		}
		leaf_clusters++;
	}
	header[0] = leaf_clusters;
	header[1] = leaf_clusters;
	return RD::get_singleton()->storage_buffer_create(data.size(), data, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
}

RID VirtualGeometryEffect::create_uniform_set(RID p_cluster_buffer, RID p_command_buffer, RID p_visibility_buffer, RID p_node_buffer, RID p_work_buffer, RID p_page_table_buffer, RID p_children_buffer, RID p_request_buffer, ShaderMode p_mode) const {
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
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 2;
		u.append_id(stats_buffer);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 3;
		u.append_id(p_visibility_buffer);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 4;
		u.append_id(p_node_buffer);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.binding = 5;
		u.append_id(p_work_buffer);
		uniforms.push_back(u);
	}
	const RID streaming[3] = { p_page_table_buffer, p_children_buffer, p_request_buffer };
	for (uint32_t i = 0; i < 3; i++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6 + i, streaming[i]));
	}
	return RD::get_singleton()->uniform_set_create(uniforms, get_shader(p_mode), 0);
}

void VirtualGeometryEffect::begin_software_raster(const Size2i &p_size, uint32_t p_slot_count) {
	ERR_FAIL_COND(!supports_software_raster);
	RD *rd = RD::get_singleton();
	const uint32_t visibility_size_bytes = uint32_t(p_size.x) * uint32_t(p_size.y) * sizeof(uint64_t);
	if (visibility_buffer.is_null() || visibility_size != p_size) {
		if (visibility_buffer.is_valid()) {
			rd->free_rid(visibility_buffer);
		}
		visibility_buffer = rd->storage_buffer_create(visibility_size_bytes);
		visibility_size = p_size;
		raster_frame_uniform_set = RID();
		resolve_uniform_set = RID();
		tiles_uniform_set = RID();
	}
	// A tile has 64 pixels, so it lands in at most 64 slot lists; that bounds the lists exactly.
	const uint32_t tile_count = ((p_size.x + TILE_SIZE - 1) / TILE_SIZE) * ((p_size.y + TILE_SIZE - 1) / TILE_SIZE);
	const uint32_t tile_words = p_slot_count + tile_count * MIN(p_slot_count, TILE_SIZE * TILE_SIZE);
	if (tile_buffer.is_null() || tile_buffer_words < tile_words) {
		if (tile_buffer.is_valid()) {
			rd->free_rid(tile_buffer);
		}
		tile_buffer_words = MAX(tile_words, 1u);
		tile_buffer = rd->storage_buffer_create(tile_buffer_words * sizeof(uint32_t));
		tiles_uniform_set = RID();
	}
	if (tile_command_buffer.is_null() || tile_command_capacity < p_slot_count) {
		if (tile_command_buffer.is_valid()) {
			rd->free_rid(tile_command_buffer);
		}
		tile_command_capacity = MAX(p_slot_count, 1u);
		tile_command_buffer = rd->storage_buffer_create(tile_command_capacity * TILE_COMMAND_STRIDE, Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		tiles_uniform_set = RID();
	}
	tile_slot_count = p_slot_count;
	if (entry_buffer.is_null()) {
		entry_buffer = rd->storage_buffer_create(sizeof(uint32_t) * 2 + sizeof(uint32_t) * 2 * ENTRY_CAPACITY);
	}
	rd->buffer_clear(visibility_buffer, 0, visibility_size_bytes);
	rd->buffer_clear(entry_buffer, 0, sizeof(uint32_t) * 2);

	if (!raster_frame_uniform_set.is_valid() || !rd->uniform_set_is_valid(raster_frame_uniform_set)) {
		Vector<RD::Uniform> uniforms;
		RD::Uniform u_visibility(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, visibility_buffer);
		uniforms.push_back(u_visibility);
		RD::Uniform u_entries(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, entry_buffer);
		uniforms.push_back(u_entries);
		raster_frame_uniform_set = rd->uniform_set_create(uniforms, raster_shader.version_get_shader(raster_shader_version, 0), 1);
	}
}

RID VirtualGeometryEffect::create_raster_uniform_set(RID p_cluster_buffer, RID p_work_buffer, RID p_page_buffer, RID p_page_table_buffer) const {
	ERR_FAIL_COND_V(!supports_software_raster, RID());
	Vector<RD::Uniform> uniforms;
	const RID buffers[4] = { p_cluster_buffer, p_work_buffer, p_page_buffer, p_page_table_buffer };
	for (uint32_t i = 0; i < 4; i++) {
		RD::Uniform u(RD::UNIFORM_TYPE_STORAGE_BUFFER, i, buffers[i]);
		uniforms.push_back(u);
	}
	return RD::get_singleton()->uniform_set_create(uniforms, const_cast<VirtualGeometryRasterShaderRD &>(raster_shader).version_get_shader(raster_shader_version, 0), 0);
}

void VirtualGeometryEffect::classify_software_tiles() {
	ERR_FAIL_COND(tile_buffer.is_null() || visibility_buffer.is_null());
	if (tile_slot_count == 0) {
		return;
	}
	RD *rd = RD::get_singleton();
	if (!tiles_uniform_set.is_valid() || !rd->uniform_set_is_valid(tiles_uniform_set)) {
		Vector<RD::Uniform> uniforms;
		const RID buffers[4] = { visibility_buffer, entry_buffer, tile_buffer, tile_command_buffer };
		for (uint32_t i = 0; i < 4; i++) {
			uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, i, buffers[i]));
		}
		tiles_uniform_set = rd->uniform_set_create(uniforms, tiles_shader.version_get_shader(tiles_shader_version, TILES_COUNT), 0);
	}
	rd->buffer_clear(tile_command_buffer, 0, tile_slot_count * TILE_COMMAND_STRIDE);
	rd->buffer_clear(tile_buffer, 0, tile_slot_count * sizeof(uint32_t));

	TilesPushConstant push_constant = {};
	push_constant.size[0] = visibility_size.x;
	push_constant.size[1] = visibility_size.y;
	push_constant.slot_count = tile_slot_count;
	const uint32_t tiles_x = (visibility_size.x + TILE_SIZE - 1) / TILE_SIZE;
	const uint32_t tiles_y = (visibility_size.y + TILE_SIZE - 1) / TILE_SIZE;

	RD::ComputeListID compute_list = rd->compute_list_begin();
	for (int mode = 0; mode < TILES_MAX; mode++) {
		if (mode > 0) {
			rd->compute_list_add_barrier(compute_list);
		}
		rd->compute_list_bind_compute_pipeline(compute_list, tiles_pipelines[mode]);
		rd->compute_list_bind_uniform_set(compute_list, tiles_uniform_set, 0);
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(push_constant));
		if (mode == TILES_PREFIX) {
			rd->compute_list_dispatch(compute_list, 1, 1, 1);
		} else {
			rd->compute_list_dispatch(compute_list, tiles_x, tiles_y, 1);
		}
	}
	rd->compute_list_end();
}

void VirtualGeometryEffect::resolve_software_depth(RID p_framebuffer) {
	ERR_FAIL_COND(visibility_buffer.is_null());
	RD *rd = RD::get_singleton();
	RID shader_rd = resolve_shader.version_get_shader(resolve_shader_version, 0);
	const RD::FramebufferFormatID format = rd->framebuffer_get_format(p_framebuffer);
	RID *pipeline = resolve_pipelines.getptr(format);
	if (!pipeline) {
		RD::PipelineDepthStencilState depth_state;
		depth_state.enable_depth_test = true;
		depth_state.enable_depth_write = true;
		depth_state.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
		pipeline = &resolve_pipelines.insert(format, rd->render_pipeline_create(shader_rd, format, RD::INVALID_ID, RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), depth_state, RD::PipelineColorBlendState::create_disabled(0), 0))->value;
	}
	if (!resolve_uniform_set.is_valid() || !rd->uniform_set_is_valid(resolve_uniform_set)) {
		Vector<RD::Uniform> uniforms;
		RD::Uniform u(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, visibility_buffer);
		uniforms.push_back(u);
		resolve_uniform_set = rd->uniform_set_create(uniforms, shader_rd, 0);
	}

	ResolvePushConstant push_constant = {};
	push_constant.size[0] = visibility_size.x;
	push_constant.size[1] = visibility_size.y;
	push_constant.bias = SOFTWARE_DEPTH_BIAS;

	RD::DrawListID draw_list = rd->draw_list_begin(p_framebuffer, RD::DRAW_DEFAULT_ALL);
	rd->draw_list_bind_render_pipeline(draw_list, *pipeline);
	rd->draw_list_bind_uniform_set(draw_list, resolve_uniform_set, 0);
	rd->draw_list_set_push_constant(draw_list, &push_constant, sizeof(push_constant));
	rd->draw_list_draw(draw_list, false, 1, 3);
	rd->draw_list_end();
}

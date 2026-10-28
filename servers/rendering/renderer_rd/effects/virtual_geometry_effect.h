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

#include "core/templates/safe_refcount.h"
#include "servers/rendering/renderer_rd/shaders/effects/virtual_geometry_hzb.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/virtual_geometry_raster.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/virtual_geometry_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/virtual_geometry_select.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/virtual_geometry_tiles.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

class VirtualGeometryEffect {
public:
	static constexpr uint32_t COMMAND_STRIDE = sizeof(uint32_t) * 5;
	static constexpr uint32_t GROUP_SIZE = 64;
	static constexpr uint32_t FLAG_ORTHOGONAL = 1;
	static constexpr uint32_t FLAG_COMPACT = 2;
	static constexpr uint32_t FLAG_STATS = 4;
	static constexpr uint32_t FLAG_OCCLUSION_PHASE1 = 8;
	static constexpr uint32_t FLAG_FLIP_Y = 16;
	static constexpr uint32_t FLAG_HIERARCHY = 32;
	// Small camera clusters go to the compute rasterizer; the size limit is packed in the upper flag bits.
	static constexpr uint32_t FLAG_SOFTWARE = 64;
	// Shadow window pass: frustum-cull against the window and write its own command range (index in the bits above SOFTWARE_LIMIT_SHIFT).
	static constexpr uint32_t FLAG_WINDOW = 128;
	static constexpr uint32_t WINDOW_RANGE_SHIFT = 16;
	static constexpr uint32_t SOFTWARE_LIMIT_SHIFT = 16;
	// Traversal dispatch size, candidate count and the software raster dispatch sizes, see create_work_buffer().
	static constexpr uint32_t WORK_HEADER_SIZE = 64;
	static constexpr uint32_t WORK_SOFTWARE_EARLY_OFFSET = 8 * sizeof(uint32_t);
	static constexpr uint32_t WORK_SOFTWARE_LATE_OFFSET = 11 * sizeof(uint32_t);

	enum ShaderMode {
		MODE_SELECT,
		MODE_OCCLUSION,
		MODE_TRAVERSE,
		MODE_INSTANCE_OCCLUSION,
		MODE_MAX,
	};
	// Draw counts (camera, shadow, late camera, software early, software late) padded to 32 bytes,
	// followed by the five command ranges. The software ranges are only drawn by the color pass.
	// Counts for the camera, shadow, late, software and software-late ranges plus one per virtual shadow window culled this frame.
	static constexpr uint32_t WINDOW_RANGE_FIRST = 5;
	static constexpr uint32_t WINDOW_RANGES = 16;
	static constexpr uint32_t COMMAND_RANGES = WINDOW_RANGE_FIRST + WINDOW_RANGES;
	static constexpr uint32_t HEADER_SIZE = 96;
	static_assert(HEADER_SIZE >= sizeof(uint32_t) * COMMAND_RANGES);

	struct PushConstant {
		float modelview[12];
		float projection[16];
		uint32_t cluster_count;
		float lod_scale;
		uint32_t flags;
		uint32_t node_count;
	};
	static_assert(sizeof(PushConstant) == 128);

	struct RasterPushConstant {
		float model_view_projection[16];
		uint32_t size[2];
		uint32_t list_offset;
		uint32_t flags;
		uint32_t slot;
		uint32_t entry_capacity;
		uint32_t pad[2];
	};
	static_assert(sizeof(RasterPushConstant) == 96);
	// Drop triangles by the sign of their pixel space area, from the material cull mode.
	static constexpr uint32_t RASTER_FLAG_CULL_POSITIVE = 2;
	static constexpr uint32_t RASTER_FLAG_CULL_NEGATIVE = 4;
	static constexpr uint32_t RASTER_FLAG_COMPACT = 8; // Shadow window raster: the command range is compacted with a count.
	// Rasterized clusters per frame; each visibility buffer id holds the entry in 25 bits.
	static constexpr uint32_t ENTRY_CAPACITY = 1 << 20;
	// Material pass tiles, see virtual_geometry_tiles.glsl.
	static constexpr uint32_t TILE_SIZE = 8;
	static constexpr uint32_t TILE_COMMAND_STRIDE = sizeof(uint32_t) * 4;

	// GPU counters: camera clusters, camera triangles, shadow clusters, shadow triangles.
	enum Stat {
		STAT_CLUSTERS,
		STAT_TRIANGLES,
		STAT_SHADOW_CLUSTERS,
		STAT_SHADOW_TRIANGLES,
		STAT_SOFTWARE_CLUSTERS,
		STAT_SOFTWARE_TRIANGLES,
		STAT_WINDOW_CLUSTERS,
		STAT_WINDOW_TRIANGLES,
		STAT_MAX,
	};

private:
	VirtualGeometrySelectShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];

	struct HzbPushConstant {
		int32_t source_size[2];
		int32_t dest_size[2];
	};
	VirtualGeometryHzbShaderRD hzb_shader;
	RID hzb_shader_version;
	RID hzb_pipelines[2];
	bool occlusion_culling = true;
	bool shadow_window_culling = true;
	float pixel_error = 1.0f;
	bool use_indirect_count = false;

	bool collect_stats = false;
	bool supports_software_raster = false;
	bool software_raster = false;
	bool shadow_compute_raster = false;
	float software_raster_max_pixels = 64.0f;
	RID stats_buffer;
	uint64_t stats_frame = 0;

	struct ResolvePushConstant {
		uint32_t size[2];
		float bias;
		uint32_t pad;
	};
	VirtualGeometryRasterShaderRD raster_shader;
	RID raster_shader_version;
	RID raster_pipeline;
	RID shadow_raster_pipeline;
	VirtualGeometryResolveShaderRD resolve_shader;
	RID resolve_shader_version;
	HashMap<RD::FramebufferFormatID, RID> resolve_pipelines;
	RID visibility_buffer;
	Size2i visibility_size;
	RID entry_buffer;
	RID raster_frame_uniform_set;
	RID resolve_uniform_set;

	struct TilesPushConstant {
		uint32_t size[2];
		uint32_t slot_count;
		uint32_t pad;
	};
	enum TilesMode {
		TILES_COUNT,
		TILES_PREFIX,
		TILES_FILL,
		TILES_MAX,
	};
	VirtualGeometryTilesShaderRD tiles_shader;
	RID tiles_shader_version;
	RID tiles_pipelines[TILES_MAX];
	RID tile_buffer;
	RID tile_command_buffer;
	uint32_t tile_command_capacity = 0;
	uint32_t tile_buffer_words = 0;
	uint32_t tile_slot_count = 0;
	RID tiles_uniform_set;
	static SafeNumeric<uint64_t> stats[STAT_MAX];
	static void _stats_received(const Vector<uint8_t> &p_data);

public:
	VirtualGeometryEffect();
	~VirtualGeometryEffect();

	RID get_shader(ShaderMode p_mode = MODE_SELECT) const;
	RID get_pipeline(bool p_occlusion = false) const { return pipelines[p_occlusion ? MODE_OCCLUSION : MODE_SELECT]; }
	RID get_traverse_pipeline() const { return pipelines[MODE_TRAVERSE]; }
	RID get_instance_occlusion_pipeline() const { return pipelines[MODE_INSTANCE_OCCLUSION]; }
	bool uses_occlusion_culling() const { return occlusion_culling && use_indirect_count; }

	// Builds the reversed-Z depth pyramid for p_depth and returns it, sized to the largest power of two that fits.
	RID build_hzb(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_depth);
	RID get_occlusion_uniform_set(RID p_hzb, ShaderMode p_mode = MODE_OCCLUSION);
	RID create_visibility_buffer(uint32_t p_cluster_count) const;
	float get_pixel_error() const { return pixel_error; }
	bool uses_indirect_count() const { return use_indirect_count; }
	bool uses_window_culling() const { return shadow_window_culling; }
	bool collects_stats() const { return collect_stats; }
	bool can_rasterize_in_compute() const { return supports_software_raster; }
	bool uses_software_raster() const { return software_raster; }
	bool uses_shadow_raster() const { return shadow_compute_raster; }
	float get_software_raster_max_pixels() const { return software_raster_max_pixels; }

	// Allocates and clears the 64-bit visibility buffer and the entry table for this frame.
	// p_slot_count is the number of surfaces rasterized this frame, which also sizes the material tile lists.
	void begin_software_raster(const Size2i &p_size, uint32_t p_slot_count);
	// Once the visibility buffer is final: builds the per-surface tile lists and indirect draws of the material pass.
	void classify_software_tiles();
	// Storage buffer of fill cursors and tile lists, read by the material pass vertex stage.
	RID get_tile_buffer() const { return tile_buffer; }
	// One non-indexed indirect draw command per surface slot, TILE_COMMAND_STRIDE apart.
	RID get_tile_command_buffer() const { return tile_command_buffer; }
	RID get_raster_pipeline() const { return raster_pipeline; }
	RID get_shadow_raster_pipeline() const { return shadow_raster_pipeline; }
	RID get_shadow_raster_frame_uniform_set(RID p_buffer);
	// Set 1 of the raster shader: visibility buffer and entry table.
	RID get_raster_frame_uniform_set() const { return raster_frame_uniform_set; }
	RID get_visibility_buffer() const { return visibility_buffer; }
	RID get_entry_buffer() const { return entry_buffer; }
	Size2i get_visibility_size() const { return visibility_size; }
	// Matches VGSurface in scene_forward_clustered_vg_inc.glsl.
	struct MaterialParams {
		uint32_t size[2];
		uint32_t slot;
		uint32_t pad;
		float depth_bias;
		uint32_t pad1[3];
	};
	static_assert(sizeof(MaterialParams) == 32);
	static constexpr float SOFTWARE_DEPTH_BIAS = 1.0f / 65536.0f;
	RID create_raster_uniform_set(RID p_cluster_buffer, RID p_work_buffer, RID p_page_buffer, RID p_page_table_buffer) const;
	RID create_shadow_raster_uniform_set(RID p_cluster_buffer, RID p_command_buffer, RID p_page_buffer, RID p_page_table_buffer) const;
	// Writes the depth of every rasterized pixel into p_framebuffer's depth attachment.
	void resolve_software_depth(RID p_framebuffer);

	// Reads back the previous frame's counters and resets them; only the first call per frame does anything.
	void begin_frame_stats(uint64_t p_frame);
	uint64_t get_stat(Stat p_stat) const { return stats[p_stat].get(); }

	// Command buffer with a count header and two draw ranges (camera and shadow); both start as the full-detail leaf clusters.
	RID create_command_buffer(const Vector<uint8_t> &p_cluster_data, uint32_t p_leaf_index_count) const;
	// Traversal queue and candidate list for one instance of a surface with p_node_count hierarchy nodes.
	RID create_work_buffer(uint32_t p_node_count, uint32_t p_cluster_count) const;
	RID create_uniform_set(RID p_cluster_buffer, RID p_command_buffer, RID p_visibility_buffer, RID p_node_buffer, RID p_work_buffer, RID p_page_table_buffer, RID p_children_buffer, RID p_request_buffer, ShaderMode p_mode = MODE_SELECT) const;
};

} // namespace RendererRD

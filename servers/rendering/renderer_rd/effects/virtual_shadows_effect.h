/**************************************************************************/
/*  virtual_shadows_effect.h                                              */
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
#include "core/math/rect2.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/effects/virtual_shadows.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Virtual shadow maps for one directional light: a clipmap of LEVELS levels, each PAGES x PAGES pages of
// TEXELS x TEXELS texels, backed by one physical page pool. Pages are requested from the camera depth buffer,
// rendered in windows of WINDOW_PAGES x WINDOW_PAGES pages into a scratch depth target, copied into the pool and
// kept until the light moves or a moving instance touches them.
class VirtualShadowsEffect {
public:
	enum {
		LEVELS = 16,
		PAGES = 128,
		TEXELS = 128,
		WINDOW_PAGES = 8,
		WINDOW_TEXELS = WINDOW_PAGES * TEXELS,
		ENTRIES_PER_LEVEL = PAGES * PAGES,
		ENTRIES = LEVELS * ENTRIES_PER_LEVEL,
	};

	struct Window {
		uint32_t level = 0;
		int32_t wx = 0;
		int32_t wy = 0;
		Rect2i rect; // In the scratch target.
	};

private:
	enum Mode {
		MODE_MARK,
		MODE_COPY,
		MODE_MAX
	};

	static VirtualShadowsEffect *singleton;

	bool enabled = false;
	uint32_t debug_mode = 0;
	bool dumped = false;
	void _dump(RID p_texture, const String &p_path, int p_step);
	float base_extent = 4.0f;
	uint32_t pool_pages = 0;
	uint32_t pool_cols = 64;
	uint32_t windows_per_frame = 16;
	uint32_t scratch_cols = 4;

	VirtualShadowsShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];

	RID pool_texture;
	RID scratch_depth;
	RID scratch_fb;
	RID page_table_buffer;
	RID request_buffer;
	RID copy_buffer;
	RID mark_params_buffer;

	// Readback of page requests.
	Vector<uint32_t> requests;
	bool requests_pending = false;
	bool have_requests = false;
	Vector3 pending_camera;
	Vector3 marked_camera;
	bool marked_once = false;
	Vector3 request_camera;

	// The light.
	RID light;
	bool light_valid = false;
	Basis basis;
	float z_origin = 0.0f;
	float z_range = 0.0f;
	float normal_bias = 0.0f;

	// Page table: two words per entry (physical page << 1 | valid, absolute page key).
	Vector<uint32_t> table;
	Vector<uint32_t> entry_key;
	Vector<uint8_t> entry_valid;
	bool table_dirty = false;
	bool level_dirty[LEVELS] = {};
	Vector<uint32_t> phys_entry;
	Vector<uint64_t> phys_last_used;
	LocalVector<uint32_t> free_list;

	LocalVector<Window> windows;
	LocalVector<uint32_t> copies;
	uint64_t frame = 0;

	struct Stats {
		uint32_t requested = 0;
		uint32_t missing = 0;
		uint32_t rendered = 0;
		uint32_t windows_needed = 0;
	} stats;

	static void _requests_received(const Vector<uint8_t> &p_data);
	void _invalidate_all();
	void _free_entry(uint32_t p_entry);
	int32_t _alloc_page();
	static uint32_t _key(int32_t p_x, int32_t p_y) {
		return (uint32_t(p_x) & 0xFFFFu) | (uint32_t(p_y) << 16);
	}
	float _page_size(uint32_t p_level) const {
		return base_extent * float(1u << p_level) / float(PAGES);
	}
	Vector2 _light_xy(const Vector3 &p_world) const {
		Vector3 l = basis.xform_inv(p_world);
		return Vector2(l.x, l.y);
	}

public:
	static VirtualShadowsEffect *get_singleton() { return singleton; }

	bool is_enabled() const { return enabled; }
	RID get_pool_texture() const { return pool_texture; }
	RID get_page_table_buffer() const { return page_table_buffer; }
	RID get_scratch_framebuffer() const { return scratch_fb; }

	void begin_frame(uint64_t p_frame);
	// Marks the pages the visible pixels need. p_depth is the camera depth after the pre-pass.
	void mark(RID p_depth, const RenderSceneDataRD &p_scene_data, const Size2i &p_size);
	// Decides which windows to render this frame for the light and allocates their pages; false when this light is not
	// handled (only the first shadowed directional light is).
	bool plan_light(RID p_light_instance, const Transform3D &p_cascade_transform, float p_cascade_zfar, float p_normal_bias, const LocalVector<AABB> &p_invalidate);
	// Light-space (xy) extents, for culling casters against windows.
	Rect2 get_light_rect(const AABB &p_aabb) const;
	Rect2 get_window_light_rect(const Window &p_window) const;
	const LocalVector<Window> &get_windows() const { return windows; }
	Projection get_window_projection(const Window &p_window) const;
	Transform3D get_window_transform(const Window &p_window) const;
	// After the windows were rendered: copies their pages into the pool and uploads the page table.
	void copy_pages();
	// Fills the per-light data read by the scene shader.
	void fill_light_data(RID p_light_instance, bool p_shadows, float *r_light_view, float *r_depth_row, float *r_params, uint32_t *r_info) const;
	const Stats &get_stats() const { return stats; }

	VirtualShadowsEffect();
	~VirtualShadowsEffect();
};

} // namespace RendererRD

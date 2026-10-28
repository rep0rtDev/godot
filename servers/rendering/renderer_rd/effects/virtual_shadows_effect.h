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
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/effects/virtual_shadows.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Virtual shadow maps: one physical page pool shared by every virtual light. The directional light (slot 0) is a
// clipmap of LEVELS levels; spot lights (slots 1..SPOT_SLOTS) are perspective maps with SPOT_MIPS mips. Every level
// is PAGES x PAGES pages of TEXELS x TEXELS texels. Pages are requested from the camera depth buffer, rendered in
// windows of WINDOW_PAGES x WINDOW_PAGES pages into a scratch depth target, copied into the pool and kept until the
// light moves or a caster under them changes.
class VirtualShadowsEffect {
public:
	enum {
		LEVELS = 16,
		SPOT_SLOTS = 20, // Perspective slots: spot lights take one, omni lights six consecutive ones (cube faces).
		SPOT_MIPS = 6,
		TOTAL_LEVELS = LEVELS + SPOT_SLOTS * SPOT_MIPS,
		PAGES = 128,
		TEXELS = 128,
		WINDOW_PAGES = 8,
		WINDOW_TEXELS = WINDOW_PAGES * TEXELS,
		ENTRIES_PER_LEVEL = PAGES * PAGES,
		ENTRIES = TOTAL_LEVELS * ENTRIES_PER_LEVEL,
	};

	struct Window {
		uint32_t slot = 0;
		uint32_t level = 0; // Clipmap level or spot mip.
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

	struct Slot {
		RID light;
		bool valid = false;
		uint64_t last_seen = 0;
		uint32_t version = 1; // Key of the spot pages; bumped when the light moves.
		Transform3D transform;
		Transform3D light_transform; // Omni only: the light itself (transform is the face).
		Projection projection; // Spot only.
		float normal_bias = 0.0f;
		// Directional only: world anchored light frame and depth window.
		Basis basis;
		float z_origin = 0.0f;
		float z_range = 0.0f;
	};

	static VirtualShadowsEffect *singleton;

	bool enabled = false;
	uint32_t debug_mode = 0;
	uint32_t smrt_rays = 4;
	bool dumped = false;
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
	RID spot_params_buffer;

	// Readback of page requests.
	Vector<uint32_t> requests;
	bool requests_pending = false;
	bool have_requests = false;
	bool requests_ready = false; // This frame plans from a fresh readback.
	Vector3 pending_camera;
	Vector3 request_camera;
	Vector3 marked_camera;
	bool marked_once = false;

	Slot slots[1 + SPOT_SLOTS];

	// Page table: two words per entry (physical page << 1 | valid, key).
	Vector<uint32_t> table;
	Vector<uint32_t> entry_key;
	Vector<uint8_t> entry_valid;
	bool table_dirty = false;
	bool level_dirty[TOTAL_LEVELS] = {};
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
		uint32_t dropped = 0; // Requested pages that found no free physical page this frame.
	} stats, last_stats; // last_stats: the previous completed frame, for rendering info read from scripts.

	static void _requests_received(const Vector<uint8_t> &p_data);
	void _free_slot(uint32_t p_slot);
	void _free_entry(uint32_t p_entry);
	int32_t _alloc_page();
	void _dump(RID p_texture, const String &p_path, int p_step);
	static uint32_t _key(int32_t p_x, int32_t p_y) {
		return (uint32_t(p_x) & 0xFFFFu) | (uint32_t(p_y) << 16);
	}
	static uint32_t _slot_level(uint32_t p_slot, uint32_t p_level) {
		return p_slot == 0 ? p_level : LEVELS + (p_slot - 1) * SPOT_MIPS + p_level;
	}
	float _page_size(uint32_t p_level) const {
		return base_extent * float(1u << p_level) / float(PAGES);
	}
	Vector2 _light_xy(const Slot &p_slot, const Vector3 &p_world) const {
		Vector3 l = p_slot.basis.xform_inv(p_world);
		return Vector2(l.x, l.y);
	}
	int32_t _slot_of(RID p_light) const;
	void _plan_directional(Slot &p_slot, uint32_t p_slot_index, const Transform3D &p_cascade_transform, float p_cascade_zfar);
	void _plan_spot(Slot &p_slot, const Transform3D &p_transform, const Projection &p_projection);
	void _invalidate(uint32_t p_slot, const LocalVector<AABB> &p_invalidate);
	void _plan_windows(uint32_t p_slot);
	Rect2 _spot_ndc_rect(const Slot &p_slot, const AABB &p_aabb, bool &r_all) const;

public:
	static VirtualShadowsEffect *get_singleton() { return singleton; }

	bool is_enabled() const { return enabled; }
	RID get_pool_texture() const { return pool_texture; }
	RID get_page_table_buffer() const { return page_table_buffer; }
	RID get_spot_params_buffer() const { return spot_params_buffer; }
	RID get_scratch_framebuffer() const { return scratch_fb; }

	void begin_frame(uint64_t p_frame);
	// Marks the pages the visible pixels need. p_depth is the camera depth after the pre-pass.
	void mark(RID p_depth, const RenderSceneDataRD &p_scene_data, const Size2i &p_size);
	// Decides which windows to render this frame for the light and allocates their pages. p_invalidate holds the bounds
	// of casters that changed. Returns false when the light is not handled (no free slot, or not the first directional).
	// Basis of cube face p_face of an omni light (the window camera looks down -Z): must match vsm_omni_face() in the scene shader.
	static Basis omni_face_basis(int p_face);
	bool has_light(RID p_light_instance) const { return _slot_of(p_light_instance) >= 0; }
	bool plan_light(RID p_light_instance, bool p_directional, bool p_omni, const Transform3D &p_transform, const Projection &p_projection, float p_normal_bias, const LocalVector<AABB> &p_invalidate);
	void get_windows(RID p_light_instance, LocalVector<const Window *> &r_windows) const;
	Projection get_window_projection(const Window &p_window) const;
	Transform3D get_window_transform(const Window &p_window) const;
	// Whether a caster with these bounds can touch the window.
	bool window_touches(const Window &p_window, const AABB &p_aabb) const;
	// After the windows were rendered: copies their pages into the pool and uploads the page table.
	void copy_pages();
	// Per-light data read by the scene shader.
	void fill_directional_light_data(RID p_light_instance, bool p_shadows, float *r_light_view, float *r_depth_row, float *r_params, uint32_t *r_info) const;
	void fill_omni_light_data(RID p_light_instance, bool p_shadows, const Transform3D &p_camera_inverse_transform, float p_pixel_scale, float *r_light_matrix, float *r_matrix, float *r_params, uint32_t *r_info) const;
	void fill_spot_light_data(RID p_light_instance, bool p_shadows, const Transform3D &p_camera_inverse_transform, const Transform3D &p_light_transform, float p_pixel_scale, float *r_matrix, float *r_params, uint32_t *r_info) const;
	const Stats &get_stats() const { return stats; }
	const Stats &get_last_stats() const { return last_stats; }
	uint32_t get_pool_pages() const { return pool_pages; }
	uint32_t get_resident_pages() const { return pool_pages - free_list.size(); }

	VirtualShadowsEffect();
	~VirtualShadowsEffect();
};

} // namespace RendererRD

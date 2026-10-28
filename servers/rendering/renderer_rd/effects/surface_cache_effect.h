/**************************************************************************/
/*  surface_cache_effect.h                                                */
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

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/paged_array.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Surface cache for global illumination (Lumen-style): every instance gets up to six axis cards, orthographic
// "photographs" of its surface stored in one atlas (albedo, normal, orm, emission, depth). Cards are captured with
// the regular material pass on a per-frame budget and live until the instance moves, disappears or is unused.
class SurfaceCacheEffect {
public:
	struct Card {
		uint32_t instance_id = 0;
		int axis = 0; // 0..5: +X -X +Y -Y +Z -Z in object space.
		Rect2i rect; // Atlas texels.
		Transform3D capture_transform; // World camera transform used for the capture.
		Projection capture_projection;
		uint64_t captured_frame = 0;
		bool dirty = true;
	};

	struct Instance {
		RenderGeometryInstance *geometry = nullptr;
		Transform3D transform;
		AABB aabb; // Object space.
		uint64_t last_seen = 0;
		uint32_t first_card = 0;
		uint32_t card_count = 0;
	};

	struct Stats {
		uint32_t instances = 0;
		uint32_t cards = 0;
		uint32_t captured = 0;
		uint32_t dirty = 0;
		uint32_t atlas_texels = 0;
	};

	typedef void (*CaptureFunction)(void *p_userdata, const Transform3D &p_transform, const Projection &p_projection, const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_rect);

	static SurfaceCacheEffect *get_singleton();

	bool is_enabled() const { return enabled; }
	uint32_t get_debug_mode() const { return debug_mode; }

	// Registers the instances rendered this frame, builds cards for new or moved ones, drops stale ones.
	void begin_frame(uint64_t p_frame, const PagedArray<RenderGeometryInstance *> &p_instances, const Vector3 &p_camera_position);
	// Captures dirty cards within the budget, nearest to the camera first.
	void capture(CaptureFunction p_capture, void *p_userdata);

	RID get_albedo_texture() const { return albedo; }
	RID get_normal_texture() const { return normal; }
	RID get_orm_texture() const { return orm; }
	RID get_emission_texture() const { return emission; }
	RID get_depth_texture() const { return depth; }
	int get_atlas_size() const { return atlas_size; }
	const LocalVector<Card> &get_cards() const { return cards; }
	const Stats &get_stats() const { return stats; }

	SurfaceCacheEffect();
	~SurfaceCacheEffect();

private:
	static SurfaceCacheEffect *singleton;

	bool enabled = false;
	uint32_t debug_mode = 0;
	int atlas_size = 4096;
	int cards_per_frame = 300;
	float texel_density = 10.0f; // Texels per meter at the base distance.
	int min_card_size = 8;
	int max_card_size = 512;
	uint32_t unused_frames = 256;

	RID albedo;
	RID normal;
	RID orm;
	RID emission;
	RID depth;
	RID depth_stencil;
	RID framebuffer;

	// Shelf allocator: one shelf per row band, shelves keyed by height.
	struct Shelf {
		int y = 0;
		int height = 0;
		int next_x = 0;
	};
	LocalVector<Shelf> shelves;
	int shelf_next_y = 0;
	LocalVector<Rect2i> free_rects; // Returned by dropped cards, reused for equal sizes.

	LocalVector<Card> cards;
	LocalVector<uint32_t> free_cards;
	HashMap<RenderGeometryInstance *, uint32_t> instance_map;
	LocalVector<Instance> instances;
	LocalVector<uint32_t> free_instances;
	uint64_t frame = 0;
	Vector3 camera_position;
	Stats stats;
	PagedArrayPool<RenderGeometryInstance *> capture_pool;
	PagedArray<RenderGeometryInstance *> capture_list;

	bool _allocate_rect(const Size2i &p_size, Rect2i &r_rect);
	void _free_rect(const Rect2i &p_rect);
	void _build_cards(uint32_t p_instance);
	void _drop_cards(uint32_t p_instance);
	Size2i _card_size(const Vector3 &p_extent_world, int p_axis) const;
	void _card_view(const Instance &p_inst, int p_axis, Transform3D &r_transform, Projection &r_projection) const;
};

} // namespace RendererRD

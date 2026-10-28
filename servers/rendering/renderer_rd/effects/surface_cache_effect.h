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
#include "servers/rendering/renderer_rd/shaders/effects/surface_cache_light.glsl.gen.h"
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
		uint32_t first_card = 0xFFFFFFFF; // Block of 6 card slots, one per axis.
	};

	struct Stats {
		uint32_t instances = 0;
		uint32_t cards = 0;
		uint32_t captured = 0;
		uint32_t dirty = 0;
		uint32_t atlas_texels = 0;
		uint32_t lit = 0;
	};

	struct SkyInfo {
		int mode = 0; // 0 ambient color, 1 background color, 2 sky octmap.
		Vector3 color;
		float energy = 1.0f;
		Quaternion orientation;
		float border = 0.0f;
		RID octmap;
	};
	typedef void (*CaptureFunction)(void *p_userdata, const Transform3D &p_transform, const Projection &p_projection, const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_rect);

	static SurfaceCacheEffect *get_singleton();

	bool is_enabled() const { return enabled; }
	uint32_t get_debug_mode() const { return debug_mode; }

	// Registers the instances rendered this frame, builds cards for new or moved ones, drops stale ones.
	void begin_frame(uint64_t p_frame, const PagedArray<RenderGeometryInstance *> &p_instances, const Vector3 &p_camera_position);
	// Captures dirty cards within the budget, nearest to the camera first.
	void capture(CaptureFunction p_capture, void *p_userdata);
	// Direct lighting of cards captured this frame plus a round-robin share of the rest. Light buffers are in the
	// camera view space given by p_view_to_world; SDF cascades (may be empty) give shadows.
	void light_cards(RID p_directional_buffer, uint32_t p_directional_count, RID p_omni_buffer, uint32_t p_omni_count, RID p_spot_buffer, uint32_t p_spot_count, const Transform3D &p_view_to_world, const Vector<RID> &p_sdf_cascades, const Vector<Vector4> &p_sdf_offsets, uint32_t p_sdf_grid_size, float p_sdf_y_mult, const Vector3 &p_sky_radiance, const SkyInfo &p_sky);
	RID get_direct_texture() const { return direct; }
	RID get_final_texture() const { return final; }
	bool screen_probes_enabled() const { return screen_probes; }
	// Screen probes traced into the surface cache, resolved into the GI ambient buffer (alpha 1 where valid).
	void screen_probes_pass(RID p_depth, RID p_normal_roughness, RID p_ambient, RID p_reflection, RID p_hzb, const Size2i &p_internal_size, const Size2i &p_ambient_size, const Projection &p_projection, const Transform3D &p_cam_transform);
	RID get_last_reflection_texture() const { return last_reflection; }
	RID get_probe_texture() const { return probes; }
	RID get_screen_probe_texture() const { return screen_probe_texture[history_parity]; }
	RID get_radiance_cache_texture() const { return radiance_cache_texture; }
	RID get_last_ambient_texture() const { return last_ambient; }

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
	bool screen_probes = true;
	RID screen_probe_texture[2];
	int history_parity = 0;
	int screen_uniform_set_parity = -1;
	Projection prev_view_projection;
	bool has_prev_view_projection = false;
	Size2i screen_probe_size;
	RID screen_params_buffer;
	RID screen_uniform_set;
	RID screen_uniform_set_depth;
	RID screen_uniform_set_ambient;
	RID last_ambient;
	RID last_reflection;
	RID reflection_history[2];
	RID radiance_cache_texture;
	RID radiance_meta_buffer;
	RID radiance_cache_depth;
	bool radiance_cache_enabled = false;
	void _ensure_radiance_cache();
	static const uint32_t RADIANCE_PROBES = 16;
	static const uint32_t RADIANCE_CLIPMAPS = 4;
	static const uint32_t RADIANCE_OCT = 8;
	static const uint32_t RADIANCE_PROBES_PER_FRAME = 512;
	Size2i reflection_history_size;
	RID screen_uniform_set_reflection;
	RID screen_uniform_set_hzb;
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
	RID direct;
	RID probes; // atlas_size / 4 radiosity probes.
	RID final;
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
	LocalVector<uint32_t> free_blocks;
	HashMap<RenderGeometryInstance *, uint32_t> instance_map;
	LocalVector<Instance> instances;
	LocalVector<uint32_t> free_instances;
	uint64_t frame = 0;
	Vector3 camera_position;
	Stats stats;
	PagedArrayPool<RenderGeometryInstance *> capture_pool;
	PagedArray<RenderGeometryInstance *> capture_list;

	SurfaceCacheLightShaderRD light_shader;
	RID light_shader_version;
	enum LightMode {
		LIGHT_MODE_DIRECT,
		LIGHT_MODE_PROBES,
		LIGHT_MODE_RESOLVE,
		LIGHT_MODE_SCREEN_PROBES,
		LIGHT_MODE_SCREEN_RESOLVE,
		LIGHT_MODE_SCREEN_REFLECT,
		LIGHT_MODE_RADIANCE_PROBES,
		LIGHT_MODE_MAX
	};
	RID light_pipelines[LIGHT_MODE_MAX];
	RID instance_buffer;
	uint32_t instance_buffer_size = 0;
	RID grid_buffer;
	Vector3 grid_origin;
	bool grid_dirty = true;
	static const uint32_t GRID = 32;
	static const uint32_t GRID_SLOTS = 8;
	float grid_cell = 2.0f;
	RID card_buffer;
	uint32_t card_buffer_size = 0;
	RID light_params_buffer;
	RID light_uniform_set;
	RID sdf_uniform_set;
	RID sdf_params_buffer;
	RID dummy_sdf_tex;
	Vector<RID> sdf_uniform_set_textures;
	RID sdf_uniform_set_sky;
	RID dummy_sky_tex;
	LocalVector<uint32_t> captured_this_frame;
	uint32_t light_round_robin = 0;
	uint32_t light_share = 8;
	uint32_t light_debug = 0; // 1: SDF diagnostics in the direct atlas, 2: ray diagnostics in the probe atlas.

	bool _allocate_rect(const Size2i &p_size, Rect2i &r_rect);
	void _free_rect(const Rect2i &p_rect);
	void _build_cards(uint32_t p_instance);
	void _drop_cards(uint32_t p_instance);
	void _free_block(uint32_t p_instance);
	Size2i _card_size(const Vector3 &p_extent_world, int p_axis) const;
	void _card_view(const Instance &p_inst, int p_axis, Transform3D &r_transform, Projection &r_projection) const;
};

} // namespace RendererRD

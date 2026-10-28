/**************************************************************************/
/*  virtual_shadows_effect.cpp                                            */
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

#include "virtual_shadows_effect.h"

#include "core/config/project_settings.h"
#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/templates/pair.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

#include <cfloat>

using namespace RendererRD;

VirtualShadowsEffect *VirtualShadowsEffect::singleton = nullptr;

namespace {
struct MarkParams {
	float inv_projection[16];
	float inv_view[16];
	float light_view[16];
	float params[4]; // base extent, levels, projection[1][1], viewport height
	int32_t size[4]; // width, height, spot count, directional valid
};
struct SpotParams {
	float clip_from_world[16];
	float position_range[4];
	float info[4]; // first level, mips, tan(half fov), unused
};
} // namespace

VirtualShadowsEffect::VirtualShadowsEffect() {
	singleton = this;
	enabled = GLOBAL_GET("rendering/virtual_shadows/enabled");
	// Something valid to bind when the feature is off.
	page_table_buffer = RD::get_singleton()->storage_buffer_create(16);
	spot_params_buffer = RD::get_singleton()->storage_buffer_create(16); // Placeholders: the scene shader binds them even when disabled.
	if (!enabled) {
		return;
	}
	debug_mode = GLOBAL_GET("rendering/virtual_shadows/debug/mode");
	smrt_rays = GLOBAL_GET("rendering/virtual_shadows/smrt_rays");
	base_extent = GLOBAL_GET("rendering/virtual_shadows/base_extent");
	pool_pages = GLOBAL_GET("rendering/virtual_shadows/max_physical_pages");
	windows_per_frame = GLOBAL_GET("rendering/virtual_shadows/windows_per_frame");
	scratch_cols = uint32_t(Math::ceil(Math::sqrt(float(windows_per_frame))));

	Vector<String> modes;
	modes.push_back("\n#define MODE_MARK\n");
	modes.push_back("\n#define MODE_COPY\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}

	{
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R32_SFLOAT;
		tf.width = pool_cols * TEXELS;
		tf.height = ((pool_pages + pool_cols - 1) / pool_cols) * TEXELS;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		pool_texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
		RD::get_singleton()->texture_clear(pool_texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	{
		RD::TextureFormat tf;
		tf.format = LightStorage::get_shadow_atlas_depth_format(false);
		tf.width = scratch_cols * WINDOW_TEXELS;
		tf.height = ((windows_per_frame + scratch_cols - 1) / scratch_cols) * WINDOW_TEXELS;
		tf.usage_bits = LightStorage::get_shadow_atlas_depth_usage_bits() | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		scratch_depth = RD::get_singleton()->texture_create(tf, RD::TextureView());
		Vector<RID> fb;
		fb.push_back(scratch_depth);
		scratch_fb = RD::get_singleton()->framebuffer_create(fb);
	}
	RD::get_singleton()->free_rid(page_table_buffer);
	table.resize_initialized(ENTRIES * 2);
	page_table_buffer = RD::get_singleton()->storage_buffer_create(table.size() * sizeof(uint32_t), table.to_byte_array());
	entry_key.resize_initialized(ENTRIES);
	entry_valid.resize_initialized(ENTRIES);
	request_buffer = RD::get_singleton()->storage_buffer_create(ENTRIES / 32 * sizeof(uint32_t));
	RD::get_singleton()->buffer_clear(request_buffer, 0, ENTRIES / 32 * sizeof(uint32_t));
	copy_buffer = RD::get_singleton()->storage_buffer_create(uint64_t(windows_per_frame) * WINDOW_PAGES * WINDOW_PAGES * 4 * sizeof(uint32_t));
	mark_params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(MarkParams));
	RD::get_singleton()->free_rid(spot_params_buffer);
	spot_params_buffer = RD::get_singleton()->storage_buffer_create(sizeof(SpotParams) * SPOT_SLOTS);
	phys_entry.resize(pool_pages);
	phys_entry.fill(UINT32_MAX);
	phys_last_used.resize_initialized(pool_pages);
	for (int32_t i = int32_t(pool_pages) - 1; i >= 0; i--) {
		free_list.push_back(i);
	}
	print_verbose(vformat("Virtual shadows: pool of %d pages (%d MB), %d windows per frame.", pool_pages, uint64_t(pool_pages) * TEXELS * TEXELS * 4 / (1024 * 1024), windows_per_frame));
}

VirtualShadowsEffect::~VirtualShadowsEffect() {
	for (RID rid : { pool_texture, scratch_fb, scratch_depth, page_table_buffer, request_buffer, copy_buffer, mark_params_buffer, spot_params_buffer }) {
		if (rid.is_valid()) {
			RD::get_singleton()->free_rid(rid);
		}
	}
	if (shader_version.is_valid()) {
		shader.version_free(shader_version);
	}
	singleton = nullptr;
}

void VirtualShadowsEffect::begin_frame(uint64_t p_frame) {
	frame = p_frame;
	windows.clear();
	copies.clear();
	stats = Stats();
	requests_ready = have_requests;
	have_requests = false;
	// Spot lights that stopped casting shadows give their slot back.
	for (uint32_t s = 1; s <= SPOT_SLOTS; s++) {
		if (slots[s].valid && slots[s].last_seen + 60 < frame) {
			_free_slot(s);
		}
	}
}

void VirtualShadowsEffect::_requests_received(const Vector<uint8_t> &p_data) {
	VirtualShadowsEffect *self = singleton;
	if (!self) {
		return;
	}
	self->requests_pending = false;
	if (p_data.size() != int64_t(ENTRIES / 32 * sizeof(uint32_t))) {
		return;
	}
	self->requests.resize(ENTRIES / 32);
	memcpy(self->requests.ptrw(), p_data.ptr(), p_data.size());
	self->request_camera = self->pending_camera;
	self->have_requests = true;
}

void VirtualShadowsEffect::mark(RID p_depth, const RenderSceneDataRD &p_scene_data, const Size2i &p_size) {
	if (!enabled) {
		return;
	}
	MarkParams params;
	Projection correction;
	correction.set_depth_correction(p_scene_data.flip_y);
	MaterialStorage::store_camera((correction * p_scene_data.cam_projection).inverse(), params.inv_projection);
	MaterialStorage::store_transform(p_scene_data.cam_transform, params.inv_view);
	MaterialStorage::store_transform(Transform3D(slots[0].basis, Vector3()).affine_inverse(), params.light_view);
	params.params[0] = base_extent;
	params.params[1] = LEVELS;
	params.params[2] = p_scene_data.cam_projection.columns[1][1];
	params.params[3] = p_size.height;
	params.size[0] = p_size.width;
	params.size[1] = p_size.height;
	params.size[3] = slots[0].valid ? 1 : 0;

	SpotParams spots[SPOT_SLOTS];
	memset(spots, 0, sizeof(spots));
	uint32_t spot_count = 0;
	Projection spot_correction;
	spot_correction.set_depth_correction(false);
	for (uint32_t s = 1; s <= SPOT_SLOTS; s++) {
		const Slot &slot = slots[s];
		if (!slot.valid) {
			continue;
		}
		spot_count++;
		SpotParams &sp = spots[s - 1]; // Indexed by slot so the scene shader can address the faces of an omni light.
		MaterialStorage::store_camera(spot_correction * slot.projection * Projection(slot.transform.affine_inverse()), sp.clip_from_world);
		sp.position_range[0] = slot.transform.origin.x;
		sp.position_range[1] = slot.transform.origin.y;
		sp.position_range[2] = slot.transform.origin.z;
		sp.position_range[3] = slot.projection.get_z_far();
		sp.info[0] = _slot_level(s, 0);
		sp.info[1] = SPOT_MIPS;
		sp.info[2] = slot.projection.get_viewport_half_extents().x / slot.projection.get_z_near();
		sp.info[3] = 1;
	}
	params.size[2] = SPOT_SLOTS;
	RD::get_singleton()->buffer_update(spot_params_buffer, 0, sizeof(SpotParams) * SPOT_SLOTS, spots);
	if (!params.size[3] && !spot_count) {
		return;
	}
	RD::get_singleton()->buffer_update(mark_params_buffer, 0, sizeof(MarkParams), &params);

	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rid = shader.version_get_shader(shader_version, MODE_MARK);
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { sampler, p_depth });
	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 1, mark_params_buffer);
	RD::Uniform u_requests(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, request_buffer);
	RD::Uniform u_spots(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, spot_params_buffer);
	RID set = UniformSetCacheRD::get_singleton()->get_cache(shader_rid, 0, u_depth, u_params, u_requests, u_spots);

	// Read back what the previous frame marked, then clear and mark again.
	if (!requests_pending && marked_once) {
		requests_pending = true;
		pending_camera = marked_camera;
		RD::get_singleton()->buffer_get_data_async(request_buffer, callable_mp_static(&VirtualShadowsEffect::_requests_received));
	}
	RD::get_singleton()->buffer_clear(request_buffer, 0, ENTRIES / 32 * sizeof(uint32_t));
	marked_camera = p_scene_data.cam_transform.origin;
	marked_once = true;

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_MARK]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, set, 0);
	RD::get_singleton()->compute_list_dispatch(compute_list, (p_size.width + 7) / 8, (p_size.height + 7) / 8, 1);
	RD::get_singleton()->compute_list_end();
}

void VirtualShadowsEffect::_free_entry(uint32_t p_entry) {
	if (!entry_valid[p_entry]) {
		return;
	}
	const uint32_t phys = table[p_entry * 2] >> 1;
	entry_valid.write[p_entry] = 0;
	table.write[p_entry * 2] = 0;
	table.write[p_entry * 2 + 1] = 0;
	phys_entry.write[phys] = UINT32_MAX;
	free_list.push_back(phys);
	table_dirty = true;
	level_dirty[p_entry / ENTRIES_PER_LEVEL] = true;
}

void VirtualShadowsEffect::_free_slot(uint32_t p_slot) {
	const uint32_t levels = p_slot == 0 ? LEVELS : SPOT_MIPS;
	for (uint32_t l = 0; l < levels; l++) {
		const uint32_t first = _slot_level(p_slot, l) * ENTRIES_PER_LEVEL;
		for (uint32_t e = first; e < first + ENTRIES_PER_LEVEL; e++) {
			_free_entry(e);
		}
	}
	slots[p_slot].valid = false;
	slots[p_slot].light = RID();
	slots[p_slot].version++;
}

int32_t VirtualShadowsEffect::_alloc_page() {
	if (!free_list.is_empty()) {
		const uint32_t p = free_list[free_list.size() - 1];
		free_list.remove_at(free_list.size() - 1);
		return p;
	}
	// Under pressure evict the page that was idle longest; among equally idle pages the finest one goes first, so the
	// coarse pages that every pixel can fall back to survive.
	int32_t victim = -1;
	uint64_t victim_score = 0;
	for (uint32_t i = 0; i < pool_pages; i++) {
		if (phys_last_used[i] >= frame) {
			continue; // Wanted this frame.
		}
		const uint32_t level = phys_entry[i] / ENTRIES_PER_LEVEL;
		const uint32_t fineness = level < LEVELS ? LEVELS - level : SPOT_MIPS - (level - LEVELS) % SPOT_MIPS;
		const uint64_t score = (frame - phys_last_used[i]) * 64 + fineness;
		if (victim < 0 || score > victim_score) {
			victim = i;
			victim_score = score;
		}
	}
	if (victim < 0) {
		return -1;
	}
	_free_entry(phys_entry[victim]);
	free_list.remove_at(free_list.size() - 1);
	return victim;
}

int32_t VirtualShadowsEffect::_slot_of(RID p_light) const {
	for (uint32_t s = 0; s <= SPOT_SLOTS; s++) {
		if (slots[s].valid && slots[s].light == p_light) {
			return s;
		}
	}
	return -1;
}

void VirtualShadowsEffect::_plan_directional(Slot &p_slot, uint32_t p_slot_index, const Transform3D &p_cascade_transform, float p_cascade_zfar) {
	// Light frame: the cascade basis, world anchored. Depth range: a snapped window around what the cascade needs, so
	// that cached pages stay comparable while the camera moves.
	const Basis new_basis = p_cascade_transform.basis;
	bool reset = !p_slot.valid;
	for (int i = 0; i < 3 && !reset; i++) {
		reset = !new_basis.get_column(i).is_equal_approx(p_slot.basis.get_column(i));
	}
	const float z_max = new_basis.get_column(2).dot(p_cascade_transform.origin);
	const float z_min = z_max - p_cascade_zfar;
	const float snap = 128.0f;
	if (reset || z_max > p_slot.z_origin || z_min < p_slot.z_origin - p_slot.z_range) {
		_free_slot(p_slot_index);
		p_slot.basis = new_basis;
		p_slot.z_origin = Math::ceil(z_max / snap) * snap + snap;
		p_slot.z_range = Math::ceil((p_slot.z_origin - z_min) / snap) * snap + snap;
	}
	p_slot.transform = p_cascade_transform;
}

void VirtualShadowsEffect::_plan_spot(Slot &p_slot, const Transform3D &p_transform, const Projection &p_projection) {
	bool reset = !p_slot.valid || !p_slot.transform.is_equal_approx(p_transform);
	for (int c = 0; c < 4 && !reset; c++) {
		reset = !p_slot.projection.columns[c].is_equal_approx(p_projection.columns[c]);
	}
	if (reset) {
		const uint32_t s = &p_slot - slots;
		_free_slot(s);
		p_slot.transform = p_transform;
		p_slot.projection = p_projection;
	}
}

Rect2 VirtualShadowsEffect::_spot_ndc_rect(const Slot &p_slot, const AABB &p_aabb, bool &r_all) const {
	// NDC bounds of the caster; r_all when a corner is behind the light plane (then the caster can be anywhere).
	Projection clip = p_slot.projection * Projection(p_slot.transform.affine_inverse());
	Vector2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
	r_all = false;
	for (int c = 0; c < 8; c++) {
		const Vector3 e = p_aabb.get_endpoint(c);
		const Vector4 h = clip.xform(Vector4(e.x, e.y, e.z, 1.0));
		if (h.w <= 0.0001f) {
			r_all = true;
			return Rect2(-1, -1, 2, 2);
		}
		const Vector2 n(h.x / h.w, h.y / h.w);
		lo = lo.min(n);
		hi = hi.max(n);
	}
	return Rect2(lo, hi - lo);
}

void VirtualShadowsEffect::_invalidate(uint32_t p_slot, const LocalVector<AABB> &p_invalidate) {
	Slot &slot = slots[p_slot];
	for (const AABB &aabb : p_invalidate) {
		if (p_slot == 0) {
			Vector2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
			for (int c = 0; c < 8; c++) {
				const Vector2 l = _light_xy(slot, aabb.get_endpoint(c));
				lo = lo.min(l);
				hi = hi.max(l);
			}
			for (uint32_t level = 0; level < LEVELS; level++) {
				const float ps = _page_size(level);
				const int64_t x0 = int64_t(Math::floor(lo.x / ps)), x1 = int64_t(Math::floor(hi.x / ps));
				const int64_t y0 = int64_t(Math::floor(lo.y / ps)), y1 = int64_t(Math::floor(hi.y / ps));
				const uint32_t base = _slot_level(0, level) * ENTRIES_PER_LEVEL;
				if ((x1 - x0 + 1) * (y1 - y0 + 1) >= ENTRIES_PER_LEVEL) {
					for (uint32_t e = base; e < base + ENTRIES_PER_LEVEL; e++) {
						_free_entry(e);
					}
					continue;
				}
				for (int64_t y = y0; y <= y1; y++) {
					for (int64_t x = x0; x <= x1; x++) {
						const uint32_t e = base + uint32_t(y & (PAGES - 1)) * PAGES + uint32_t(x & (PAGES - 1));
						if (entry_valid[e] && entry_key[e] == _key(int32_t(x), int32_t(y))) {
							_free_entry(e);
						}
					}
				}
			}
		} else {
			bool all = false;
			const Rect2 ndc = _spot_ndc_rect(slot, aabb, all);
			for (uint32_t mip = 0; mip < SPOT_MIPS; mip++) {
				const int32_t pages = PAGES >> mip;
				const uint32_t base = _slot_level(p_slot, mip) * ENTRIES_PER_LEVEL;
				int32_t x0 = 0, x1 = pages - 1, y0 = 0, y1 = pages - 1;
				if (!all) {
					x0 = CLAMP(int32_t(Math::floor((ndc.position.x * 0.5f + 0.5f) * pages)), 0, pages - 1);
					x1 = CLAMP(int32_t(Math::floor((ndc.get_end().x * 0.5f + 0.5f) * pages)), 0, pages - 1);
					y0 = CLAMP(int32_t(Math::floor((ndc.position.y * 0.5f + 0.5f) * pages)), 0, pages - 1);
					y1 = CLAMP(int32_t(Math::floor((ndc.get_end().y * 0.5f + 0.5f) * pages)), 0, pages - 1);
					if (ndc.get_end().x < -1 || ndc.position.x > 1 || ndc.get_end().y < -1 || ndc.position.y > 1) {
						continue;
					}
				}
				for (int32_t y = y0; y <= y1; y++) {
					for (int32_t x = x0; x <= x1; x++) {
						_free_entry(base + uint32_t(y) * PAGES + uint32_t(x));
					}
				}
			}
		}
	}
}

void VirtualShadowsEffect::_plan_windows(uint32_t p_slot) {
	if (!requests_ready) {
		return;
	}
	Slot &slot = slots[p_slot];
	// Requested pages of this slot. Directional pages get absolute coordinates by unwrapping around the camera position
	// at marking time; spot pages are keyed by the slot version.
	struct Need {
		uint32_t entry;
		uint32_t level;
		int32_t x, y;
		uint32_t key;
	};
	LocalVector<Need> needs;
	const uint32_t levels = p_slot == 0 ? LEVELS : SPOT_MIPS;
	const Vector2 cam = p_slot == 0 ? _light_xy(slot, request_camera) : Vector2();
	for (uint32_t level = 0; level < levels; level++) {
		const uint32_t first_word = _slot_level(p_slot, level) * ENTRIES_PER_LEVEL / 32;
		for (uint32_t w = first_word; w < first_word + ENTRIES_PER_LEVEL / 32; w++) {
			uint32_t bits = requests[w];
			while (bits) {
				const uint32_t b = __builtin_ctz(bits);
				bits &= bits - 1;
				const uint32_t e = w * 32 + b;
				const uint32_t wrapped_x = e % PAGES, wrapped_y = (e / PAGES) % PAGES;
				int32_t x = wrapped_x, y = wrapped_y;
				uint32_t key = slot.version;
				if (p_slot == 0) {
					const float ps = _page_size(level);
					const int32_t ox = int32_t(Math::floor(cam.x / ps)) - PAGES / 2, oy = int32_t(Math::floor(cam.y / ps)) - PAGES / 2;
					x = ox + ((int32_t(wrapped_x) - ox) & (PAGES - 1));
					y = oy + ((int32_t(wrapped_y) - oy) & (PAGES - 1));
					key = _key(x, y);
				} else if (x >= (PAGES >> level) || y >= (PAGES >> level)) {
					continue;
				}
				stats.requested++;
				if (entry_valid[e] && entry_key[e] == key) {
					phys_last_used.write[table[e * 2] >> 1] = frame;
				} else {
					needs.push_back({ e, level, x, y, key });
				}
			}
		}
	}
	stats.missing += needs.size();

	// Windows with the most missing pages first, within the shared frame budget.
	HashMap<uint64_t, uint32_t> window_count;
	auto window_key = [](const Need &n) {
		return (uint64_t(n.level) << 48) | (uint64_t(uint32_t(n.x >> 3) & 0xFFFF) << 16) | (uint32_t(n.y >> 3) & 0xFFFF);
	};
	for (const Need &n : needs) {
		window_count[window_key(n)]++;
	}
	stats.windows_needed += window_count.size();
	LocalVector<Pair<uint32_t, uint64_t>> order;
	for (const KeyValue<uint64_t, uint32_t> &kv : window_count) {
		order.push_back(Pair<uint32_t, uint64_t>(kv.value, kv.key));
	}
	order.sort_custom<PairSort<uint32_t, uint64_t>>();
	HashMap<uint64_t, uint32_t> window_index;
	for (int64_t i = int64_t(order.size()) - 1; i >= 0 && windows.size() < windows_per_frame; i--) {
		Window win;
		win.slot = p_slot;
		win.level = uint32_t(order[i].second >> 48);
		win.wx = int32_t(int16_t((order[i].second >> 16) & 0xFFFF));
		win.wy = int32_t(int16_t(order[i].second & 0xFFFF));
		const uint32_t index = windows.size();
		win.rect = Rect2i((index % scratch_cols) * WINDOW_TEXELS, (index / scratch_cols) * WINDOW_TEXELS, WINDOW_TEXELS, WINDOW_TEXELS);
		window_index[order[i].second] = index;
		windows.push_back(win);
	}
	// Coarse pages first: when the pool runs out, every pixel still finds a coarser page to fall back to.
	struct NeedSort {
		bool operator()(const Need &a, const Need &b) const { return a.level > b.level; }
	};
	needs.sort_custom<NeedSort>();
	for (const Need &n : needs) {
		const uint32_t *index = window_index.getptr(window_key(n));
		if (!index) {
			continue;
		}
		const int32_t phys = _alloc_page();
		if (phys < 0) {
			stats.dropped++;
			continue;
		}
		_free_entry(n.entry);
		entry_valid.write[n.entry] = 1;
		entry_key.write[n.entry] = n.key;
		table.write[n.entry * 2] = (uint32_t(phys) << 1) | 1u;
		table.write[n.entry * 2 + 1] = n.key;
		phys_entry.write[phys] = n.entry;
		phys_last_used.write[phys] = frame;
		table_dirty = true;
		level_dirty[_slot_level(p_slot, n.level)] = true;
		const Window &win = windows[*index];
		copies.push_back(win.rect.position.x + (n.x - win.wx * WINDOW_PAGES) * TEXELS);
		copies.push_back(win.rect.position.y + (n.y - win.wy * WINDOW_PAGES) * TEXELS); // Window row 0 is the smallest y.
		copies.push_back((uint32_t(phys) % pool_cols) * TEXELS);
		copies.push_back((uint32_t(phys) / pool_cols) * TEXELS);
		stats.rendered++;
	}
}

Basis VirtualShadowsEffect::omni_face_basis(int p_face) {
	Vector3 f;
	f[p_face / 2] = (p_face & 1) ? -1.0f : 1.0f;
	const Vector3 b = -f;
	const Vector3 u = Math::abs(f.y) > 0.5f ? Vector3(0, 0, 1) : Vector3(0, 1, 0);
	return Basis(u.cross(b), u, b);
}

bool VirtualShadowsEffect::plan_light(RID p_light_instance, bool p_directional, bool p_omni, const Transform3D &p_transform, const Projection &p_projection, float p_normal_bias, const LocalVector<AABB> &p_invalidate) {
	if (!enabled) {
		return false;
	}
	const uint32_t count = p_omni ? 6 : 1;
	int32_t s = _slot_of(p_light_instance);
	if (s < 0) {
		if (p_directional) {
			if (slots[0].valid) {
				return false; // Only the first shadowed directional light.
			}
			s = 0;
		} else {
			for (uint32_t i = 1; i + count - 1 <= SPOT_SLOTS && s < 0; i++) {
				bool free = true;
				for (uint32_t c = 0; c < count && free; c++) {
					free = !slots[i + c].valid;
				}
				if (free) {
					s = i;
				}
			}
			if (s < 0) {
				return false;
			}
		}
		for (uint32_t c = 0; c < count; c++) {
			_free_slot(s + c);
		}
	}
	uint32_t version = 0;
	for (uint32_t c = 0; c < count; c++) {
		Slot &slot = slots[s + c];
		if (p_directional) {
			_plan_directional(slot, 0, p_transform, p_projection.get_z_far());
		} else if (p_omni) {
			_plan_spot(slot, Transform3D(p_transform.basis * omni_face_basis(c), p_transform.origin), p_projection);
			slot.light_transform = p_transform;
		} else {
			_plan_spot(slot, p_transform, p_projection);
		}
		slot.valid = true;
		slot.light = p_light_instance;
		slot.last_seen = frame;
		slot.normal_bias = p_normal_bias;
		version = MAX(version, slot.version);
	}
	for (uint32_t c = 0; c < count; c++) {
		// The faces of an omni light share one page key, so a reset of any face has to bump all of them.
		slots[s + c].version = version;
		_invalidate(s + c, p_invalidate);
		_plan_windows(s + c);
	}
	return true;
}

void VirtualShadowsEffect::get_windows(RID p_light_instance, LocalVector<const Window *> &r_windows) const {
	for (const Window &w : windows) {
		if (slots[w.slot].light == p_light_instance) {
			r_windows.push_back(&w);
		}
	}
}

Projection VirtualShadowsEffect::get_window_projection(const Window &p_window) const {
	const Slot &slot = slots[p_window.slot];
	Projection p;
	if (p_window.slot == 0) {
		const float half = _page_size(p_window.level) * WINDOW_PAGES * 0.5f;
		p.set_orthogonal(-half, half, -half, half, 0, slot.z_range);
	} else {
		// The part of the light frustum under this window, on the near plane.
		const Vector2 he = slot.projection.get_viewport_half_extents();
		const float pages = float(PAGES >> p_window.level);
		const float u0 = float(p_window.wx * WINDOW_PAGES) / pages * 2.0f - 1.0f, u1 = float((p_window.wx + 1) * WINDOW_PAGES) / pages * 2.0f - 1.0f;
		const float v0 = float(p_window.wy * WINDOW_PAGES) / pages * 2.0f - 1.0f, v1 = float((p_window.wy + 1) * WINDOW_PAGES) / pages * 2.0f - 1.0f;
		p.set_frustum(u0 * he.x, u1 * he.x, v0 * he.y, v1 * he.y, slot.projection.get_z_near(), slot.projection.get_z_far());
	}
	return p;
}

Transform3D VirtualShadowsEffect::get_window_transform(const Window &p_window) const {
	const Slot &slot = slots[p_window.slot];
	if (p_window.slot != 0) {
		return slot.transform;
	}
	const float ps = _page_size(p_window.level);
	const Vector2 center = Vector2(p_window.wx, p_window.wy) * ps * WINDOW_PAGES + Vector2(1, 1) * ps * WINDOW_PAGES * 0.5f;
	return Transform3D(slot.basis, slot.basis.xform(Vector3(center.x, center.y, slot.z_origin)));
}

bool VirtualShadowsEffect::window_touches(const Window &p_window, const AABB &p_aabb) const {
	const Slot &slot = slots[p_window.slot];
	if (p_window.slot == 0) {
		Vector2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
		for (int c = 0; c < 8; c++) {
			const Vector2 l = _light_xy(slot, p_aabb.get_endpoint(c));
			lo = lo.min(l);
			hi = hi.max(l);
		}
		const float extent = _page_size(p_window.level) * WINDOW_PAGES;
		return Rect2(Vector2(p_window.wx, p_window.wy) * extent, Vector2(extent, extent)).intersects(Rect2(lo, hi - lo));
	}
	bool all = false;
	const Rect2 ndc = _spot_ndc_rect(slot, p_aabb, all);
	if (all) {
		return true;
	}
	const float pages = float(PAGES >> p_window.level);
	const float u0 = float(p_window.wx * WINDOW_PAGES) / pages * 2.0f - 1.0f, v0 = float(p_window.wy * WINDOW_PAGES) / pages * 2.0f - 1.0f;
	const float size = float(WINDOW_PAGES) / pages * 2.0f;
	return Rect2(u0, v0, size, size).intersects(ndc);
}

void VirtualShadowsEffect::copy_pages() {
	if (!enabled) {
		return;
	}
	if (OS::get_singleton()->is_stdout_verbose() && !windows.is_empty()) {
		print_line(vformat("Virtual shadows: frame %d planned windows %d rendered pages %d missing %d of %d requested (%d windows needed), %d pool pages free.", frame, windows.size(), stats.rendered, stats.missing, stats.requested, stats.windows_needed, free_list.size()));
	}
	if (!copies.is_empty()) {
		RD::get_singleton()->buffer_update(copy_buffer, 0, copies.size() * sizeof(uint32_t), copies.ptr());
		MaterialStorage *material_storage = MaterialStorage::get_singleton();
		RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		RID shader_rid = shader.version_get_shader(shader_version, MODE_COPY);
		RD::Uniform u_src(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { sampler, scratch_depth });
		RD::Uniform u_copies(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, copy_buffer);
		RD::Uniform u_pool(RD::UNIFORM_TYPE_IMAGE, 2, pool_texture);
		RID set = UniformSetCacheRD::get_singleton()->get_cache(shader_rid, 0, u_src, u_copies, u_pool);
		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_COPY]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, set, 0);
		RD::get_singleton()->compute_list_dispatch(compute_list, TEXELS / 8, TEXELS / 8, copies.size() / 4);
		RD::get_singleton()->compute_list_end();
	}
	if (debug_mode != 0 && !copies.is_empty() && !dumped) {
		dumped = true;
		_dump(scratch_depth, "user://vsm_scratch.png", 4);
		_dump(pool_texture, "user://vsm_pool.png", 4);
	}
	if (table_dirty) {
		table_dirty = false;
		for (uint32_t level = 0; level < TOTAL_LEVELS; level++) {
			if (level_dirty[level]) {
				level_dirty[level] = false;
				const uint64_t offset = uint64_t(level) * ENTRIES_PER_LEVEL * 2 * sizeof(uint32_t);
				RD::get_singleton()->buffer_update(page_table_buffer, offset, ENTRIES_PER_LEVEL * 2 * sizeof(uint32_t), table.ptr() + uint64_t(level) * ENTRIES_PER_LEVEL * 2);
			}
		}
	}
}

void VirtualShadowsEffect::_dump(RID p_texture, const String &p_path, int p_step) {
	RD::TextureFormat tf = RD::get_singleton()->texture_get_format(p_texture);
	Vector<uint8_t> data = RD::get_singleton()->texture_get_data(p_texture, 0);
	if (data.size() != int64_t(tf.width) * tf.height * 4) {
		return;
	}
	const float *src = reinterpret_cast<const float *>(data.ptr());
	const int w = tf.width / p_step, h = tf.height / p_step;
	Vector<uint8_t> px;
	px.resize(w * h);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			px.write[y * w + x] = uint8_t(CLAMP(src[(y * p_step) * tf.width + x * p_step] * 255.0f, 0.0f, 255.0f));
		}
	}
	Ref<Image> img = Image::create_from_data(w, h, false, Image::FORMAT_L8, px);
	img->save_png(p_path);
	print_line(vformat("Virtual shadows: dumped %s (%dx%d).", p_path, w, h));
}

void VirtualShadowsEffect::fill_directional_light_data(RID p_light_instance, bool p_shadows, float *r_light_view, float *r_depth_row, float *r_params, uint32_t *r_info) const {
	r_info[0] = r_info[1] = r_info[2] = r_info[3] = 0;
	const Slot &slot = slots[0];
	if (!enabled || !slot.valid || !p_shadows || p_light_instance != slot.light) {
		return;
	}
	MaterialStorage::store_transform(Transform3D(slot.basis, Vector3()).affine_inverse(), r_light_view);
	// The depth a window render writes for a world position: same z mapping for every window and level.
	Projection correction;
	correction.set_depth_correction(false);
	Projection ortho;
	ortho.set_orthogonal(-1, 1, -1, 1, 0, slot.z_range);
	Projection m = correction * ortho * Projection(Transform3D(slot.basis, slot.basis.xform(Vector3(0, 0, slot.z_origin))).affine_inverse());
	for (int c = 0; c < 4; c++) {
		r_depth_row[c] = m.columns[c][2];
	}
	r_params[0] = base_extent;
	r_params[1] = LEVELS;
	r_params[2] = float(smrt_rays);
	r_params[3] = slot.normal_bias;
	r_info[0] = 1;
	r_info[1] = 0;
	r_info[2] = pool_cols;
	r_info[3] = debug_mode;
}

void VirtualShadowsEffect::fill_omni_light_data(RID p_light_instance, bool p_shadows, const Transform3D &p_camera_inverse_transform, float p_pixel_scale, float *r_light_matrix, float *r_matrix, float *r_params, uint32_t *r_info) const {
	r_info[0] = r_info[1] = r_info[2] = r_info[3] = 0;
	const int32_t s = enabled && p_shadows ? _slot_of(p_light_instance) : -1;
	if (s <= 0) {
		return;
	}
	const Slot &slot = slots[s];
	Projection correction;
	correction.set_depth_correction(false);
	// View space to light space (the atlas is not used for this light, so its shadow_matrix carries it) and face space
	// to the face's clip space; the shader picks the face from the light-local position.
	MaterialStorage::store_transform((p_camera_inverse_transform * slot.light_transform).affine_inverse(), r_light_matrix);
	MaterialStorage::store_camera(correction * slot.projection, r_matrix);
	r_params[0] = slot.projection.get_viewport_half_extents().x / slot.projection.get_z_near();
	r_params[1] = SPOT_MIPS;
	r_params[2] = slot.normal_bias;
	r_params[3] = p_pixel_scale;
	r_info[0] = 2u | (smrt_rays << 8);
	r_info[1] = _slot_level(s, 0) * ENTRIES_PER_LEVEL;
	r_info[2] = pool_cols;
	r_info[3] = slot.version;
}

void VirtualShadowsEffect::fill_spot_light_data(RID p_light_instance, bool p_shadows, const Transform3D &p_camera_inverse_transform, const Transform3D &p_light_transform, float p_pixel_scale, float *r_matrix, float *r_params, uint32_t *r_info) const {
	r_info[0] = r_info[1] = r_info[2] = r_info[3] = 0;
	const int32_t s = enabled && p_shadows ? _slot_of(p_light_instance) : -1;
	if (s <= 0) {
		return;
	}
	const Slot &slot = slots[s];
	Projection correction;
	correction.set_depth_correction(false);
	// View space to the light's clip space, as the window renders see it.
	Projection m = correction * slot.projection * Projection((p_camera_inverse_transform * p_light_transform).affine_inverse());
	MaterialStorage::store_camera(m, r_matrix);
	r_params[0] = slot.projection.get_viewport_half_extents().x / slot.projection.get_z_near();
	r_params[1] = SPOT_MIPS;
	r_params[2] = slot.normal_bias;
	r_params[3] = p_pixel_scale;
	r_info[0] = 1u | (smrt_rays << 8);
	r_info[1] = _slot_level(s, 0) * ENTRIES_PER_LEVEL;
	r_info[2] = pool_cols;
	r_info[3] = slot.version;
}

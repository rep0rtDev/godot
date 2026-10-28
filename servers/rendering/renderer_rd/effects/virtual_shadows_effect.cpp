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
#include "core/templates/hash_map.h"
#include "core/templates/pair.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

#include <cfloat>

using namespace RendererRD;

VirtualShadowsEffect *VirtualShadowsEffect::singleton = nullptr;

VirtualShadowsEffect::VirtualShadowsEffect() {
	singleton = this;
	enabled = GLOBAL_GET("rendering/virtual_shadows/enabled");
	// Something valid to bind when the feature is off.
	page_table_buffer = RD::get_singleton()->storage_buffer_create(16);
	if (!enabled) {
		return;
	}
	debug_mode = GLOBAL_GET("rendering/virtual_shadows/debug/mode");
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
	mark_params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(float) * (16 * 3 + 4 + 4));
	phys_entry.resize(pool_pages);
	phys_entry.fill(UINT32_MAX);
	phys_last_used.resize_initialized(pool_pages);
	for (int32_t i = int32_t(pool_pages) - 1; i >= 0; i--) {
		free_list.push_back(i);
	}
	print_verbose(vformat("Virtual shadows: pool of %d pages (%d MB), %d windows per frame.", pool_pages, uint64_t(pool_pages) * TEXELS * TEXELS * 4 / (1024 * 1024), windows_per_frame));
}

VirtualShadowsEffect::~VirtualShadowsEffect() {
	for (RID rid : { pool_texture, scratch_fb, scratch_depth, page_table_buffer, request_buffer, copy_buffer, mark_params_buffer }) {
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
	if (!enabled || !light_valid) {
		return;
	}
	struct Params {
		float inv_projection[16];
		float inv_view[16];
		float light_view[16];
		float params[4];
		int32_t size[4];
	} params;
	Projection correction;
	correction.set_depth_correction(p_scene_data.flip_y);
	MaterialStorage::store_camera((correction * p_scene_data.cam_projection).inverse(), params.inv_projection);
	MaterialStorage::store_transform(p_scene_data.cam_transform, params.inv_view);
	MaterialStorage::store_transform(Transform3D(basis, Vector3()).affine_inverse(), params.light_view);
	params.params[0] = base_extent;
	params.params[1] = LEVELS;
	params.params[2] = p_scene_data.cam_projection.columns[1][1];
	params.params[3] = p_size.height;
	params.size[0] = p_size.width;
	params.size[1] = p_size.height;
	params.size[2] = params.size[3] = 0;
	RD::get_singleton()->buffer_update(mark_params_buffer, 0, sizeof(Params), &params);

	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rid = shader.version_get_shader(shader_version, MODE_MARK);
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { sampler, p_depth });
	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 1, mark_params_buffer);
	RD::Uniform u_requests(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, request_buffer);
	RID set = UniformSetCacheRD::get_singleton()->get_cache(shader_rid, 0, u_depth, u_params, u_requests);

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

void VirtualShadowsEffect::_invalidate_all() {
	for (uint32_t i = 0; i < ENTRIES; i++) {
		_free_entry(i);
	}
}

int32_t VirtualShadowsEffect::_alloc_page() {
	if (!free_list.is_empty()) {
		const uint32_t p = free_list[free_list.size() - 1];
		free_list.remove_at(free_list.size() - 1);
		return p;
	}
	int32_t victim = -1;
	for (uint32_t i = 0; i < pool_pages; i++) {
		if (phys_last_used[i] >= frame) {
			continue; // Wanted this frame.
		}
		if (victim < 0 || phys_last_used[i] < phys_last_used[victim]) {
			victim = i;
		}
	}
	if (victim < 0) {
		return -1;
	}
	_free_entry(phys_entry[victim]);
	free_list.remove_at(free_list.size() - 1);
	return victim;
}

bool VirtualShadowsEffect::plan_light(RID p_light_instance, const Transform3D &p_cascade_transform, float p_cascade_zfar, float p_normal_bias, const LocalVector<AABB> &p_invalidate) {
	if (!enabled) {
		return false;
	}
	if (light.is_null() || !light_valid) {
		light = p_light_instance;
	}
	if (light != p_light_instance) {
		return false;
	}
	normal_bias = p_normal_bias;

	// Light frame: the cascade basis, world anchored. Depth range: a snapped window around what the cascade needs, so
	// that cached pages stay comparable while the camera moves.
	const Basis new_basis = p_cascade_transform.basis;
	bool reset = !light_valid;
	for (int i = 0; i < 3 && !reset; i++) {
		reset = !new_basis.get_column(i).is_equal_approx(basis.get_column(i));
	}
	const float z_max = new_basis.get_column(2).dot(p_cascade_transform.origin);
	const float z_min = z_max - p_cascade_zfar;
	const float snap = 128.0f;
	if (reset || z_max > z_origin || z_min < z_origin - z_range) {
		basis = new_basis;
		z_origin = Math::ceil(z_max / snap) * snap + snap;
		z_range = Math::ceil((z_origin - z_min) / snap) * snap + snap;
		_invalidate_all();
		light_valid = true;
	}

	// Casters that moved, appeared or disappeared: every page their bounds touch is rendered again.
	for (const AABB &aabb : p_invalidate) {
		const Rect2 r = get_light_rect(aabb);
		const Vector2 lo = r.position, hi = r.get_end();
		for (uint32_t level = 0; level < LEVELS; level++) {
			const float ps = _page_size(level);
			const int64_t x0 = int64_t(Math::floor(lo.x / ps)), x1 = int64_t(Math::floor(hi.x / ps));
			const int64_t y0 = int64_t(Math::floor(lo.y / ps)), y1 = int64_t(Math::floor(hi.y / ps));
			if ((x1 - x0 + 1) * (y1 - y0 + 1) >= ENTRIES_PER_LEVEL) {
				for (uint32_t e = level * ENTRIES_PER_LEVEL; e < (level + 1) * ENTRIES_PER_LEVEL; e++) {
					_free_entry(e);
				}
				continue;
			}
			for (int64_t y = y0; y <= y1; y++) {
				for (int64_t x = x0; x <= x1; x++) {
					const uint32_t e = level * ENTRIES_PER_LEVEL + uint32_t(y & (PAGES - 1)) * PAGES + uint32_t(x & (PAGES - 1));
					if (entry_valid[e] && entry_key[e] == _key(int32_t(x), int32_t(y))) {
						_free_entry(e);
					}
				}
			}
		}
	}

	if (!have_requests) {
		return true;
	}

	// Requested pages: absolute coordinates come from unwrapping around the camera position at marking time.
	struct Need {
		uint32_t entry;
		uint32_t level;
		int32_t x, y;
	};
	LocalVector<Need> needs;
	const Vector2 cam = _light_xy(request_camera);
	for (uint32_t w = 0; w < uint32_t(requests.size()); w++) {
		uint32_t bits = requests[w];
		while (bits) {
			const uint32_t b = __builtin_ctz(bits);
			bits &= bits - 1;
			const uint32_t e = w * 32 + b;
			const uint32_t level = e / ENTRIES_PER_LEVEL;
			const uint32_t wrapped_x = e % PAGES, wrapped_y = (e / PAGES) % PAGES;
			const float ps = _page_size(level);
			const int32_t ox = int32_t(Math::floor(cam.x / ps)) - PAGES / 2, oy = int32_t(Math::floor(cam.y / ps)) - PAGES / 2;
			const int32_t x = ox + ((int32_t(wrapped_x) - ox) & (PAGES - 1));
			const int32_t y = oy + ((int32_t(wrapped_y) - oy) & (PAGES - 1));
			stats.requested++;
			if (entry_valid[e] && entry_key[e] == _key(x, y)) {
				phys_last_used.write[table[e * 2] >> 1] = frame;
			} else {
				needs.push_back({ e, level, x, y });
			}
		}
	}
	stats.missing = needs.size();

	// Windows with the most missing pages first.
	HashMap<uint64_t, uint32_t> window_count;
	for (const Need &n : needs) {
		const uint64_t k = (uint64_t(n.level) << 48) | (uint64_t(uint32_t(n.x >> 3)) << 16) | uint32_t(n.y >> 3) & 0xFFFF;
		window_count[k]++;
	}
	stats.windows_needed = window_count.size();
	LocalVector<Pair<uint32_t, uint64_t>> order;
	for (const KeyValue<uint64_t, uint32_t> &kv : window_count) {
		order.push_back(Pair<uint32_t, uint64_t>(kv.value, kv.key));
	}
	order.sort_custom<PairSort<uint32_t, uint64_t>>();
	HashMap<uint64_t, uint32_t> window_index;
	for (int64_t i = int64_t(order.size()) - 1; i >= 0 && windows.size() < windows_per_frame; i--) {
		Window win;
		win.level = uint32_t(order[i].second >> 48);
		win.wx = int32_t(int16_t((order[i].second >> 16) & 0xFFFF));
		win.wy = int32_t(int16_t(order[i].second & 0xFFFF));
		const uint32_t slot = windows.size();
		win.rect = Rect2i((slot % scratch_cols) * WINDOW_TEXELS, (slot / scratch_cols) * WINDOW_TEXELS, WINDOW_TEXELS, WINDOW_TEXELS);
		window_index[order[i].second] = slot;
		windows.push_back(win);
	}
	for (const Need &n : needs) {
		const uint64_t k = (uint64_t(n.level) << 48) | (uint64_t(uint32_t(n.x >> 3)) << 16) | uint32_t(n.y >> 3) & 0xFFFF;
		const uint32_t *slot = window_index.getptr(k);
		if (!slot) {
			continue;
		}
		const int32_t phys = _alloc_page();
		if (phys < 0) {
			break;
		}
		_free_entry(n.entry);
		entry_valid.write[n.entry] = 1;
		entry_key.write[n.entry] = _key(n.x, n.y);
		table.write[n.entry * 2] = (uint32_t(phys) << 1) | 1u;
		table.write[n.entry * 2 + 1] = entry_key[n.entry];
		phys_entry.write[phys] = n.entry;
		phys_last_used.write[phys] = frame;
		table_dirty = true;
		level_dirty[n.level] = true;
		const Window &win = windows[*slot];
		copies.push_back(win.rect.position.x + (n.x - win.wx * WINDOW_PAGES) * TEXELS);
		copies.push_back(win.rect.position.y + (n.y - win.wy * WINDOW_PAGES) * TEXELS); // Window row 0 is the smallest light-space y.
		copies.push_back((uint32_t(phys) % pool_cols) * TEXELS);
		copies.push_back((uint32_t(phys) / pool_cols) * TEXELS);
		stats.rendered++;
	}
	have_requests = false;
	return true;
}

Rect2 VirtualShadowsEffect::get_light_rect(const AABB &p_aabb) const {
	Vector2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
	for (int c = 0; c < 8; c++) {
		const Vector2 l = _light_xy(p_aabb.get_endpoint(c));
		lo = lo.min(l);
		hi = hi.max(l);
	}
	return Rect2(lo, hi - lo);
}

Rect2 VirtualShadowsEffect::get_window_light_rect(const Window &p_window) const {
	const float extent = _page_size(p_window.level) * WINDOW_PAGES;
	return Rect2(Vector2(p_window.wx, p_window.wy) * extent, Vector2(extent, extent));
}

Projection VirtualShadowsEffect::get_window_projection(const Window &p_window) const {
	const float half = _page_size(p_window.level) * WINDOW_PAGES * 0.5f;
	Projection p;
	p.set_orthogonal(-half, half, -half, half, 0, z_range);
	return p;
}

Transform3D VirtualShadowsEffect::get_window_transform(const Window &p_window) const {
	const float ps = _page_size(p_window.level);
	const Vector2 center = Vector2(p_window.wx, p_window.wy) * ps * WINDOW_PAGES + Vector2(1, 1) * ps * WINDOW_PAGES * 0.5f;
	return Transform3D(basis, basis.xform(Vector3(center.x, center.y, z_origin)));
}

void VirtualShadowsEffect::copy_pages() {
	if (!enabled) {
		return;
	}
	if (OS::get_singleton()->is_stdout_verbose() && !windows.is_empty()) {
		print_line(vformat("Virtual shadows: frame %d light %s valid %d requests %d/%d pending %d planned windows %d rendered pages %d missing %d of %d requested (%d windows needed), %d pool pages free.", frame, light.is_valid() ? "set" : "none", int(light_valid), int(have_requests), requests.size(), int(requests_pending), windows.size(), stats.rendered, stats.missing, stats.requested, stats.windows_needed, free_list.size()));
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
		for (uint32_t level = 0; level < LEVELS; level++) {
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
		ERR_PRINT(vformat("Virtual shadows: unexpected texture data size %d for %dx%d.", data.size(), tf.width, tf.height));
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

void VirtualShadowsEffect::fill_light_data(RID p_light_instance, bool p_shadows, float *r_light_view, float *r_depth_row, float *r_params, uint32_t *r_info) const {
	r_info[0] = r_info[1] = r_info[2] = r_info[3] = 0;
	if (!enabled || !light_valid || !p_shadows || p_light_instance != light) {
		return;
	}
	MaterialStorage::store_transform(Transform3D(basis, Vector3()).affine_inverse(), r_light_view);
	// The depth a window render writes for a world position: same z mapping for every window and level.
	Projection correction;
	correction.set_depth_correction(false);
	Projection ortho;
	ortho.set_orthogonal(-1, 1, -1, 1, 0, z_range);
	Projection m = correction * ortho * Projection(Transform3D(basis, basis.xform(Vector3(0, 0, z_origin))).affine_inverse());
	for (int c = 0; c < 4; c++) {
		r_depth_row[c] = m.columns[c][2];
	}
	r_params[0] = base_extent;
	r_params[1] = LEVELS;
	r_params[2] = 0.0f;
	r_params[3] = normal_bias;
	r_info[0] = 1;
	r_info[1] = 0;
	r_info[2] = pool_cols;
	r_info[3] = debug_mode;
}

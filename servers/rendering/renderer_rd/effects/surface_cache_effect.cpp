/**************************************************************************/
/*  surface_cache_effect.cpp                                              */
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

#include "surface_cache_effect.h"

#include "core/config/project_settings.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/rendering_server.h"

using namespace RendererRD;

SurfaceCacheEffect *SurfaceCacheEffect::singleton = nullptr;

SurfaceCacheEffect *SurfaceCacheEffect::get_singleton() {
	return singleton;
}

SurfaceCacheEffect::SurfaceCacheEffect() {
	singleton = this;
	enabled = GLOBAL_GET("rendering/global_illumination/surface_cache/enabled");
	if (!enabled) {
		return;
	}
	debug_mode = GLOBAL_GET("rendering/global_illumination/surface_cache/debug/mode");
	light_debug = debug_mode == 7 ? 1 : (debug_mode == 10 ? 2 : (debug_mode == 11 || debug_mode == 14 ? 3 : 0));
	screen_probes = GLOBAL_GET("rendering/global_illumination/surface_cache/screen_probes");
	atlas_size = GLOBAL_GET("rendering/global_illumination/surface_cache/atlas_size");
	cards_per_frame = GLOBAL_GET("rendering/global_illumination/surface_cache/cards_per_frame");
	texel_density = GLOBAL_GET("rendering/global_illumination/surface_cache/texels_per_meter");
	max_card_size = GLOBAL_GET("rendering/global_illumination/surface_cache/max_card_size");

	RD::TextureFormat tf;
	tf.width = atlas_size;
	tf.height = atlas_size;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	albedo = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(albedo, "Surface cache albedo");
	normal = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(normal, "Surface cache normal");
	orm = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(orm, "Surface cache orm");
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	emission = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(emission, "Surface cache emission");
	tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	depth = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(depth, "Surface cache depth");
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	direct = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(direct, "Surface cache direct light");
	final = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(final, "Surface cache final light");
	{
		RD::TextureFormat ptf = tf;
		ptf.width = atlas_size / 4;
		ptf.height = atlas_size / 4;
		ptf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		probes = RD::get_singleton()->texture_create(ptf, RD::TextureView());
		RD::get_singleton()->set_resource_name(probes, "Surface cache probes");
	}
	tf.format = RD::get_singleton()->texture_is_format_supported_for_usage(RD::DATA_FORMAT_D16_UNORM, RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? RD::DATA_FORMAT_D16_UNORM : RD::DATA_FORMAT_X8_D24_UNORM_PACK32;
	tf.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	depth_stencil = RD::get_singleton()->texture_create(tf, RD::TextureView());

	Vector<RID> fb = { albedo, normal, orm, emission, depth, depth_stencil };
	framebuffer = RD::get_singleton()->framebuffer_create(fb);

	capture_list.set_page_pool(&capture_pool);
	// Clear once so untouched atlas texels read as empty.
	for (RID t : { albedo, normal, orm, emission, depth, direct, final, probes }) {
		RD::get_singleton()->texture_clear(t, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}

	Vector<String> modes = { "\n#define MODE_DIRECT\n", "\n#define MODE_PROBES\n", "\n#define MODE_RESOLVE\n", "\n#define MODE_SCREEN_PROBES\n", "\n#define MODE_SCREEN_RESOLVE\n", "\n#define MODE_SCREEN_REFLECT\n" };
	light_shader.initialize(modes);
	light_shader_version = light_shader.version_create();
	for (int i = 0; i < LIGHT_MODE_MAX; i++) {
		light_pipelines[i] = RD::get_singleton()->compute_pipeline_create(light_shader.version_get_shader(light_shader_version, i));
	}
	light_params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(float) * 16 + sizeof(uint32_t) * 4 + sizeof(float) * 8);
	grid_buffer = RD::get_singleton()->storage_buffer_create(GRID * GRID * GRID * GRID_SLOTS * sizeof(uint32_t));
	screen_params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(float) * (32 + 8));
	sdf_params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(float) * 4 * 12);
	{
		RD::TextureFormat stf;
		stf.width = 1;
		stf.height = 1;
		stf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		stf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		dummy_sky_tex = RD::get_singleton()->texture_create(stf, RD::TextureView());
		RD::get_singleton()->texture_clear(dummy_sky_tex, Color(0, 0, 0, 1), 0, 1, 0, 1);
	}
	{
		RD::TextureFormat dtf;
		dtf.texture_type = RD::TEXTURE_TYPE_3D;
		dtf.width = 1;
		dtf.height = 1;
		dtf.depth = 1;
		dtf.format = RD::DATA_FORMAT_R8_UNORM;
		dtf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT;
		dtf.usage_bits |= RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		dummy_sdf_tex = RD::get_singleton()->texture_create(dtf, RD::TextureView());
		RD::get_singleton()->texture_clear(dummy_sdf_tex, Color(1, 1, 1, 1), 0, 1, 0, 1);
	}
}

SurfaceCacheEffect::~SurfaceCacheEffect() {
	capture_list.reset();
	if (light_shader_version.is_valid()) {
		light_shader.version_free(light_shader_version);
	}
	for (RID rid : { framebuffer, depth_stencil, depth, direct, final, probes, emission, orm, normal, albedo, card_buffer, instance_buffer, grid_buffer, light_params_buffer, sdf_params_buffer, dummy_sdf_tex, dummy_sky_tex, screen_probe_texture, reflection_history, screen_params_buffer }) {
		if (rid.is_valid()) {
			RD::get_singleton()->free_rid(rid);
		}
	}
	singleton = nullptr;
}

bool SurfaceCacheEffect::_allocate_rect(const Size2i &p_size, Rect2i &r_rect) {
	for (uint32_t i = 0; i < free_rects.size(); i++) {
		if (free_rects[i].size == p_size) {
			r_rect = free_rects[i];
			free_rects.remove_at_unordered(i);
			return true;
		}
	}
	for (Shelf &s : shelves) {
		if (s.height == p_size.height && s.next_x + p_size.width <= atlas_size) {
			r_rect = Rect2i(s.next_x, s.y, p_size.width, p_size.height);
			s.next_x += p_size.width;
			return true;
		}
	}
	if (shelf_next_y + p_size.height > atlas_size) {
		return false;
	}
	Shelf s;
	s.y = shelf_next_y;
	s.height = p_size.height;
	s.next_x = p_size.width;
	shelf_next_y += p_size.height;
	shelves.push_back(s);
	r_rect = Rect2i(0, s.y, p_size.width, p_size.height);
	return true;
}

void SurfaceCacheEffect::_free_rect(const Rect2i &p_rect) {
	free_rects.push_back(p_rect);
}

Size2i SurfaceCacheEffect::_card_size(const Vector3 &p_extent_world, int p_axis) const {
	int a = p_axis / 2;
	int u = (a + 1) % 3;
	int v = (a + 2) % 3;
	Size2i size;
	size.width = Math::next_power_of_2(uint32_t(Math::ceil(p_extent_world[u] * texel_density)));
	size.height = Math::next_power_of_2(uint32_t(Math::ceil(p_extent_world[v] * texel_density)));
	size.width = CLAMP(size.width, min_card_size, max_card_size);
	size.height = CLAMP(size.height, min_card_size, max_card_size);
	return size;
}

void SurfaceCacheEffect::_card_view(const Instance &p_inst, int p_axis, Transform3D &r_transform, Projection &r_projection) const {
	int a = p_axis / 2;
	float sign = (p_axis & 1) ? -1.0f : 1.0f;
	int u = (a + 1) % 3;
	int v = (a + 2) % 3;
	const Basis &b = p_inst.transform.basis;
	Vector3 axis_u = b.get_column(u);
	Vector3 axis_v = b.get_column(v);
	Vector3 axis_n = b.get_column(a);
	float len_u = axis_u.length();
	float len_v = axis_v.length();
	float len_n = axis_n.length();
	if (len_u < CMP_EPSILON || len_v < CMP_EPSILON || len_n < CMP_EPSILON) {
		r_transform = Transform3D();
		r_projection = Projection();
		return;
	}
	axis_u /= len_u;
	axis_v /= len_v;
	axis_n /= len_n;
	Vector3 half = p_inst.aabb.size * 0.5f;
	Vector3 center = p_inst.transform.xform(p_inst.aabb.get_center());
	float hu = half[u] * len_u;
	float hv = half[v] * len_v;
	float hn = half[a] * len_n;
	const float margin = 0.01f + hn * 0.02f;
	// Camera looks along -sign*axis_n (its -Z), sitting just outside the face.
	Vector3 z = axis_n * sign;
	Vector3 x = axis_u;
	Vector3 y = z.cross(x);
	if (y.dot(axis_v) < 0.0f) {
		y = -y;
		x = y.cross(z);
	}
	r_transform.basis = Basis(x, y, z);
	r_transform.origin = center + z * (hn + margin);
	r_projection.set_orthogonal(-hu, hu, -hv, hv, 0.0f, 2.0f * (hn + margin));
}

void SurfaceCacheEffect::_build_cards(uint32_t p_instance) {
	Instance &inst = instances[p_instance];
	_drop_cards(p_instance);
	if (inst.first_card == 0xFFFFFFFF) {
		if (free_blocks.size()) {
			inst.first_card = free_blocks[free_blocks.size() - 1];
			free_blocks.resize(free_blocks.size() - 1);
		} else {
			inst.first_card = cards.size();
			cards.resize(cards.size() + 6);
		}
	}
	const Basis &b = inst.transform.basis;
	Vector3 extent(inst.aabb.size.x * b.get_column(0).length(), inst.aabb.size.y * b.get_column(1).length(), inst.aabb.size.z * b.get_column(2).length());
	for (int axis = 0; axis < 6; axis++) {
		Card &c = cards[inst.first_card + axis];
		c = Card();
		c.instance_id = p_instance;
		c.axis = axis;
		int a = axis / 2;
		// A face thinner than a texel in either direction is not worth a card.
		if (extent[(a + 1) % 3] * texel_density < 1.0f || extent[(a + 2) % 3] * texel_density < 1.0f) {
			continue;
		}
		if (!_allocate_rect(_card_size(extent, axis), c.rect)) {
			c.rect = Rect2i();
			continue;
		}
		_card_view(inst, axis, c.capture_transform, c.capture_projection);
	}
	grid_dirty = true;
}

void SurfaceCacheEffect::_drop_cards(uint32_t p_instance) {
	Instance &inst = instances[p_instance];
	if (inst.first_card == 0xFFFFFFFF) {
		return;
	}
	for (int axis = 0; axis < 6; axis++) {
		Card &c = cards[inst.first_card + axis];
		if (c.rect.size != Size2i()) {
			_free_rect(c.rect);
		}
		c.rect = Rect2i();
	}
	grid_dirty = true;
}

void SurfaceCacheEffect::_free_block(uint32_t p_instance) {
	Instance &inst = instances[p_instance];
	_drop_cards(p_instance);
	if (inst.first_card != 0xFFFFFFFF) {
		for (int axis = 0; axis < 6; axis++) {
			cards[inst.first_card + axis].instance_id = 0xFFFFFFFF;
		}
		free_blocks.push_back(inst.first_card);
		inst.first_card = 0xFFFFFFFF;
	}
}

void SurfaceCacheEffect::begin_frame(uint64_t p_frame, const PagedArray<RenderGeometryInstance *> &p_instances, const Vector3 &p_camera_position) {
	frame = p_frame;
	camera_position = p_camera_position;
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	for (uint64_t i = 0; i < p_instances.size(); i++) {
		RenderGeometryInstanceBase *gi = static_cast<RenderGeometryInstanceBase *>(p_instances[i]);
		if (gi->data->base_type != RSE::INSTANCE_MESH || gi->data->skeleton.is_valid()) {
			continue; // Multimesh, particles and skinned meshes are not cached yet.
		}
		uint32_t id;
		HashMap<RenderGeometryInstance *, uint32_t>::Iterator it = instance_map.find(gi);
		AABB aabb = mesh_storage->mesh_get_aabb(gi->data->base);
		if (it) {
			id = it->value;
			Instance &inst = instances[id];
			inst.last_seen = frame;
			if (inst.transform != gi->transform || inst.aabb != aabb) {
				inst.transform = gi->transform;
				inst.aabb = aabb;
				_build_cards(id);
			}
		} else {
			Instance inst;
			inst.geometry = gi;
			inst.transform = gi->transform;
			inst.aabb = aabb;
			inst.last_seen = frame;
			if (free_instances.size()) {
				id = free_instances[free_instances.size() - 1];
				free_instances.resize(free_instances.size() - 1);
				instances[id] = inst;
			} else {
				id = instances.size();
				instances.push_back(inst);
			}
			instance_map[gi] = id;
			_build_cards(id);
		}
	}
	// Drop instances not rendered for a while.
	LocalVector<RenderGeometryInstance *> stale;
	for (const KeyValue<RenderGeometryInstance *, uint32_t> &kv : instance_map) {
		if (frame - instances[kv.value].last_seen > unused_frames) {
			stale.push_back(kv.key);
		}
	}
	for (RenderGeometryInstance *g : stale) {
		uint32_t id = instance_map[g];
		_free_block(id);
		instances[id].geometry = nullptr;
		free_instances.push_back(id);
		instance_map.erase(g);
	}
	stats.instances = instance_map.size();
	stats.cards = 0;
	for (const Card &c : cards) {
		if (c.instance_id != 0xFFFFFFFF && c.rect.size != Size2i()) {
			stats.cards++;
		}
	}
	stats.captured = 0;
	stats.lit = 0;
	captured_this_frame.clear();
	stats.dirty = 0;
	stats.atlas_texels = 0;
	for (const Card &c : cards) {
		if (c.instance_id != 0xFFFFFFFF && c.rect.size != Size2i()) {
			stats.atlas_texels += c.rect.get_area();
			if (c.dirty) {
				stats.dirty++;
			}
		}
	}
}

void SurfaceCacheEffect::capture(CaptureFunction p_capture, void *p_userdata) {
	struct Pending {
		float distance;
		uint32_t card;
		bool operator<(const Pending &p) const { return distance < p.distance; }
	};
	LocalVector<Pending> pending;
	for (uint32_t i = 0; i < cards.size(); i++) {
		const Card &c = cards[i];
		if (c.instance_id == 0xFFFFFFFF || c.rect.size == Size2i() || !c.dirty) {
			continue;
		}
		Pending p;
		p.card = i;
		p.distance = c.capture_transform.origin.distance_squared_to(camera_position);
		pending.push_back(p);
	}
	pending.sort();
	uint32_t count = MIN(pending.size(), uint32_t(cards_per_frame));
	for (uint32_t i = 0; i < count; i++) {
		Card &c = cards[pending[i].card];
		Instance &inst = instances[c.instance_id];
		if (!inst.geometry) {
			continue;
		}
		capture_list.clear();
		capture_list.push_back(inst.geometry);
		p_capture(p_userdata, c.capture_transform, c.capture_projection, capture_list, framebuffer, c.rect);
		c.dirty = false;
		c.captured_frame = frame;
		stats.captured++;
		captured_this_frame.push_back(pending[i].card);
	}
	capture_list.clear();
}

void SurfaceCacheEffect::light_cards(RID p_directional_buffer, uint32_t p_directional_count, RID p_omni_buffer, uint32_t p_omni_count, RID p_spot_buffer, uint32_t p_spot_count, const Transform3D &p_view_to_world, const Vector<RID> &p_sdf_cascades, const Vector<Vector4> &p_sdf_offsets, uint32_t p_sdf_grid_size, float p_sdf_y_mult, const Vector3 &p_sky_radiance, const SkyInfo &p_sky) {
	// Cards to light: captured this frame plus every light_share-th resident card.
	LocalVector<uint32_t> todo;
	for (uint32_t id : captured_this_frame) {
		todo.push_back(id);
	}
	for (uint32_t i = light_round_robin; i < cards.size(); i += light_share) {
		const Card &c = cards[i];
		if (c.instance_id != 0xFFFFFFFF && c.rect.size != Size2i() && !c.dirty && c.captured_frame != frame) {
			todo.push_back(i);
		}
	}
	light_round_robin = (light_round_robin + 1) % light_share;
	if (todo.is_empty()) {
		return;
	}

	struct CardData {
		int32_t rect[4];
		float view_to_world[16];
		float world_to_view[16];
		float extents[4];
	};
	LocalVector<CardData> gpu_cards;
	gpu_cards.resize(cards.size());
	for (uint32_t i = 0; i < cards.size(); i++) {
		const Card &c = cards[i];
		CardData &d = gpu_cards[i];
		d.rect[0] = c.rect.position.x;
		d.rect[1] = c.rect.position.y;
		d.rect[2] = c.rect.size.x;
		d.rect[3] = c.rect.size.y;
		MaterialStorage::store_transform(c.capture_transform, d.view_to_world);
		MaterialStorage::store_transform(c.capture_transform.affine_inverse(), d.world_to_view);
		// Orthographic extents from the projection: x = 1/m00, y = 1/m11, depth range = zfar.
		d.extents[0] = c.capture_projection.columns[0][0] != 0.0f ? 1.0f / c.capture_projection.columns[0][0] : 0.0f;
		d.extents[1] = c.capture_projection.columns[1][1] != 0.0f ? 1.0f / c.capture_projection.columns[1][1] : 0.0f;
		d.extents[2] = c.capture_projection.get_z_far();
		d.extents[3] = 0.0f;
	}
	uint32_t needed = gpu_cards.size() * sizeof(CardData);
	bool sets_dirty = false;
	if (card_buffer.is_null() || card_buffer_size < needed) {
		if (card_buffer.is_valid()) {
			RD::get_singleton()->free_rid(card_buffer);
		}
		card_buffer_size = MAX(needed, 64u * uint32_t(sizeof(CardData)));
		card_buffer = RD::get_singleton()->storage_buffer_create(card_buffer_size);
		sets_dirty = true;
	}
	RD::get_singleton()->buffer_update(card_buffer, 0, needed, gpu_cards.ptr());

	struct InstanceData {
		float world_to_local[16];
		float aabb_min[4];
		float aabb_size[4];
		uint32_t info[4];
	};
	LocalVector<InstanceData> gpu_instances;
	gpu_instances.resize(MAX(instances.size(), 1u));
	for (uint32_t i = 0; i < instances.size(); i++) {
		const Instance &inst = instances[i];
		InstanceData &d = gpu_instances[i];
		MaterialStorage::store_transform(inst.transform.affine_inverse(), d.world_to_local);
		for (int k = 0; k < 3; k++) {
			d.aabb_min[k] = inst.aabb.position[k];
			d.aabb_size[k] = inst.aabb.size[k];
		}
		d.aabb_min[3] = d.aabb_size[3] = 0.0f;
		d.info[0] = inst.first_card;
		d.info[1] = (inst.geometry != nullptr && inst.first_card != 0xFFFFFFFF) ? 1 : 0;
		d.info[2] = d.info[3] = 0;
	}
	needed = gpu_instances.size() * sizeof(InstanceData);
	if (instance_buffer.is_null() || instance_buffer_size < needed) {
		if (instance_buffer.is_valid()) {
			RD::get_singleton()->free_rid(instance_buffer);
		}
		instance_buffer_size = MAX(needed, 64u * uint32_t(sizeof(InstanceData)));
		instance_buffer = RD::get_singleton()->storage_buffer_create(instance_buffer_size);
		sets_dirty = true;
	}
	RD::get_singleton()->buffer_update(instance_buffer, 0, needed, gpu_instances.ptr());

	// Object grid around the camera, snapped to cells so a still camera keeps it.
	Vector3 origin = ((camera_position - Vector3(1, 1, 1) * (grid_cell * GRID * 0.5f)) / grid_cell).floor() * grid_cell;
	if (grid_dirty || origin != grid_origin) {
		grid_origin = origin;
		grid_dirty = false;
		LocalVector<uint32_t> grid;
		grid.resize(GRID * GRID * GRID * GRID_SLOTS);
		for (uint32_t i = 0; i < grid.size(); i++) {
			grid[i] = 0xFFFFFFFF;
		}
		for (uint32_t i = 0; i < instances.size(); i++) {
			const Instance &inst = instances[i];
			if (inst.geometry == nullptr || inst.first_card == 0xFFFFFFFF) {
				continue;
			}
			AABB world = inst.transform.xform(inst.aabb).grow(grid_cell * 0.5f);
			Vector3i lo = Vector3i(((world.position - grid_origin) / grid_cell).floor());
			Vector3i hi = Vector3i(((world.position + world.size - grid_origin) / grid_cell).floor());
			lo = lo.maxi(0);
			hi = hi.mini(GRID - 1);
			for (int z = lo.z; z <= hi.z; z++) {
				for (int y = lo.y; y <= hi.y; y++) {
					for (int x = lo.x; x <= hi.x; x++) {
						uint32_t base = ((z * GRID + y) * GRID + x) * GRID_SLOTS;
						for (uint32_t s = 0; s < GRID_SLOTS; s++) {
							if (grid[base + s] == 0xFFFFFFFF) {
								grid[base + s] = i;
								break;
							}
						}
					}
				}
			}
		}
		RD::get_singleton()->buffer_update(grid_buffer, 0, grid.size() * sizeof(uint32_t), grid.ptr());
	}

	struct Params {
		float view_to_world[16];
		uint32_t counts[4];
		float grid[4];
		float sky[4];
	} params;
	MaterialStorage::store_transform(p_view_to_world, params.view_to_world);
	params.counts[0] = p_directional_count;
	params.counts[1] = p_omni_count;
	params.counts[2] = p_spot_count;
	params.counts[3] = p_sdf_cascades.size() > 0 ? 1 : 0;
	params.grid[0] = grid_origin.x;
	params.grid[1] = grid_origin.y;
	params.grid[2] = grid_origin.z;
	params.grid[3] = grid_cell;
	params.sky[0] = p_sky_radiance.x;
	params.sky[1] = p_sky_radiance.y;
	params.sky[2] = p_sky_radiance.z;
	params.sky[3] = float(frame % 4096);
	RD::get_singleton()->buffer_update(light_params_buffer, 0, sizeof(Params), &params);
	{
		float sdf_params[4 * 12] = {};
		sdf_params[0] = float(p_sdf_grid_size);
		sdf_params[1] = p_sdf_y_mult;
		sdf_params[2] = float(MIN(p_sdf_cascades.size(), 8));
		for (int i = 0; i < MIN(p_sdf_offsets.size(), 8); i++) {
			sdf_params[4 + i * 4 + 0] = p_sdf_offsets[i].x;
			sdf_params[4 + i * 4 + 1] = p_sdf_offsets[i].y;
			sdf_params[4 + i * 4 + 2] = p_sdf_offsets[i].z;
			sdf_params[4 + i * 4 + 3] = p_sdf_offsets[i].w;
		}
		sdf_params[36] = float(p_sky.mode);
		sdf_params[37] = p_sky.energy;
		sdf_params[38] = p_sky.border;
		sdf_params[39] = 1.0f - p_sky.border * 2.0f;
		sdf_params[40] = p_sky.color.x;
		sdf_params[41] = p_sky.color.y;
		sdf_params[42] = p_sky.color.z;
		sdf_params[44] = p_sky.orientation.x;
		sdf_params[45] = p_sky.orientation.y;
		sdf_params[46] = p_sky.orientation.z;
		sdf_params[47] = p_sky.orientation.w;
		RD::get_singleton()->buffer_update(sdf_params_buffer, 0, sizeof(sdf_params), sdf_params);
	}

	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	RID shader = light_shader.version_get_shader(light_shader_version, LIGHT_MODE_DIRECT);
	if (sets_dirty || light_uniform_set.is_null() || !RD::get_singleton()->uniform_set_is_valid(light_uniform_set)) {
		Vector<RD::Uniform> uniforms;
		auto add = [&](RD::UniformType p_type, int p_binding, RID p_rid) {
			RD::Uniform u;
			u.uniform_type = p_type;
			u.binding = p_binding;
			u.append_id(p_rid);
			uniforms.push_back(u);
		};
		add(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, card_buffer);
		add(RD::UNIFORM_TYPE_TEXTURE, 1, albedo);
		add(RD::UNIFORM_TYPE_TEXTURE, 2, normal);
		add(RD::UNIFORM_TYPE_TEXTURE, 3, depth);
		add(RD::UNIFORM_TYPE_TEXTURE, 4, emission);
		add(RD::UNIFORM_TYPE_IMAGE, 5, direct);
		add(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 6, p_directional_buffer); // Directional lights live in a UBO in LightStorage.
		add(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_omni_buffer);
		add(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, p_spot_buffer);
		add(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 9, light_params_buffer);
		add(RD::UNIFORM_TYPE_SAMPLER, 10, material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
		add(RD::UNIFORM_TYPE_SAMPLER, 11, material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
		add(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, instance_buffer);
		add(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, grid_buffer);
		add(RD::UNIFORM_TYPE_IMAGE, 14, probes);
		add(RD::UNIFORM_TYPE_IMAGE, 15, final);
		if (light_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(light_uniform_set)) {
			RD::get_singleton()->free_rid(light_uniform_set);
		}
		light_uniform_set = RD::get_singleton()->uniform_set_create(uniforms, shader, 0);
	}

	Vector<RID> sdf_textures;
	for (int i = 0; i < 8; i++) {
		sdf_textures.push_back(i < p_sdf_cascades.size() ? p_sdf_cascades[i] : dummy_sdf_tex);
	}
	RID sky_tex = p_sky.octmap.is_valid() ? p_sky.octmap : dummy_sky_tex;
	if (sdf_uniform_set.is_null() || !RD::get_singleton()->uniform_set_is_valid(sdf_uniform_set) || sdf_uniform_set_textures != sdf_textures || sdf_uniform_set_sky != sky_tex) {
		Vector<RD::Uniform> su;
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.binding = 0;
		u.append_id(sdf_params_buffer);
		su.push_back(u);
		RD::Uniform t;
		t.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		t.binding = 1;
		for (RID r : sdf_textures) {
			t.append_id(r);
		}
		su.push_back(t);
		RD::Uniform sk;
		sk.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		sk.binding = 2;
		sk.append_id(sky_tex);
		su.push_back(sk);
		if (sdf_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(sdf_uniform_set)) {
			RD::get_singleton()->free_rid(sdf_uniform_set);
		}
		sdf_uniform_set = RD::get_singleton()->uniform_set_create(su, shader, 1);
		sdf_uniform_set_textures = sdf_textures;
		sdf_uniform_set_sky = sky_tex;
	}

	RD::ComputeListID cl = RD::get_singleton()->compute_list_begin();
	for (int mode = 0; mode <= LIGHT_MODE_RESOLVE; mode++) {
		RD::get_singleton()->compute_list_bind_compute_pipeline(cl, light_pipelines[mode]);
		RD::get_singleton()->compute_list_bind_uniform_set(cl, light_uniform_set, 0);
		RD::get_singleton()->compute_list_bind_uniform_set(cl, sdf_uniform_set, 1);
		for (uint32_t id : todo) {
			const Card &c = cards[id];
			uint32_t pc[4] = { id, light_debug, 0, 0 };
			RD::get_singleton()->compute_list_set_push_constant(cl, pc, sizeof(pc));
			uint32_t div = mode == LIGHT_MODE_PROBES ? 32 : 8;
			RD::get_singleton()->compute_list_dispatch(cl, Math::division_round_up(uint32_t(c.rect.size.x), div), Math::division_round_up(uint32_t(c.rect.size.y), div), 1);
			if (mode == LIGHT_MODE_DIRECT) {
				stats.lit++;
			}
		}
		RD::get_singleton()->compute_list_add_barrier(cl);
	}
	RD::get_singleton()->compute_list_end();
}

void SurfaceCacheEffect::screen_probes_pass(RID p_depth, RID p_normal_roughness, RID p_ambient, RID p_reflection, const Size2i &p_internal_size, const Size2i &p_ambient_size, const Projection &p_projection, const Transform3D &p_cam_transform) {
	if (light_uniform_set.is_null() || !RD::get_singleton()->uniform_set_is_valid(light_uniform_set) || sdf_uniform_set.is_null() || !RD::get_singleton()->uniform_set_is_valid(sdf_uniform_set)) {
		return; // Nothing lit yet this frame.
	}
	Size2i probe_size = Size2i(Math::division_round_up(p_internal_size.x, 16), Math::division_round_up(p_internal_size.y, 16));
	if (screen_probe_texture.is_null() || probe_size != screen_probe_size) {
		if (screen_probe_texture.is_valid()) {
			RD::get_singleton()->free_rid(screen_probe_texture);
		}
		RD::TextureFormat tf;
		tf.width = probe_size.x;
		tf.height = probe_size.y;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		screen_probe_texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
		RD::get_singleton()->set_resource_name(screen_probe_texture, "Surface cache screen probes");
		RD::get_singleton()->texture_clear(screen_probe_texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
		screen_probe_size = probe_size;
	}
	if (reflection_history.is_null() || reflection_history_size != p_ambient_size) {
		if (reflection_history.is_valid()) {
			RD::get_singleton()->free_rid(reflection_history);
		}
		RD::TextureFormat tf;
		tf.width = p_ambient_size.x;
		tf.height = p_ambient_size.y;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		reflection_history = RD::get_singleton()->texture_create(tf, RD::TextureView());
		RD::get_singleton()->set_resource_name(reflection_history, "Surface cache reflection history");
		RD::get_singleton()->texture_clear(reflection_history, Color(0, 0, 0, 0), 0, 1, 0, 1);
		reflection_history_size = p_ambient_size;
	}

	struct ScreenParams {
		float inv_projection[16];
		float cam_transform[16];
		float size[4];
		float misc[4];
	} sp;
	Projection correction;
	correction.set_depth_correction(true);
	MaterialStorage::store_camera((correction * p_projection).inverse(), sp.inv_projection);
	MaterialStorage::store_transform(p_cam_transform, sp.cam_transform);
	sp.size[0] = p_internal_size.x;
	sp.size[1] = p_internal_size.y;
	sp.size[2] = p_ambient_size.x;
	sp.size[3] = p_ambient_size.y;
	sp.misc[0] = p_projection.get_z_far();
	sp.misc[1] = float(frame % 4096);
	sp.misc[2] = sp.misc[3] = 0.0f;
	RD::get_singleton()->buffer_update(screen_params_buffer, 0, sizeof(ScreenParams), &sp);

	RID shader = light_shader.version_get_shader(light_shader_version, LIGHT_MODE_SCREEN_PROBES);
	if (screen_uniform_set.is_null() || !RD::get_singleton()->uniform_set_is_valid(screen_uniform_set) || screen_uniform_set_depth != p_depth || screen_uniform_set_ambient != p_ambient || screen_uniform_set_reflection != p_reflection) {
		Vector<RD::Uniform> uniforms;
		auto add = [&](RD::UniformType p_type, int p_binding, RID p_rid) {
			RD::Uniform u;
			u.uniform_type = p_type;
			u.binding = p_binding;
			u.append_id(p_rid);
			uniforms.push_back(u);
		};
		add(RD::UNIFORM_TYPE_TEXTURE, 0, p_depth);
		add(RD::UNIFORM_TYPE_TEXTURE, 1, p_normal_roughness);
		add(RD::UNIFORM_TYPE_IMAGE, 2, screen_probe_texture);
		add(RD::UNIFORM_TYPE_IMAGE, 3, p_ambient);
		add(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, screen_params_buffer);
		add(RD::UNIFORM_TYPE_IMAGE, 5, p_reflection);
		add(RD::UNIFORM_TYPE_IMAGE, 6, reflection_history);
		if (screen_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(screen_uniform_set)) {
			RD::get_singleton()->free_rid(screen_uniform_set);
		}
		screen_uniform_set = RD::get_singleton()->uniform_set_create(uniforms, shader, 2);
		screen_uniform_set_depth = p_depth;
		screen_uniform_set_ambient = p_ambient;
		screen_uniform_set_reflection = p_reflection;
	}

	last_ambient = p_ambient;
	last_reflection = p_reflection;
	uint32_t pc[4] = { 0, light_debug, 0, 0 };
	RD::ComputeListID cl = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(cl, light_pipelines[LIGHT_MODE_SCREEN_PROBES]);
	RD::get_singleton()->compute_list_bind_uniform_set(cl, light_uniform_set, 0);
	RD::get_singleton()->compute_list_bind_uniform_set(cl, sdf_uniform_set, 1);
	RD::get_singleton()->compute_list_bind_uniform_set(cl, screen_uniform_set, 2);
	RD::get_singleton()->compute_list_set_push_constant(cl, pc, sizeof(pc));
	RD::get_singleton()->compute_list_dispatch(cl, Math::division_round_up(uint32_t(probe_size.x), 8u), Math::division_round_up(uint32_t(probe_size.y), 8u), 1);
	RD::get_singleton()->compute_list_add_barrier(cl);
	RD::get_singleton()->compute_list_bind_compute_pipeline(cl, light_pipelines[LIGHT_MODE_SCREEN_RESOLVE]);
	RD::get_singleton()->compute_list_bind_uniform_set(cl, light_uniform_set, 0);
	RD::get_singleton()->compute_list_bind_uniform_set(cl, sdf_uniform_set, 1);
	RD::get_singleton()->compute_list_bind_uniform_set(cl, screen_uniform_set, 2);
	RD::get_singleton()->compute_list_set_push_constant(cl, pc, sizeof(pc));
	RD::get_singleton()->compute_list_dispatch(cl, Math::division_round_up(uint32_t(p_ambient_size.x), 8u), Math::division_round_up(uint32_t(p_ambient_size.y), 8u), 1);
	RD::get_singleton()->compute_list_bind_compute_pipeline(cl, light_pipelines[LIGHT_MODE_SCREEN_REFLECT]);
	RD::get_singleton()->compute_list_bind_uniform_set(cl, light_uniform_set, 0);
	RD::get_singleton()->compute_list_bind_uniform_set(cl, sdf_uniform_set, 1);
	RD::get_singleton()->compute_list_bind_uniform_set(cl, screen_uniform_set, 2);
	RD::get_singleton()->compute_list_set_push_constant(cl, pc, sizeof(pc));
	RD::get_singleton()->compute_list_dispatch(cl, Math::division_round_up(uint32_t(p_ambient_size.x), 8u), Math::division_round_up(uint32_t(p_ambient_size.y), 8u), 1);
	RD::get_singleton()->compute_list_end();
}

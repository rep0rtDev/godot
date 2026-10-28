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
	light_debug = debug_mode == 7;
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
	tf.format = RD::get_singleton()->texture_is_format_supported_for_usage(RD::DATA_FORMAT_D16_UNORM, RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? RD::DATA_FORMAT_D16_UNORM : RD::DATA_FORMAT_X8_D24_UNORM_PACK32;
	tf.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	depth_stencil = RD::get_singleton()->texture_create(tf, RD::TextureView());

	Vector<RID> fb = { albedo, normal, orm, emission, depth, depth_stencil };
	framebuffer = RD::get_singleton()->framebuffer_create(fb);

	capture_list.set_page_pool(&capture_pool);
	// Clear once so untouched atlas texels read as empty.
	for (RID t : { albedo, normal, orm, emission, depth, direct }) {
		RD::get_singleton()->texture_clear(t, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}

	Vector<String> modes = { "" };
	light_shader.initialize(modes);
	light_shader_version = light_shader.version_create();
	light_pipeline = RD::get_singleton()->compute_pipeline_create(light_shader.version_get_shader(light_shader_version, 0));
	light_params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(float) * 16 + sizeof(uint32_t) * 4);
	sdf_params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(float) * 4 * 9);
	{
		RD::TextureFormat dtf;
		dtf.texture_type = RD::TEXTURE_TYPE_3D;
		dtf.width = 1;
		dtf.height = 1;
		dtf.depth = 1;
		dtf.format = RD::DATA_FORMAT_R8_UNORM;
		dtf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT;
		dummy_sdf_tex = RD::get_singleton()->texture_create(dtf, RD::TextureView());
	}
}

SurfaceCacheEffect::~SurfaceCacheEffect() {
	capture_list.reset();
	if (light_shader_version.is_valid()) {
		light_shader.version_free(light_shader_version);
	}
	for (RID rid : { framebuffer, depth_stencil, depth, direct, emission, orm, normal, albedo, card_buffer, light_params_buffer, sdf_params_buffer, dummy_sdf_tex }) {
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
	const Basis &b = inst.transform.basis;
	Vector3 extent(inst.aabb.size.x * b.get_column(0).length(), inst.aabb.size.y * b.get_column(1).length(), inst.aabb.size.z * b.get_column(2).length());
	inst.first_card = cards.size();
	inst.card_count = 0;
	LocalVector<uint32_t> ids;
	for (int axis = 0; axis < 6; axis++) {
		int a = axis / 2;
		// A face thinner than a texel in either direction is not worth a card.
		if (extent[(a + 1) % 3] * texel_density < 1.0f || extent[(a + 2) % 3] * texel_density < 1.0f) {
			continue;
		}
		Card c;
		c.instance_id = p_instance;
		c.axis = axis;
		if (!_allocate_rect(_card_size(extent, axis), c.rect)) {
			continue;
		}
		_card_view(inst, axis, c.capture_transform, c.capture_projection);
		uint32_t id;
		if (free_cards.size()) {
			id = free_cards[free_cards.size() - 1];
			free_cards.resize(free_cards.size() - 1);
			cards[id] = c;
		} else {
			id = cards.size();
			cards.push_back(c);
		}
		ids.push_back(id);
	}
	// Cards of one instance are kept contiguous in the id list stored on the instance.
	inst.first_card = ids.size() ? ids[0] : 0;
	inst.card_count = ids.size();
	if (ids.size() > 1) {
		for (uint32_t i = 1; i < ids.size(); i++) {
			if (ids[i] != ids[0] + i) {
				// Non-contiguous: fall back to marking by instance id during iteration.
				inst.first_card = 0xFFFFFFFF;
				break;
			}
		}
	}
}

void SurfaceCacheEffect::_drop_cards(uint32_t p_instance) {
	for (uint32_t i = 0; i < cards.size(); i++) {
		if (cards[i].instance_id == p_instance && cards[i].rect.size != Size2i()) {
			_free_rect(cards[i].rect);
			cards[i].rect = Rect2i();
			cards[i].instance_id = 0xFFFFFFFF;
			free_cards.push_back(i);
		}
	}
	instances[p_instance].card_count = 0;
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
		_drop_cards(id);
		instances[id].geometry = nullptr;
		free_instances.push_back(id);
		instance_map.erase(g);
	}
	stats.instances = instance_map.size();
	stats.cards = cards.size() - free_cards.size();
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

void SurfaceCacheEffect::light_cards(RID p_directional_buffer, uint32_t p_directional_count, RID p_omni_buffer, uint32_t p_omni_count, RID p_spot_buffer, uint32_t p_spot_count, const Transform3D &p_view_to_world, const Vector<RID> &p_sdf_cascades, const Vector<Vector4> &p_sdf_offsets, uint32_t p_sdf_grid_size, float p_sdf_y_mult) {
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
		// Orthographic extents from the projection: x = 1/m00, y = 1/m11, depth range = zfar.
		d.extents[0] = c.capture_projection.columns[0][0] != 0.0f ? 1.0f / c.capture_projection.columns[0][0] : 0.0f;
		d.extents[1] = c.capture_projection.columns[1][1] != 0.0f ? 1.0f / c.capture_projection.columns[1][1] : 0.0f;
		d.extents[2] = c.capture_projection.get_z_far();
		d.extents[3] = 0.0f;
	}
	uint32_t needed = gpu_cards.size() * sizeof(CardData);
	if (card_buffer.is_null() || card_buffer_size < needed) {
		if (card_buffer.is_valid()) {
			RD::get_singleton()->free_rid(card_buffer);
		}
		card_buffer_size = MAX(needed, 64u * uint32_t(sizeof(CardData)));
		card_buffer = RD::get_singleton()->storage_buffer_create(card_buffer_size);
	}
	RD::get_singleton()->buffer_update(card_buffer, 0, needed, gpu_cards.ptr());

	struct Params {
		float view_to_world[16];
		uint32_t counts[4];
	} params;
	MaterialStorage::store_transform(p_view_to_world, params.view_to_world);
	params.counts[0] = p_directional_count;
	params.counts[1] = p_omni_count;
	params.counts[2] = p_spot_count;
	params.counts[3] = p_sdf_cascades.size() > 0 ? 1 : 0;
	{
		float sdf_params[4 * 9] = {};
		sdf_params[0] = float(p_sdf_grid_size);
		sdf_params[1] = p_sdf_y_mult;
		sdf_params[2] = float(MIN(p_sdf_cascades.size(), 8));
		for (int i = 0; i < MIN(p_sdf_offsets.size(), 8); i++) {
			sdf_params[4 + i * 4 + 0] = p_sdf_offsets[i].x;
			sdf_params[4 + i * 4 + 1] = p_sdf_offsets[i].y;
			sdf_params[4 + i * 4 + 2] = p_sdf_offsets[i].z;
			sdf_params[4 + i * 4 + 3] = p_sdf_offsets[i].w;
		}
		RD::get_singleton()->buffer_update(sdf_params_buffer, 0, sizeof(sdf_params), sdf_params);
	}
	RD::get_singleton()->buffer_update(light_params_buffer, 0, sizeof(Params), &params);

	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	RID shader = light_shader.version_get_shader(light_shader_version, 0);
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
	if (light_uniform_set.is_null() || !RD::get_singleton()->uniform_set_is_valid(light_uniform_set)) {
		light_uniform_set = RD::get_singleton()->uniform_set_create(uniforms, shader, 0);
	}
	RID set0 = light_uniform_set;

	Vector<RID> sdf_textures;
	for (int i = 0; i < 8; i++) {
		sdf_textures.push_back(i < p_sdf_cascades.size() ? p_sdf_cascades[i] : dummy_sdf_tex);
	}
	if (sdf_uniform_set.is_null() || !RD::get_singleton()->uniform_set_is_valid(sdf_uniform_set) || sdf_uniform_set_textures != sdf_textures) {
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
		if (sdf_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(sdf_uniform_set)) {
			RD::get_singleton()->free_rid(sdf_uniform_set);
		}
		sdf_uniform_set = RD::get_singleton()->uniform_set_create(su, shader, 1);
		sdf_uniform_set_textures = sdf_textures;
	}

	RD::ComputeListID cl = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(cl, light_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(cl, set0, 0);
	RD::get_singleton()->compute_list_bind_uniform_set(cl, sdf_uniform_set, 1);
	for (uint32_t id : todo) {
		const Card &c = cards[id];
		uint32_t pc[4] = { id, light_debug ? 1u : 0u, 0, 0 };
		RD::get_singleton()->compute_list_set_push_constant(cl, pc, sizeof(pc));
		RD::get_singleton()->compute_list_dispatch(cl, Math::division_round_up(uint32_t(c.rect.size.x), 8u), Math::division_round_up(uint32_t(c.rect.size.y), 8u), 1);
		stats.lit++;
	}
	RD::get_singleton()->compute_list_end();
}

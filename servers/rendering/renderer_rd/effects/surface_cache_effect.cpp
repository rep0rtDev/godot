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
	tf.format = RD::get_singleton()->texture_is_format_supported_for_usage(RD::DATA_FORMAT_D16_UNORM, RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? RD::DATA_FORMAT_D16_UNORM : RD::DATA_FORMAT_X8_D24_UNORM_PACK32;
	tf.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	depth_stencil = RD::get_singleton()->texture_create(tf, RD::TextureView());

	Vector<RID> fb = { albedo, normal, orm, emission, depth, depth_stencil };
	framebuffer = RD::get_singleton()->framebuffer_create(fb);

	capture_list.set_page_pool(&capture_pool);
	// Clear once so untouched atlas texels read as empty.
	for (RID t : { albedo, normal, orm, emission, depth }) {
		RD::get_singleton()->texture_clear(t, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
}

SurfaceCacheEffect::~SurfaceCacheEffect() {
	capture_list.reset();
	for (RID rid : { framebuffer, depth_stencil, depth, emission, orm, normal, albedo }) {
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
	}
	capture_list.clear();
}

/**************************************************************************/
/*  virtual_geometry.cpp                                                  */
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

#include "virtual_geometry.h"

#include "core/math/aabb.h"
#include "core/math/basis.h"
#include "core/math/rect2.h"
#include "core/math/vector4.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/sort_array.h"

VirtualGeometry::BuildFunc VirtualGeometry::build_func = nullptr;

bool VirtualGeometry::build(const Vector<Vector3> &p_positions, const Vector<Vector3> &p_normals, const Vector<int> &p_indices, VirtualGeometryBuildResult &r_result) {
	ERR_FAIL_NULL_V_MSG(build_func, false, "Virtual geometry builder is unavailable (meshoptimizer module disabled).");
	ERR_FAIL_COND_V(p_positions.is_empty() || p_indices.size() < 3 || p_indices.size() % 3 != 0, false);
	ERR_FAIL_COND_V(!p_normals.is_empty() && p_normals.size() != p_positions.size(), false);
	Vector<uint32_t> indices;
	indices.resize(p_indices.size());
	uint32_t *w = indices.ptrw();
	const int *r = p_indices.ptr();
	for (int i = 0; i < p_indices.size(); i++) {
		ERR_FAIL_INDEX_V(r[i], p_positions.size(), false);
		w[i] = r[i];
	}
	return build_func(p_positions.ptr(), p_normals.is_empty() ? nullptr : p_normals.ptr(), p_positions.size(), indices.ptr(), indices.size(), r_result);
}

namespace {

uint16_t to_unorm16(float p_value) {
	return (uint16_t)CLAMP(p_value * 65535.0f + 0.5f, 0.0f, 65535.0f);
}

// Same TBN encoding as compressed mesh surfaces: the rotation axis of the tangent frame plus an angle whose
// half carries the binormal sign.
void encode_tbn(const Vector3 &p_normal, const Vector4 &p_tangent, Vector3 &r_axis, float &r_angle) {
	Vector3 normal = p_normal.normalized();
	Vector3 tangent = Vector3(p_tangent.x, p_tangent.y, p_tangent.z).normalized();
	Vector3 binormal = normal.cross(tangent).normalized();
	Basis tbn;
	tbn.rows[0] = tangent;
	tbn.rows[1] = binormal;
	tbn.rows[2] = normal;
	real_t angle;
	tbn.get_axis_angle(r_axis, angle);
	if (p_tangent.w < 0.0f) {
		r_angle = CLAMP((1.0 - angle / Math::PI) * 0.5, 0.0, 0.49999);
	} else {
		r_angle = CLAMP((angle / Math::PI) * 0.5 + 0.5, 0.500008, 1.0);
	}
}

void put_u16(Vector<uint8_t> &r_data, uint16_t p_value) {
	r_data.push_back(p_value & 0xFF);
	r_data.push_back(p_value >> 8);
}

} // namespace

Vector<uint8_t> VirtualGeometry::pack(const VirtualGeometryBuildResult &p_result, const VirtualGeometryAttributes &p_attributes) {
	const uint32_t cluster_count = p_result.clusters.size();
	const uint32_t node_count = p_result.nodes.size();
	const uint32_t vertex_count = p_attributes.positions.size();
	ERR_FAIL_COND_V(cluster_count == 0 || p_result.cluster_level.size() != (int)cluster_count || p_result.cluster_part.size() != (int)cluster_count || p_result.cluster_source_part.size() != (int)cluster_count, Vector<uint8_t>());
	const bool has_normals = p_attributes.normals.size() == (int)vertex_count;
	const bool has_tangents = has_normals && p_attributes.tangents.size() == (int)vertex_count * 4;
	const bool has_uv = p_attributes.uv.size() == (int)vertex_count;
	const bool has_uv2 = p_attributes.uv2.size() == (int)vertex_count;
	const bool has_color = p_attributes.colors.size() == (int)vertex_count;
	const uint32_t flags = (has_tangents ? GEOMETRY_FLAG_TANGENT : 0) | (has_uv ? GEOMETRY_FLAG_UV : 0) | (has_uv2 ? GEOMETRY_FLAG_UV2 : 0) | (has_color ? GEOMETRY_FLAG_COLOR : 0);
	const uint32_t stride = 12 + (has_uv ? 4 : 0) + (has_uv2 ? 4 : 0) + (has_color ? 4 : 0);

	// Coarse clusters first so the root page holds the lowest detail and parents never sit in a later page than
	// their children. Clusters of one group share a level and are contiguous in hierarchy order, so they stay together.
	Vector<uint32_t> order;
	order.resize(cluster_count);
	for (uint32_t i = 0; i < cluster_count; i++) {
		order.write[i] = i;
	}
	struct ByLevel {
		const uint32_t *levels;
		bool operator()(uint32_t a, uint32_t b) const { return levels[a] != levels[b] ? levels[a] > levels[b] : a < b; }
	};
	SortArray<uint32_t, ByLevel> sorter;
	sorter.compare.levels = p_result.cluster_level.ptr();
	sorter.sort(order.ptrw(), cluster_count);

	Vector<VirtualGeometryCluster> clusters = p_result.clusters;
	Vector<VirtualGeometryPage> pages;
	Vector<uint8_t> page_data;
	Vector<uint32_t> cluster_page;
	cluster_page.resize(cluster_count);
	LocalVector<int32_t> local;
	local.resize(vertex_count);
	for (uint32_t v = 0; v < vertex_count; v++) {
		local[v] = -1;
	}
	LocalVector<uint32_t> used;
	VirtualGeometryPage page = {};
	for (uint32_t oi = 0; oi < cluster_count; oi++) {
		const uint32_t ci = order[oi];
		VirtualGeometryCluster &c = clusters.write[ci];
		const uint32_t index_first = p_result.clusters[ci].geometry_offset;
		const uint32_t triangle_count = p_result.clusters[ci].page_and_triangles;
		ERR_FAIL_COND_V(triangle_count == 0 || triangle_count > MAX_CLUSTER_TRIANGLES || index_first + triangle_count * 3 > (uint32_t)p_result.indices.size(), Vector<uint8_t>());
		const uint32_t *idx = p_result.indices.ptr() + index_first;

		used.clear();
		AABB aabb;
		Rect2 uv_rect;
		Rect2 uv2_rect;
		for (uint32_t k = 0; k < triangle_count * 3; k++) {
			const uint32_t v = idx[k];
			ERR_FAIL_COND_V(v >= vertex_count, Vector<uint8_t>());
			if (local[v] < 0) {
				local[v] = used.size();
				used.push_back(v);
				if (used.size() == 1) {
					aabb.position = p_attributes.positions[v];
					if (has_uv) {
						uv_rect.position = p_attributes.uv[v];
					}
					if (has_uv2) {
						uv2_rect.position = p_attributes.uv2[v];
					}
				} else {
					aabb.expand_to(p_attributes.positions[v]);
					if (has_uv) {
						uv_rect.expand_to(p_attributes.uv[v]);
					}
					if (has_uv2) {
						uv2_rect.expand_to(p_attributes.uv2[v]);
					}
				}
			}
		}
		ERR_FAIL_COND_V(used.size() > MAX_CLUSTER_VERTICES + 1, Vector<uint8_t>());

		VirtualGeometryClusterGeometry geometry = {};
		geometry.vertex_count = used.size();
		geometry.triangle_count = triangle_count;
		geometry.vertex_stride = stride;
		geometry.flags = flags;
		geometry.index_offset = sizeof(VirtualGeometryClusterGeometry) + used.size() * stride;
		for (int a = 0; a < 3; a++) {
			geometry.aabb_min[a] = aabb.position[a];
			geometry.aabb_size[a] = aabb.size[a];
		}
		geometry.uv_min[0] = uv_rect.position.x;
		geometry.uv_min[1] = uv_rect.position.y;
		geometry.uv_size[0] = uv_rect.size.x;
		geometry.uv_size[1] = uv_rect.size.y;
		geometry.uv2_min[0] = uv2_rect.position.x;
		geometry.uv2_min[1] = uv2_rect.position.y;
		geometry.uv2_size[0] = uv2_rect.size.x;
		geometry.uv2_size[1] = uv2_rect.size.y;
		const uint32_t cluster_bytes = (geometry.index_offset + triangle_count * 3 + 3) & ~3u;

		const uint32_t limit = pages.is_empty() ? ROOT_PAGE_SIZE : PAGE_SIZE;
		if (page.size > 0 && page.size + cluster_bytes > limit) {
			pages.push_back(page);
			page.offset += page.size;
			page.size = 0;
		}
		cluster_page.write[ci] = pages.size();
		c.geometry_offset = page.offset + page.size;
		c.page_and_triangles = (pages.size() << 8) | triangle_count;
		page.size += cluster_bytes;

		const uint32_t base = page_data.size();
		page_data.resize(base + cluster_bytes);
		memcpy(page_data.ptrw() + base, &geometry, sizeof(geometry));
		Vector<uint8_t> records;
		for (uint32_t v : used) {
			const Vector3 pos = p_attributes.positions[v];
			for (int a = 0; a < 3; a++) {
				put_u16(records, to_unorm16(aabb.size[a] > 0.0f ? (pos[a] - aabb.position[a]) / aabb.size[a] : 0.0f));
			}
			Vector3 axis = has_normals ? p_attributes.normals[v] : Vector3(0, 0, 1);
			float angle = 0.5f;
			if (has_tangents) {
				const float *t = p_attributes.tangents.ptr() + v * 4;
				encode_tbn(axis, Vector4(t[0], t[1], t[2], t[3]), axis, angle);
			}
			const Vector2 oct = axis.normalized().octahedron_encode();
			put_u16(records, to_unorm16(oct.x));
			put_u16(records, to_unorm16(oct.y));
			put_u16(records, to_unorm16(angle));
			if (has_uv) {
				const Vector2 uv = p_attributes.uv[v];
				put_u16(records, to_unorm16(uv_rect.size.x > 0.0f ? (uv.x - uv_rect.position.x) / uv_rect.size.x : 0.0f));
				put_u16(records, to_unorm16(uv_rect.size.y > 0.0f ? (uv.y - uv_rect.position.y) / uv_rect.size.y : 0.0f));
			}
			if (has_uv2) {
				const Vector2 uv = p_attributes.uv2[v];
				put_u16(records, to_unorm16(uv2_rect.size.x > 0.0f ? (uv.x - uv2_rect.position.x) / uv2_rect.size.x : 0.0f));
				put_u16(records, to_unorm16(uv2_rect.size.y > 0.0f ? (uv.y - uv2_rect.position.y) / uv2_rect.size.y : 0.0f));
			}
			if (has_color) {
				const uint32_t rgba = p_attributes.colors[v].to_abgr32();
				for (int b = 0; b < 4; b++) {
					records.push_back((rgba >> (b * 8)) & 0xFF);
				}
			}
		}
		memcpy(page_data.ptrw() + base + sizeof(geometry), records.ptr(), records.size());
		uint8_t *iw = page_data.ptrw() + base + geometry.index_offset;
		for (uint32_t k = 0; k < triangle_count * 3; k++) {
			iw[k] = local[idx[k]];
		}
		for (uint32_t v : used) {
			local[v] = -1;
		}
	}
	pages.push_back(page);

	// Page dependencies: the pages holding the parents of each page's clusters.
	HashMap<int32_t, HashSet<uint32_t>> part_pages; // Group -> pages of the clusters produced from it (the parents of that group's clusters).
	for (uint32_t ci = 0; ci < cluster_count; ci++) {
		if (p_result.cluster_source_part[ci] >= 0) {
			part_pages[p_result.cluster_source_part[ci]].insert(cluster_page[ci]);
		}
	}
	Vector<HashSet<uint32_t>> page_deps;
	page_deps.resize(pages.size());
	for (uint32_t ci = 0; ci < cluster_count; ci++) {
		const int32_t part = p_result.cluster_part[ci];
		if (part < 0 || !part_pages.has(part)) {
			continue;
		}
		for (uint32_t pg : part_pages[part]) {
			if (pg != cluster_page[ci]) {
				page_deps.write[cluster_page[ci]].insert(pg);
			}
		}
	}
	Vector<uint32_t> dependencies;
	for (int pg = 0; pg < pages.size(); pg++) {
		pages.write[pg].dependency_first = dependencies.size();
		pages.write[pg].dependency_count = page_deps[pg].size();
		for (uint32_t d : page_deps[pg]) {
			dependencies.push_back(d);
		}
	}

	VirtualGeometryHeader header = {};
	header.magic = DATA_MAGIC;
	header.cluster_count = cluster_count;
	header.node_count = node_count;
	header.page_count = pages.size();
	header.dependency_count = dependencies.size();
	header.data_size = page_data.size();
	Vector<uint8_t> data;
	data.resize(get_page_data_offset(header) + page_data.size());
	uint8_t *w = data.ptrw();
	memcpy(w, &header, sizeof(header));
	memcpy(w + get_cluster_data_offset(), clusters.ptr(), cluster_count * sizeof(VirtualGeometryCluster));
	memcpy(w + get_node_data_offset(cluster_count), p_result.nodes.ptr(), node_count * sizeof(VirtualGeometryNode));
	memcpy(w + get_page_table_offset(header), pages.ptr(), pages.size() * sizeof(VirtualGeometryPage));
	if (dependencies.size()) {
		memcpy(w + get_dependency_offset(header), dependencies.ptr(), dependencies.size() * sizeof(uint32_t));
	}
	memcpy(w + get_page_data_offset(header), page_data.ptr(), page_data.size());
	return data;
}

bool VirtualGeometry::unpack_header(const Vector<uint8_t> &p_data, VirtualGeometryHeader &r_header) {
	r_header = {};
	if (p_data.size() < (int64_t)sizeof(VirtualGeometryHeader)) {
		return false;
	}
	VirtualGeometryHeader header;
	memcpy(&header, p_data.ptr(), sizeof(header));
	if (header.magic != DATA_MAGIC || header.cluster_count == 0 || header.node_count == 0 || header.page_count == 0) {
		return false;
	}
	const uint64_t expected = uint64_t(get_page_data_offset(header)) + header.data_size;
	if (uint64_t(p_data.size()) != expected) {
		return false;
	}
	r_header = header;
	return true;
}

bool VirtualGeometry::unpack_header(const Vector<uint8_t> &p_data, uint32_t &r_cluster_count, uint32_t &r_node_count) {
	VirtualGeometryHeader header;
	const bool ok = unpack_header(p_data, header);
	r_cluster_count = header.cluster_count;
	r_node_count = header.node_count;
	return ok;
}

bool VirtualGeometry::decode_cluster(const uint8_t *p_page_data, uint32_t p_geometry_offset, Vector<Vector3> &r_positions, Vector<int> &r_indices) {
	VirtualGeometryClusterGeometry geometry;
	memcpy(&geometry, p_page_data + p_geometry_offset, sizeof(geometry));
	ERR_FAIL_COND_V(geometry.vertex_count == 0 || geometry.vertex_count > MAX_CLUSTER_VERTICES + 1 || geometry.triangle_count > MAX_CLUSTER_TRIANGLES, false);
	r_positions.resize(geometry.vertex_count);
	const uint8_t *records = p_page_data + p_geometry_offset + sizeof(geometry);
	for (uint32_t v = 0; v < geometry.vertex_count; v++) {
		const uint8_t *r = records + v * geometry.vertex_stride;
		Vector3 pos;
		for (int a = 0; a < 3; a++) {
			const uint16_t q = r[a * 2] | (r[a * 2 + 1] << 8);
			pos[a] = geometry.aabb_min[a] + geometry.aabb_size[a] * (q / 65535.0f);
		}
		r_positions.write[v] = pos;
	}
	r_indices.resize(geometry.triangle_count * 3);
	const uint8_t *idx = p_page_data + p_geometry_offset + geometry.index_offset;
	for (uint32_t k = 0; k < geometry.triangle_count * 3; k++) {
		ERR_FAIL_COND_V(idx[k] >= geometry.vertex_count, false);
		r_indices.write[k] = idx[k];
	}
	return true;
}

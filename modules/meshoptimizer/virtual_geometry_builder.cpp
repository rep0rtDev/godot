/**************************************************************************/
/*  virtual_geometry_builder.cpp                                          */
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

#include "virtual_geometry_builder.h"

#include "core/templates/local_vector.h"

#include <thirdparty/meshoptimizer/meshoptimizer.h>

#include <cfloat>

namespace {

struct WorkCluster {
	LocalVector<uint32_t> indices;
	float cull_sphere[4] = {};
	float self_sphere[4] = {};
	float parent_sphere[4] = {};
	float self_error = 0.0f;
	float parent_error = FLT_MAX;
	uint32_t level = 0;
};

struct Builder {
	LocalVector<float> positions;
	LocalVector<float> normals;
	LocalVector<uint32_t> position_remap;
	uint32_t vertex_count = 0;
	LocalVector<WorkCluster> clusters;

	// Splits a triangle list into clusters; every new cluster inherits the LOD sphere and error of the group that produced it.
	void clusterize(const uint32_t *p_indices, uint32_t p_index_count, const float *p_lod_sphere, float p_lod_error, uint32_t p_level, LocalVector<uint32_t> &r_new_ids) {
		const size_t max_meshlets = meshopt_buildMeshletsBound(p_index_count, VirtualGeometry::MAX_CLUSTER_VERTICES, VirtualGeometry::MAX_CLUSTER_TRIANGLES);
		LocalVector<meshopt_Meshlet> meshlets;
		meshlets.resize(max_meshlets);
		LocalVector<uint32_t> meshlet_vertices;
		meshlet_vertices.resize(p_index_count);
		LocalVector<uint8_t> meshlet_triangles;
		meshlet_triangles.resize(p_index_count);

		const size_t count = meshopt_buildMeshlets(meshlets.ptr(), meshlet_vertices.ptr(), meshlet_triangles.ptr(), p_indices, p_index_count, positions.ptr(), vertex_count, sizeof(float) * 3, VirtualGeometry::MAX_CLUSTER_VERTICES, VirtualGeometry::MAX_CLUSTER_TRIANGLES, 0.0f);

		for (size_t m = 0; m < count; m++) {
			const meshopt_Meshlet &ml = meshlets[m];
			WorkCluster c;
			c.level = p_level;
			c.indices.resize(ml.triangle_count * 3);
			for (uint32_t k = 0; k < ml.triangle_count * 3; k++) {
				c.indices[k] = meshlet_vertices[ml.vertex_offset + meshlet_triangles[ml.triangle_offset + k]];
			}
			const meshopt_Bounds b = meshopt_computeClusterBounds(c.indices.ptr(), c.indices.size(), positions.ptr(), vertex_count, sizeof(float) * 3);
			c.cull_sphere[0] = b.center[0];
			c.cull_sphere[1] = b.center[1];
			c.cull_sphere[2] = b.center[2];
			c.cull_sphere[3] = b.radius;
			for (int i = 0; i < 4; i++) {
				c.self_sphere[i] = p_lod_sphere ? p_lod_sphere[i] : c.cull_sphere[i];
			}
			c.self_error = p_lod_error;
			r_new_ids.push_back(clusters.size());
			clusters.push_back(c);
		}
	}

	// Groups clusters that share edges, simplifies each group with its outer boundary locked so
	// neighboring groups stay crack-free at any LOD mix, and re-splits the result into clusters.
	bool build_level(const LocalVector<uint32_t> &p_level_ids, uint32_t p_level, LocalVector<uint32_t> &r_next_ids) {
		LocalVector<uint32_t> partition_indices;
		LocalVector<uint32_t> partition_counts;
		for (uint32_t id : p_level_ids) {
			for (uint32_t idx : clusters[id].indices) {
				partition_indices.push_back(position_remap[idx]);
			}
			partition_counts.push_back(clusters[id].indices.size());
		}
		LocalVector<uint32_t> partition;
		partition.resize(p_level_ids.size());
		const size_t group_count = meshopt_partitionClusters(partition.ptr(), partition_indices.ptr(), partition_indices.size(), partition_counts.ptr(), p_level_ids.size(), positions.ptr(), vertex_count, sizeof(float) * 3, VirtualGeometry::GROUP_TARGET_SIZE);

		LocalVector<LocalVector<uint32_t>> groups;
		groups.resize(group_count);
		for (uint32_t i = 0; i < p_level_ids.size(); i++) {
			groups[partition[i]].push_back(p_level_ids[i]);
		}

		// A position is locked when it is referenced by more than one group.
		LocalVector<int32_t> owner;
		owner.resize(vertex_count);
		LocalVector<uint8_t> shared;
		shared.resize(vertex_count);
		for (uint32_t v = 0; v < vertex_count; v++) {
			owner[v] = -1;
			shared[v] = 0;
		}
		for (uint32_t g = 0; g < groups.size(); g++) {
			for (uint32_t id : groups[g]) {
				for (uint32_t idx : clusters[id].indices) {
					const uint32_t p = position_remap[idx];
					if (owner[p] == -1) {
						owner[p] = g;
					} else if (owner[p] != (int32_t)g) {
						shared[p] = 1;
					}
				}
			}
		}
		LocalVector<uint8_t> lock;
		lock.resize(vertex_count);
		for (uint32_t v = 0; v < vertex_count; v++) {
			lock[v] = shared[position_remap[v]];
		}

		const float attribute_weights[3] = { 0.5f, 0.5f, 0.5f };
		LocalVector<uint32_t> merged;
		LocalVector<uint32_t> simplified;
		LocalVector<float> centers;
		LocalVector<float> radii;

		for (const LocalVector<uint32_t> &group : groups) {
			merged.clear();
			for (uint32_t id : group) {
				for (uint32_t idx : clusters[id].indices) {
					merged.push_back(idx);
				}
			}
			const size_t target = (merged.size() / 6) * 3;
			simplified.resize(merged.size());
			float error = 0.0f;
			const size_t result = meshopt_simplifyWithAttributes(simplified.ptr(), merged.ptr(), merged.size(), positions.ptr(), vertex_count, sizeof(float) * 3,
					normals.is_empty() ? nullptr : normals.ptr(), sizeof(float) * 3, normals.is_empty() ? nullptr : attribute_weights, normals.is_empty() ? 0 : 3,
					lock.ptr(), target, FLT_MAX, meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute, &error);

			if (result == 0 || result > merged.size() * 85 / 100) {
				// Not reducible enough (mostly locked boundary): these clusters stay roots.
				continue;
			}

			centers.clear();
			radii.clear();
			float group_error = error;
			for (uint32_t id : group) {
				const WorkCluster &c = clusters[id];
				centers.push_back(c.self_sphere[0]);
				centers.push_back(c.self_sphere[1]);
				centers.push_back(c.self_sphere[2]);
				radii.push_back(c.self_sphere[3]);
				group_error = MAX(group_error, c.self_error);
			}
			const meshopt_Bounds gb = meshopt_computeSphereBounds(centers.ptr(), group.size(), sizeof(float) * 3, radii.ptr(), sizeof(float));
			const float group_sphere[4] = { gb.center[0], gb.center[1], gb.center[2], gb.radius };

			for (uint32_t id : group) {
				WorkCluster &c = clusters[id];
				for (int i = 0; i < 4; i++) {
					c.parent_sphere[i] = group_sphere[i];
				}
				c.parent_error = group_error;
			}
			clusterize(simplified.ptr(), result, group_sphere, group_error, p_level, r_next_ids);
		}
		return !r_next_ids.is_empty();
	}
};

} // namespace

bool virtual_geometry_build(const Vector3 *p_positions, const Vector3 *p_normals, uint32_t p_vertex_count, const uint32_t *p_indices, uint32_t p_index_count, VirtualGeometryBuildResult &r_result) {
	ERR_FAIL_COND_V(p_vertex_count == 0 || p_index_count < 3 || p_index_count % 3 != 0, false);

	Builder b;
	b.vertex_count = p_vertex_count;
	b.positions.resize(p_vertex_count * 3);
	for (uint32_t i = 0; i < p_vertex_count; i++) {
		b.positions[i * 3 + 0] = p_positions[i].x;
		b.positions[i * 3 + 1] = p_positions[i].y;
		b.positions[i * 3 + 2] = p_positions[i].z;
	}
	if (p_normals) {
		b.normals.resize(p_vertex_count * 3);
		for (uint32_t i = 0; i < p_vertex_count; i++) {
			b.normals[i * 3 + 0] = p_normals[i].x;
			b.normals[i * 3 + 1] = p_normals[i].y;
			b.normals[i * 3 + 2] = p_normals[i].z;
		}
	}
	b.position_remap.resize(p_vertex_count);
	meshopt_generatePositionRemap(b.position_remap.ptr(), b.positions.ptr(), p_vertex_count, sizeof(float) * 3);

	LocalVector<uint32_t> level_ids;
	b.clusterize(p_indices, p_index_count, nullptr, 0.0f, 0, level_ids);
	const uint32_t leaf_cluster_count = b.clusters.size();

	uint32_t level = 1;
	while (level_ids.size() > 1 && level < VirtualGeometry::MAX_LEVELS) {
		LocalVector<uint32_t> next_ids;
		if (!b.build_level(level_ids, level, next_ids)) {
			break;
		}
		level_ids = next_ids;
		level++;
	}

	uint32_t total_indices = 0;
	for (const WorkCluster &c : b.clusters) {
		total_indices += c.indices.size();
	}
	r_result.indices.resize(total_indices);
	r_result.clusters.resize(b.clusters.size());
	r_result.leaf_cluster_count = leaf_cluster_count;
	r_result.leaf_index_count = 0;
	r_result.level_count = level;

	uint32_t *iw = r_result.indices.ptrw();
	VirtualGeometryCluster *cw = r_result.clusters.ptrw();
	uint32_t offset = 0;
	for (uint32_t i = 0; i < b.clusters.size(); i++) {
		const WorkCluster &c = b.clusters[i];
		VirtualGeometryCluster &o = cw[i];
		for (int k = 0; k < 4; k++) {
			o.cull_sphere[k] = c.cull_sphere[k];
			o.self_sphere[k] = c.self_sphere[k];
			o.parent_sphere[k] = c.parent_error == FLT_MAX ? c.self_sphere[k] : c.parent_sphere[k];
		}
		o.self_error = c.self_error;
		o.parent_error = c.parent_error;
		o.index_offset = offset;
		o.index_count = c.indices.size();
		memcpy(iw + offset, c.indices.ptr(), sizeof(uint32_t) * c.indices.size());
		offset += c.indices.size();
		if (i < leaf_cluster_count) {
			r_result.leaf_index_count += c.indices.size();
		}
	}
	return true;
}

/**************************************************************************/
/*  virtual_geometry.h                                                    */
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

#include "core/math/color.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/templates/vector.h"

// A cluster of up to MAX_CLUSTER_TRIANGLES triangles in a hierarchical LOD DAG.
// Layout matches the std430 GPU buffer consumed by the cluster selection shader.
struct VirtualGeometryCluster {
	float cull_sphere[4]; // Tight bounds of this cluster's triangles (center xyz, radius w).
	float self_sphere[4]; // LOD bounds of the group that produced this cluster.
	float parent_sphere[4]; // LOD bounds of the group this cluster was simplified into.
	float self_error; // Object-space error of this cluster relative to the source mesh.
	float parent_error; // Error of the coarser representation; FLT_MAX for roots.
	uint32_t geometry_offset; // Byte offset of this cluster's VirtualGeometryClusterGeometry inside the page data.
	uint32_t page_and_triangles; // (page << 8) | triangle_count.
};

static_assert(sizeof(VirtualGeometryCluster) == 64);

// Per-cluster geometry inside a page, followed by the vertex records and the 8-bit triangle indices.
// Vertex record: position unorm16 x3 in the cluster AABB, normal (or TBN rotation axis when FLAG_TANGENT)
// octahedral unorm16 x2, TBN angle unorm16 (12 bytes), then optional uv unorm16 x2 in the cluster UV range,
// uv2 likewise and color rgba8.
struct VirtualGeometryClusterGeometry {
	uint32_t vertex_count;
	uint32_t triangle_count;
	uint32_t vertex_stride;
	uint32_t flags;
	float aabb_min[3];
	uint32_t index_offset; // Byte offset of the indices from the start of this header.
	float aabb_size[3];
	uint32_t pad;
	float uv_min[2];
	float uv_size[2];
	float uv2_min[2];
	float uv2_size[2];
};

static_assert(sizeof(VirtualGeometryClusterGeometry) == 80);

struct VirtualGeometryPage {
	uint32_t offset; // Byte offset in the page data.
	uint32_t size;
	uint32_t dependency_first; // Pages holding parents of this page's clusters; they must be resident first.
	uint32_t dependency_count;
};

static_assert(sizeof(VirtualGeometryPage) == 16);

// Bounding volume hierarchy node over cluster "parts" (clusters simplified into the same parent group).
// Every node covers a contiguous cluster range, children of a node are contiguous in the node array.
struct VirtualGeometryNode {
	float cull_sphere[4]; // Union of the cull spheres of all clusters below.
	float lod_sphere[4]; // Union of the parent LOD spheres of all clusters below.
	float max_parent_error; // FLT_MAX when a root cluster is below.
	uint32_t child_first;
	uint32_t child_count; // 0 for leaves.
	uint32_t cluster_first;
	uint32_t cluster_count;
	uint32_t pad[3];
};

static_assert(sizeof(VirtualGeometryNode) == 64);

// Serialized surface data ("vg_clusters"): header, clusters, then hierarchy nodes.
struct VirtualGeometryHeader {
	uint32_t magic;
	uint32_t cluster_count;
	uint32_t node_count;
	uint32_t page_count;
	uint32_t dependency_count;
	uint32_t data_size; // Page data bytes.
	uint32_t reserved[2];
};

static_assert(sizeof(VirtualGeometryHeader) == 32);

struct VirtualGeometryBuildResult {
	// Indices of every cluster of every level, finest level first, so the
	// range [0, leaf_index_count) is the full-detail mesh.
	Vector<uint32_t> indices;
	Vector<VirtualGeometryCluster> clusters;
	Vector<VirtualGeometryNode> nodes;
	Vector<uint32_t> cluster_level; // 0 for the finest clusters.
	Vector<int32_t> cluster_part; // Group this cluster was simplified into, -1 for roots.
	Vector<int32_t> cluster_source_part; // Group that produced this cluster, -1 for leaves.
	uint32_t leaf_index_count = 0;
	uint32_t leaf_cluster_count = 0;
	uint32_t level_count = 0;
};

struct VirtualGeometryAttributes {
	Vector<Vector3> positions;
	Vector<Vector3> normals;
	Vector<float> tangents; // 4 per vertex.
	Vector<Vector2> uv;
	Vector<Vector2> uv2;
	Vector<Color> colors;
};

class VirtualGeometry {
public:
	static constexpr uint32_t MAX_CLUSTER_TRIANGLES = 128;
	static constexpr uint32_t MAX_CLUSTER_VERTICES = 255;
	static constexpr uint32_t GROUP_TARGET_SIZE = 8;
	static constexpr uint32_t MAX_LEVELS = 32;
	static constexpr uint32_t NODE_FANOUT = 4;
	static constexpr uint32_t DATA_MAGIC = 0x33444756; // "VGD3".
	static constexpr uint32_t PAGE_SIZE = 128 * 1024;
	static constexpr uint32_t ROOT_PAGE_SIZE = 32 * 1024;
	static constexpr uint32_t GEOMETRY_FLAG_TANGENT = 1;
	static constexpr uint32_t GEOMETRY_FLAG_UV = 2;
	static constexpr uint32_t GEOMETRY_FLAG_UV2 = 4;
	static constexpr uint32_t GEOMETRY_FLAG_COLOR = 8;

	// Layout: header, clusters, nodes, pages, page dependencies (uint32), cluster children (uint32), page data.
	// Children of a cluster: (first << 8) | count of the finer clusters it was simplified from, NO_CHILDREN for leaves.
	static constexpr uint32_t NO_CHILDREN = 0xFFFFFFFF;
	static Vector<uint8_t> pack(const VirtualGeometryBuildResult &p_result, const VirtualGeometryAttributes &p_attributes);
	// Validates serialized data; returns false for malformed or old-format data.
	static bool unpack_header(const Vector<uint8_t> &p_data, VirtualGeometryHeader &r_header);
	static bool unpack_header(const Vector<uint8_t> &p_data, uint32_t &r_cluster_count, uint32_t &r_node_count);
	static uint32_t get_page_table_offset(const VirtualGeometryHeader &p_header) { return get_node_data_offset(p_header.cluster_count) + p_header.node_count * sizeof(VirtualGeometryNode); }
	static uint32_t get_dependency_offset(const VirtualGeometryHeader &p_header) { return get_page_table_offset(p_header) + p_header.page_count * sizeof(VirtualGeometryPage); }
	static uint32_t get_children_offset(const VirtualGeometryHeader &p_header) { return get_dependency_offset(p_header) + p_header.dependency_count * sizeof(uint32_t); }
	static uint32_t get_page_data_offset(const VirtualGeometryHeader &p_header) { return get_children_offset(p_header) + p_header.cluster_count * sizeof(uint32_t); }
	// Decodes one cluster of the page data back into triangles (tests and CPU fallbacks).
	static bool decode_cluster(const uint8_t *p_page_data, uint32_t p_geometry_offset, Vector<Vector3> &r_positions, Vector<int> &r_indices);
	static uint32_t get_cluster_data_offset() { return sizeof(VirtualGeometryHeader); }
	static uint32_t get_node_data_offset(uint32_t p_cluster_count) { return sizeof(VirtualGeometryHeader) + p_cluster_count * sizeof(VirtualGeometryCluster); }

	typedef bool (*BuildFunc)(const Vector3 *p_positions, const Vector3 *p_normals, uint32_t p_vertex_count, const uint32_t *p_indices, uint32_t p_index_count, VirtualGeometryBuildResult &r_result);
	static BuildFunc build_func;

	static bool build(const Vector<Vector3> &p_positions, const Vector<Vector3> &p_normals, const Vector<int> &p_indices, VirtualGeometryBuildResult &r_result);
};

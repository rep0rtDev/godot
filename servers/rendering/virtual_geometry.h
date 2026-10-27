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
	uint32_t index_offset;
	uint32_t index_count;
};

static_assert(sizeof(VirtualGeometryCluster) == 64);

struct VirtualGeometryBuildResult {
	// Indices of every cluster of every level, finest level first, so the
	// range [0, leaf_index_count) is the full-detail mesh.
	Vector<uint32_t> indices;
	Vector<VirtualGeometryCluster> clusters;
	uint32_t leaf_index_count = 0;
	uint32_t leaf_cluster_count = 0;
	uint32_t level_count = 0;
};

class VirtualGeometry {
public:
	static constexpr uint32_t MAX_CLUSTER_TRIANGLES = 128;
	static constexpr uint32_t MAX_CLUSTER_VERTICES = 255;
	static constexpr uint32_t GROUP_TARGET_SIZE = 8;
	static constexpr uint32_t MAX_LEVELS = 32;

	typedef bool (*BuildFunc)(const Vector3 *p_positions, const Vector3 *p_normals, uint32_t p_vertex_count, const uint32_t *p_indices, uint32_t p_index_count, VirtualGeometryBuildResult &r_result);
	static BuildFunc build_func;

	static bool build(const Vector<Vector3> &p_positions, const Vector<Vector3> &p_normals, const Vector<int> &p_indices, VirtualGeometryBuildResult &r_result);
};

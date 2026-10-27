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

Vector<uint8_t> VirtualGeometry::pack(const VirtualGeometryBuildResult &p_result) {
	const uint32_t cluster_count = p_result.clusters.size();
	const uint32_t node_count = p_result.nodes.size();
	Vector<uint8_t> data;
	data.resize(get_node_data_offset(cluster_count) + node_count * sizeof(VirtualGeometryNode));
	uint8_t *w = data.ptrw();
	const VirtualGeometryHeader header = { DATA_MAGIC, cluster_count, node_count, 0 };
	memcpy(w, &header, sizeof(header));
	memcpy(w + get_cluster_data_offset(), p_result.clusters.ptr(), cluster_count * sizeof(VirtualGeometryCluster));
	memcpy(w + get_node_data_offset(cluster_count), p_result.nodes.ptr(), node_count * sizeof(VirtualGeometryNode));
	return data;
}

bool VirtualGeometry::unpack_header(const Vector<uint8_t> &p_data, uint32_t &r_cluster_count, uint32_t &r_node_count) {
	r_cluster_count = 0;
	r_node_count = 0;
	if (p_data.size() < (int64_t)sizeof(VirtualGeometryHeader)) {
		return false;
	}
	VirtualGeometryHeader header;
	memcpy(&header, p_data.ptr(), sizeof(header));
	if (header.magic != DATA_MAGIC || header.cluster_count == 0 || header.node_count == 0) {
		return false;
	}
	const uint64_t expected = sizeof(VirtualGeometryHeader) + uint64_t(header.cluster_count) * sizeof(VirtualGeometryCluster) + uint64_t(header.node_count) * sizeof(VirtualGeometryNode);
	if (uint64_t(p_data.size()) != expected) {
		return false;
	}
	r_cluster_count = header.cluster_count;
	r_node_count = header.node_count;
	return true;
}

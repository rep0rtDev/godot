#[compute]

#version 450

#VERSION_DEFINES

#ifndef MODE_SHADOW
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require
#endif

// Compute rasterizer for small virtual geometry clusters: one workgroup per cluster, one thread per triangle.
// Each covered pixel center gets atomicMax((depth bits << 32) | (entry << 7) | triangle) in a 64-bit visibility buffer.
// Depth is reversed (1 near, 0 far) so a larger value is closer, and the cleared value 0 means empty.
// The entry indexes a per-frame table of (instance slot, cluster) pairs used to find the triangle again later.

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

struct Cluster {
	vec4 cull_sphere;
	vec4 self_sphere;
	vec4 parent_sphere;
	float self_error;
	float parent_error;
	uint geometry_offset;
	uint page_and_triangles;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Clusters {
	Cluster data[];
}
clusters;

layout(set = 0, binding = 1, std430) restrict readonly buffer Work {
	uint data[];
}
work;

layout(set = 0, binding = 2, std430) restrict readonly buffer Pool {
	uint data[];
}
pool;

layout(set = 0, binding = 3, std430) restrict readonly buffer Pages {
	uint data[];
}
pages;

#define VG_POOL(w) pool.data[w]
#define VG_PAGE_BASE(p) pages.data[p]
#include "virtual_geometry_geometry_inc.glsl"

#ifdef MODE_SHADOW
// Virtual shadow window: depth bits per window texel, [window slot][size.y][size.x], merged into the page pool by the copy pass.
layout(set = 1, binding = 0, std430) restrict buffer ShadowRaster {
	uint data[];
}
shadow_raster;
#define RASTER_FLAG_COMPACT 8u
#else
layout(set = 1, binding = 0, std430) restrict buffer VisibilityBuffer {
	uint64_t data[];
}
visibility_buffer;

layout(set = 1, binding = 1, std430) restrict buffer Entries {
	uint count;
	uint pad0;
	uvec2 data[];
}
entries;
#endif

#define FLAG_CULL_POSITIVE 2u
#define FLAG_CULL_NEGATIVE 4u

layout(push_constant, std430) uniform Params {
	mat4 model_view_projection;
	uvec2 size;
	uint list_offset; // Index in work.data of the first cluster id of the list to rasterize.
	uint flags;
	uint slot;
	uint entry_capacity;
	uint pad0;
	uint pad1;
}
params;

#define MAX_ENTRIES (1u << 25u)

#ifndef MODE_SHADOW
shared uint entry;
#endif

vec3 project(vec3 p_position) {
	vec4 clip = params.model_view_projection * vec4(p_position, 1.0);
	vec3 ndc = clip.xyz / clip.w;
	return vec3((ndc.xy * 0.5 + 0.5) * vec2(params.size), ndc.z);
}

float edge(vec2 a, vec2 b, vec2 p) {
	return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

void main() {
#ifdef MODE_SHADOW
	// The window's culled command range: [vertex_count, instance_count, first_vertex = cluster * 384, first_instance, pad] per entry,
	// its count in work.data[entry_capacity] when compacted, otherwise one entry per cluster with instance_count 0 or 1.
	uint cmd = params.list_offset + gl_WorkGroupID.x * 5u;
	if ((params.flags & RASTER_FLAG_COMPACT) != 0u) {
		if (gl_WorkGroupID.x >= work.data[params.entry_capacity]) {
			return;
		}
	} else if (work.data[cmd + 1u] == 0u) {
		return;
	}
	uint cluster_id = work.data[cmd + 2u] / 384u;
	Cluster c = clusters.data[cluster_id];
	uint tri = gl_LocalInvocationIndex;
	if (tri >= (c.page_and_triangles & 0xFFu)) {
		return;
	}
#else
	uint cluster_id = work.data[params.list_offset + gl_WorkGroupID.x];
	Cluster c = clusters.data[cluster_id];
	if (gl_LocalInvocationIndex == 0u) {
		entry = atomicAdd(entries.count, 1u);
		if (entry < min(params.entry_capacity, MAX_ENTRIES)) {
			entries.data[entry] = uvec2(params.slot, cluster_id);
		}
	}
	barrier();

	uint tri = gl_LocalInvocationIndex;
	if (tri >= (c.page_and_triangles & 0xFFu) || entry >= min(params.entry_capacity, MAX_ENTRIES)) {
		return;
	}
#endif

	VGGeometry g = vg_load_geometry(c.geometry_offset, c.page_and_triangles >> 8u);
	vec3 a = project(vg_geometry_position(g, vg_geometry_index(g, tri * 3u)));
	vec3 b = project(vg_geometry_position(g, vg_geometry_index(g, tri * 3u + 1u)));
	vec3 v2 = project(vg_geometry_position(g, vg_geometry_index(g, tri * 3u + 2u)));

	float area = edge(a.xy, b.xy, v2.xy);
	if (area == 0.0 || (params.flags & (area > 0.0 ? FLAG_CULL_POSITIVE : FLAG_CULL_NEGATIVE)) != 0u) {
		return;
	}
	if (area < 0.0) {
		vec3 t = b;
		b = v2;
		v2 = t;
		area = -area;
	}

	// Pixel x covers the center x + 0.5.
	vec2 lo = min(min(a.xy, b.xy), v2.xy);
	vec2 hi = max(max(a.xy, b.xy), v2.xy);
	ivec2 first_px = max(ivec2(ceil(lo - 0.5)), ivec2(0));
	ivec2 last_px = min(ivec2(floor(hi - 0.5)), ivec2(params.size) - 1);

#ifndef MODE_SHADOW
	uint64_t id = uint64_t((entry << 7u) | tri);
#endif
	float inv_area = 1.0 / area;
	for (int y = first_px.y; y <= last_px.y; y++) {
		for (int x = first_px.x; x <= last_px.x; x++) {
			vec2 p = vec2(x, y) + 0.5;
			float w0 = edge(b.xy, v2.xy, p);
			float w1 = edge(v2.xy, a.xy, p);
			float w2 = edge(a.xy, b.xy, p);
			if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) {
				continue;
			}
			float z = (w0 * a.z + w1 * b.z + w2 * v2.z) * inv_area;
			if (z <= 0.0 || z > 1.0) {
				continue;
			}
#ifdef MODE_SHADOW
			atomicMax(shadow_raster.data[params.pad0 + uint(y) * params.size.x + uint(x)], floatBitsToUint(z));
#else
			atomicMax(visibility_buffer.data[uint(y) * params.size.x + uint(x)], (uint64_t(floatBitsToUint(z)) << 32u) | id);
#endif
		}
	}
}

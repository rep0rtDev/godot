#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require

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
	uint index_offset;
	uint index_count;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Clusters {
	Cluster data[];
}
clusters;

layout(set = 0, binding = 1, std430) restrict readonly buffer Work {
	uint data[];
}
work;

layout(set = 0, binding = 2, std430) restrict readonly buffer Positions {
	uint data[]; // float3 per vertex, or unorm16 x4 (xyz in the surface AABB, w tangent angle) when FLAG_COMPRESSED.
}
positions;

layout(set = 0, binding = 3, std430) restrict readonly buffer Indices {
	uint data[];
}
indices;

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

#define FLAG_INDEX_16 1u
#define FLAG_CULL_POSITIVE 2u
#define FLAG_CULL_NEGATIVE 4u
#define FLAG_COMPRESSED 8u

layout(push_constant, std430) uniform Params {
	mat4 model_view_projection;
	uvec2 size;
	uint list_offset; // Index in work.data of the first cluster id of the list to rasterize.
	uint flags;
	uint slot;
	uint entry_capacity;
	uint pad0;
	uint pad1;
	vec4 aabb_position; // Dequantization of FLAG_COMPRESSED positions.
	vec4 aabb_size;
}
params;

#define MAX_ENTRIES (1u << 25u)

shared uint entry;

uint fetch_index(uint p_index) {
	if ((params.flags & FLAG_INDEX_16) != 0u) {
		uint word = indices.data[p_index >> 1u];
		return (p_index & 1u) != 0u ? word >> 16u : word & 0xFFFFu;
	}
	return indices.data[p_index];
}

vec3 position(uint p_vertex) {
	if ((params.flags & FLAG_COMPRESSED) != 0u) {
		vec3 unorm = vec3(unpackUnorm2x16(positions.data[p_vertex * 2u]), unpackUnorm2x16(positions.data[p_vertex * 2u + 1u]).x);
		return unorm * params.aabb_size.xyz + params.aabb_position.xyz;
	}
	return vec3(uintBitsToFloat(positions.data[p_vertex * 3u]), uintBitsToFloat(positions.data[p_vertex * 3u + 1u]), uintBitsToFloat(positions.data[p_vertex * 3u + 2u]));
}

vec3 project(uint p_vertex) {
	vec4 clip = params.model_view_projection * vec4(position(p_vertex), 1.0);
	vec3 ndc = clip.xyz / clip.w;
	return vec3((ndc.xy * 0.5 + 0.5) * vec2(params.size), ndc.z);
}

float edge(vec2 a, vec2 b, vec2 p) {
	return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

void main() {
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
	if (tri * 3u >= c.index_count || entry >= min(params.entry_capacity, MAX_ENTRIES)) {
		return;
	}

	uint first = c.index_offset + tri * 3u;
	vec3 a = project(fetch_index(first));
	vec3 b = project(fetch_index(first + 1u));
	vec3 v2 = project(fetch_index(first + 2u));

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

	uint64_t id = uint64_t((entry << 7u) | tri);
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
			atomicMax(visibility_buffer.data[uint(y) * params.size.x + uint(x)], (uint64_t(floatBitsToUint(z)) << 32u) | id);
		}
	}
}

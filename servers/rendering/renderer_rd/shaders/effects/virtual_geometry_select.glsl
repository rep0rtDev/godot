#[compute]

#version 450

#VERSION_DEFINES

// Per-cluster LOD selection for virtual geometry surfaces.
// A cluster is drawn when its own simplification error projects below the pixel
// threshold while the error of the coarser group it was merged into does not.
// Both tests use spheres that grow monotonically up the hierarchy, so every
// thread independently picks the same crack-free cut through the cluster DAG.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

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

struct DrawIndexedCommand {
	uint index_count;
	uint instance_count;
	uint first_index;
	int vertex_offset;
	uint first_instance;
};

// Commands [0, cluster_count): camera pass (LOD + frustum). [cluster_count, 2 * cluster_count): shadow and auxiliary passes (LOD only).
// With FLAG_COMPACT the selected clusters are packed at the start of each range and counts[] holds the draw count
// (cleared to zero before dispatch). Otherwise every cluster keeps its own slot and rejected ones get zero instances.
layout(set = 0, binding = 1, std430) restrict buffer Commands {
	uint counts[4];
	DrawIndexedCommand data[];
}
commands;

#define FLAG_ORTHOGONAL 1u
#define FLAG_COMPACT 2u
#define FLAG_STATS 4u
#define FLAG_OCCLUSION_PHASE1 8u
#define FLAG_FLIP_Y 16u

// 1 when the cluster passed the occlusion test last frame. Phase one draws only those; phase two
// tests everything against the depth pyramid built from phase one and draws the newly visible rest.
layout(set = 0, binding = 3, std430) restrict buffer Visibility {
	uint data[];
}
visibility;

#ifdef MODE_OCCLUSION
layout(set = 1, binding = 0) uniform sampler2D hzb;
#endif

// Camera clusters, camera triangles, shadow clusters, shadow triangles.
layout(set = 0, binding = 2, std430) restrict buffer Stats {
	uint data[4];
}
stats;

layout(push_constant, std430) uniform Params {
	vec4 modelview[3]; // Rows of the object to view space affine transform.
	mat4 projection;
	uint cluster_count;
	float lod_scale; // Pixels per unit at unit distance, divided by the pixel error threshold.
	uint flags;
	uint pad;
}
params;

vec3 to_view(vec3 p) {
	vec4 v = vec4(p, 1.0);
	return vec3(dot(params.modelview[0], v), dot(params.modelview[1], v), dot(params.modelview[2], v));
}

bool error_is_imperceptible(vec4 sphere, float error, float scale) {
	if (error > 1.0e37) {
		return false;
	}
	float distance = 1.0;
	if ((params.flags & FLAG_ORTHOGONAL) == 0u) {
		distance = max(length(to_view(sphere.xyz)) - sphere.w * scale, 1.0e-5);
	}
	return error * scale * params.lod_scale <= distance;
}

bool in_frustum(vec3 center, float radius) {
	vec4 row0 = vec4(params.projection[0][0], params.projection[1][0], params.projection[2][0], params.projection[3][0]);
	vec4 row1 = vec4(params.projection[0][1], params.projection[1][1], params.projection[2][1], params.projection[3][1]);
	vec4 row3 = vec4(params.projection[0][3], params.projection[1][3], params.projection[2][3], params.projection[3][3]);
	vec4 planes[4] = vec4[](row3 + row0, row3 - row0, row3 + row1, row3 - row1);
	for (int i = 0; i < 4; i++) {
		vec4 p = planes[i];
		float len = length(p.xyz);
		if (len > 0.0 && (dot(p.xyz, center) + p.w) / len < -radius) {
			return false;
		}
	}
	return true;
}

#ifdef MODE_OCCLUSION
// Screen-space bounds of a view-space sphere from its tangent planes along each axis
// (Mara and McGuire, "2D Polyhedral Bounds of a Clipped, Perspective-Projected 3D Sphere", 2013).
vec2 sphere_extent(float a, float d, float r) {
	float t = sqrt(a * a + d * d - r * r);
	return vec2((t * a - r * d) / (t * d + r * a), (t * a + r * d) / (t * d - r * a));
}

bool is_occluded(vec3 center, float radius) {
	float d = -center.z;
	float near = params.projection[3][2] / (params.projection[2][2] - 1.0);
	float nearest = d - radius;
	if (nearest <= near) {
		return false;
	}

	vec2 ex = sphere_extent(center.x, d, radius) * params.projection[0][0] - params.projection[2][0];
	vec2 ey = sphere_extent(center.y, d, radius) * params.projection[1][1] - params.projection[2][1];
	vec2 vy = (params.flags & FLAG_FLIP_Y) != 0u ? 0.5 - 0.5 * ey.yx : 0.5 + 0.5 * ey;
	vec4 uv = clamp(vec4(0.5 + 0.5 * ex.x, vy.x, 0.5 + 0.5 * ex.y, vy.y), 0.0, 1.0);

	// Reversed depth of the nearest point; the RD correction maps GL clip z to 0.5 - 0.5 * ndc_z.
	float z_clip = -params.projection[2][2] * nearest + params.projection[3][2];
	float sphere_depth = 0.5 - 0.5 * (z_clip / nearest);

	ivec2 size = textureSize(hzb, 0);
	vec2 extent = (uv.zw - uv.xy) * vec2(size);
	int levels = textureQueryLevels(hzb);
	int level = clamp(int(ceil(log2(max(max(extent.x, extent.y), 1.0)))), 0, levels - 1);
	ivec2 level_size = max(size >> level, ivec2(1));
	ivec2 lo = clamp(ivec2(uv.xy * vec2(level_size)), ivec2(0), level_size - 1);
	ivec2 hi = clamp(ivec2(uv.zw * vec2(level_size)), ivec2(0), level_size - 1);
	if (any(greaterThan(hi - lo, ivec2(1))) && level + 1 < levels) {
		level++;
		level_size = max(size >> level, ivec2(1));
		lo = clamp(ivec2(uv.xy * vec2(level_size)), ivec2(0), level_size - 1);
		hi = clamp(ivec2(uv.zw * vec2(level_size)), ivec2(0), level_size - 1);
	}

	float farthest = min(min(texelFetch(hzb, lo, level).r, texelFetch(hzb, ivec2(hi.x, lo.y), level).r),
			min(texelFetch(hzb, ivec2(lo.x, hi.y), level).r, texelFetch(hzb, hi, level).r));
	return sphere_depth < farthest;
}
#endif

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= params.cluster_count) {
		return;
	}

	Cluster c = clusters.data[index];

	vec3 axis_x = vec3(params.modelview[0].x, params.modelview[1].x, params.modelview[2].x);
	vec3 axis_y = vec3(params.modelview[0].y, params.modelview[1].y, params.modelview[2].y);
	vec3 axis_z = vec3(params.modelview[0].z, params.modelview[1].z, params.modelview[2].z);
	float scale = sqrt(max(dot(axis_x, axis_x), max(dot(axis_y, axis_y), dot(axis_z, axis_z))));

	bool selected = error_is_imperceptible(c.self_sphere, c.self_error, scale) && !error_is_imperceptible(c.parent_sphere, c.parent_error, scale);
	bool visible = selected && in_frustum(to_view(c.cull_sphere.xyz), c.cull_sphere.w * scale);

#ifdef MODE_OCCLUSION
	bool now_visible = visible && !is_occluded(to_view(c.cull_sphere.xyz), c.cull_sphere.w * scale);
	if (now_visible && visibility.data[index] == 0u) {
		DrawIndexedCommand late;
		late.index_count = c.index_count;
		late.instance_count = 1u;
		late.first_index = c.index_offset;
		late.vertex_offset = 0;
		late.first_instance = 0u;
		commands.data[2u * params.cluster_count + atomicAdd(commands.counts[2], 1u)] = late;
		if ((params.flags & FLAG_STATS) != 0u) {
			atomicAdd(stats.data[0], 1u);
			atomicAdd(stats.data[1], c.index_count / 3u);
		}
	}
	visibility.data[index] = now_visible ? 1u : 0u;
	return;
#else
	if ((params.flags & FLAG_OCCLUSION_PHASE1) != 0u && visibility.data[index] == 0u) {
		visible = false;
	}
#endif

	DrawIndexedCommand cmd;
	cmd.index_count = c.index_count;
	cmd.first_index = c.index_offset;
	cmd.vertex_offset = 0;
	cmd.first_instance = 0u;

	if ((params.flags & FLAG_STATS) != 0u) {
		if (visible) {
			atomicAdd(stats.data[0], 1u);
			atomicAdd(stats.data[1], c.index_count / 3u);
		}
		if (selected) {
			atomicAdd(stats.data[2], 1u);
			atomicAdd(stats.data[3], c.index_count / 3u);
		}
	}

	if ((params.flags & FLAG_COMPACT) != 0u) {
		cmd.instance_count = 1u;
		if (visible) {
			commands.data[atomicAdd(commands.counts[0], 1u)] = cmd;
		}
		if (selected) {
			commands.data[params.cluster_count + atomicAdd(commands.counts[1], 1u)] = cmd;
		}
	} else {
		cmd.instance_count = visible ? 1u : 0u;
		commands.data[index] = cmd;
		cmd.instance_count = selected ? 1u : 0u;
		commands.data[params.cluster_count + index] = cmd;
	}
}

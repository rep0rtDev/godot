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

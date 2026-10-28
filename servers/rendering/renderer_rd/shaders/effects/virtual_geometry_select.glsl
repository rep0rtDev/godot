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
	uint geometry_offset;
	uint page_and_triangles; // (page << 8) | triangle_count.
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Clusters {
	Cluster data[];
}
clusters;

// Non-indexed draw pulling its vertices from the cluster pages; first_vertex = cluster * 384 carries the cluster
// (gl_VertexIndex always includes it, unlike first_instance which needs drawIndirectFirstInstance).
struct DrawCommand {
	uint vertex_count;
	uint instance_count;
	uint first_vertex;
	uint first_instance;
	uint pad;
};

// Commands [0, cluster_count): camera pass (LOD + frustum). [cluster_count, 2 * cluster_count): shadow and auxiliary passes (LOD only).
// [2, 3) * cluster_count: late camera clusters. [3, 5) * cluster_count: early and late clusters rasterized in compute,
// which only the color pass draws.
// With FLAG_COMPACT the selected clusters are packed at the start of each range and counts[] holds the draw count
// (cleared to zero before dispatch). Otherwise every cluster keeps its own slot and rejected ones get zero instances.
layout(set = 0, binding = 1, std430) restrict buffer Commands {
	uint counts[8];
	uvec4 window_args[16]; // Per window range: visible cluster count as dispatch x, then 1, 1, 0.
	DrawCommand data[];
}
commands;

#define FLAG_ORTHOGONAL 1u
#define FLAG_COMPACT 2u
#define FLAG_STATS 4u
#define FLAG_OCCLUSION_PHASE1 8u
#define FLAG_FLIP_Y 16u
#define FLAG_HIERARCHY 32u
#define FLAG_SOFTWARE 64u
#define FLAG_WINDOW 128u

struct Node {
	vec4 cull_sphere;
	vec4 lod_sphere;
	float max_parent_error;
	uint child_first;
	uint child_count;
	uint cluster_first;
	uint cluster_count;
	uint pad0;
	uint pad1;
	uint pad2;
};

layout(set = 0, binding = 4, std430) restrict readonly buffer Nodes {
	Node data[];
}
nodes;

// [0..2]: indirect dispatch size for the candidate pass, [3]: candidate count,
// [4]: 1 when the whole instance failed the camera frustum or occlusion test in last frame's phase two,
// [5]: the value of [4] seen by this frame's phase one, [6..7]: unused,
// [8..10] and [11..13]: indirect dispatch size of the early and late software raster (one workgroup per cluster),
// then the traversal queue (node_count entries), the candidate cluster ids (cluster_count entries)
// and the early and late software raster cluster ids (cluster_count entries each).
#define WORK_HEADER 16u
layout(set = 0, binding = 5, std430) restrict coherent buffer Work {
	uint data[];
}
work;

// 1 when the cluster passed the occlusion test last frame. Phase one draws only those; phase two
// tests everything against the depth pyramid built from phase one and draws the newly visible rest.
layout(set = 0, binding = 3, std430) restrict buffer Visibility {
	uint data[];
}
visibility;

// Streaming: word offset per page in the page pool, NOT_RESIDENT when the page is not loaded.
#define NOT_RESIDENT 0xFFFFFFFFu
layout(set = 0, binding = 6, std430) restrict readonly buffer Pages {
	uint data[];
}
pages;

// (first << 8) | count of the finer clusters each cluster was simplified from, NOT_RESIDENT for leaves.
layout(set = 0, binding = 7, std430) restrict readonly buffer Children {
	uint data[];
}
children;

// Per page: bit 0 drawn this frame, bit 1 wanted but not resident. Read back and cleared by the CPU.
layout(set = 0, binding = 8, std430) restrict buffer Requests {
	uint data[];
}
requests;

bool page_resident(uint p_page) {
	return pages.data[p_page] != NOT_RESIDENT;
}

#ifdef MODE_OCCLUSION
layout(set = 1, binding = 0) uniform sampler2D hzb;
#endif

// Camera clusters, camera triangles, shadow clusters, shadow triangles, software clusters, software triangles.
layout(set = 0, binding = 2, std430) restrict buffer Stats {
	uint data[8];
}
stats;

layout(push_constant, std430) uniform Params {
	vec4 modelview[3]; // Rows of the object to view space affine transform.
	mat4 projection;
	uint cluster_count;
	float lod_scale; // Pixels per unit at unit distance, divided by the pixel error threshold.
	uint flags;
	uint node_count;
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

float instance_scale() {
	vec3 axis_x = vec3(params.modelview[0].x, params.modelview[1].x, params.modelview[2].x);
	vec3 axis_y = vec3(params.modelview[0].y, params.modelview[1].y, params.modelview[2].y);
	vec3 axis_z = vec3(params.modelview[0].z, params.modelview[1].z, params.modelview[2].z);
	return sqrt(max(dot(axis_x, axis_x), max(dot(axis_y, axis_y), dot(axis_z, axis_z))));
}

#ifdef MODE_TRAVERSE

// One workgroup walks the hierarchy breadth first. A node is expanded while some cluster below it could still
// need finer detail than its parent offers; accepted leaves emit their clusters as candidates for the
// per-cluster pass. Every node is queued at most once, so the queue never exceeds node_count entries.
shared uint queue_head;
shared uint queue_tail;

void main() {
	uint lid = gl_LocalInvocationID.x;
	float scale = instance_scale();
	uint candidates = WORK_HEADER + params.node_count;
	if (lid == 0u) {
		queue_head = 0u;
		queue_tail = 1u;
		work.data[3] = 0u;
		work.data[WORK_HEADER] = 0u;
		work.data[8] = 0u;
		work.data[9] = 1u;
		work.data[10] = 1u;
		work.data[11] = 0u;
		work.data[12] = 1u;
		work.data[13] = 1u;
		if ((params.flags & FLAG_OCCLUSION_PHASE1) != 0u) {
			work.data[5] = work.data[4];
		} else {
			work.data[4] = 0u;
			work.data[5] = 0u;
		}
	}

	while (true) {
		memoryBarrierBuffer();
		memoryBarrierShared();
		barrier();
		uint start = queue_head;
		uint end = min(queue_tail, start + gl_WorkGroupSize.x);
		barrier();
		if (start >= end) {
			break;
		}
		if (lid == 0u) {
			queue_head = end;
		}
		if (start + lid < end) {
			Node n = nodes.data[work.data[WORK_HEADER + start + lid]];
			if (!error_is_imperceptible(n.lod_sphere, n.max_parent_error, scale)) {
				if (n.child_count == 0u) {
					uint dst = atomicAdd(work.data[3], n.cluster_count);
					for (uint k = 0u; k < n.cluster_count; k++) {
						work.data[candidates + dst + k] = n.cluster_first + k;
					}
				} else {
					uint dst = atomicAdd(queue_tail, n.child_count);
					for (uint k = 0u; k < n.child_count; k++) {
						work.data[WORK_HEADER + dst + k] = n.child_first + k;
					}
				}
			}
		}
	}

	if (lid == 0u) {
		uint count = work.data[3];
		work.data[0] = (count + 63u) / 64u;
		work.data[1] = 1u;
		work.data[2] = 1u;
	}
}

#elif defined(MODE_INSTANCE)

// Phase two instance test against the root bounds. A rejected instance skips the per-cluster occlusion pass;
// its per-cluster visibility goes stale, so the next frame's phase one draws none of its clusters for the
// camera and its phase two treats every cluster as not yet drawn.
void main() {
	if (gl_LocalInvocationID.x != 0u) {
		return;
	}
	vec4 sphere = nodes.data[0].cull_sphere;
	vec3 center = to_view(sphere.xyz);
	float radius = sphere.w * instance_scale();
	bool rejected = !in_frustum(center, radius) || is_occluded(center, radius);
	work.data[4] = rejected ? 1u : 0u;
	if (rejected) {
		work.data[0] = 0u;
	}
}

#else

// Clusters whose bounding sphere projects smaller than the limit (in pixels, divided by the pixel error like lod_scale)
// are rasterized in compute. Clusters touching the near plane always stay on the hardware rasterizer.
bool use_software(Cluster c, float scale) {
	if ((params.flags & (FLAG_SOFTWARE | FLAG_HIERARCHY)) != (FLAG_SOFTWARE | FLAG_HIERARCHY) || (params.flags & FLAG_ORTHOGONAL) != 0u) {
		return false;
	}
	vec3 center = to_view(c.cull_sphere.xyz);
	float radius = c.cull_sphere.w * scale;
	float near = params.projection[3][2] / (params.projection[2][2] - 1.0);
	float closest = -center.z - radius;
	if (closest <= near) {
		return false;
	}
	float limit = float(params.flags >> 16u) / 16.0;
	return 2.0 * radius * params.lod_scale < limit * closest;
}

void append_software(uint p_index, uint p_list, uint p_counter) {
	uint slot = atomicAdd(work.data[p_counter], 1u);
	work.data[WORK_HEADER + params.node_count + (1u + p_list) * params.cluster_count + slot] = p_index;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if ((params.flags & FLAG_HIERARCHY) != 0u) {
		if (index >= work.data[3]) {
			return;
		}
		index = work.data[WORK_HEADER + params.node_count + index];
	} else if (index >= params.cluster_count) {
		return;
	}

	Cluster c = clusters.data[index];

	float scale = instance_scale();

	// A cluster draws when its parent is too coarse and it is fine enough itself, or when the finer clusters it
	// stands in for are not streamed in yet; those pages get requested. Clusters of unloaded pages never draw.
	bool self_fine = error_is_imperceptible(c.self_sphere, c.self_error, scale);
	bool parent_fine = error_is_imperceptible(c.parent_sphere, c.parent_error, scale);
	bool children_ok = true;
	uint child_range = children.data[index];
	if (!parent_fine && !self_fine && child_range != NOT_RESIDENT) {
		for (uint k = 0u; k < (child_range & 0xFFu); k++) {
			uint child_page = clusters.data[(child_range >> 8u) + k].page_and_triangles >> 8u;
			if (!page_resident(child_page)) {
				children_ok = false;
				atomicOr(requests.data[child_page], 2u);
			}
		}
	}
	uint page = c.page_and_triangles >> 8u;
	bool selected = page_resident(page) && !parent_fine && (self_fine || !children_ok);
	if (selected) {
		atomicOr(requests.data[page], 1u);
	}
	bool visible = selected && in_frustum(to_view(c.cull_sphere.xyz), c.cull_sphere.w * scale);
	if ((params.flags & FLAG_WINDOW) != 0u) {
		// Virtual shadow window: only the clusters inside this window go to its own range; nothing else is touched.
		uint range = 5u + (params.flags >> 16u);
		DrawCommand wcmd;
		wcmd.vertex_count = (c.page_and_triangles & 0xFFu) * 3u;
		wcmd.instance_count = 1u;
		wcmd.first_vertex = index * 384u;
		wcmd.first_instance = 0u;
		wcmd.pad = 0u;
		if (visible && (params.flags & FLAG_STATS) != 0u) {
			atomicAdd(stats.data[6], 1u);
			atomicAdd(stats.data[7], (c.page_and_triangles & 0xFFu));
		}
		if ((params.flags & FLAG_COMPACT) != 0u) {
			if (visible) {
				commands.data[range * params.cluster_count + atomicAdd(commands.window_args[params.flags >> 16u].x, 1u)] = wcmd;
			}
		} else {
			wcmd.instance_count = visible ? 1u : 0u;
			commands.data[range * params.cluster_count + index] = wcmd;
		}
		return;
	}

#ifdef MODE_OCCLUSION
	bool now_visible = visible && !is_occluded(to_view(c.cull_sphere.xyz), c.cull_sphere.w * scale);
	bool drawn_early = visibility.data[index] != 0u && ((params.flags & FLAG_HIERARCHY) == 0u || work.data[5] == 0u);
	if (now_visible && !drawn_early) {
		DrawCommand late;
		late.vertex_count = (c.page_and_triangles & 0xFFu) * 3u;
		late.instance_count = 1u;
		late.first_vertex = index * 384u;
		late.first_instance = 0u;
		late.pad = 0u;
		bool software = use_software(c, scale);
		if (software) {
			append_software(index, 1u, 11u);
			commands.data[4u * params.cluster_count + atomicAdd(commands.counts[4], 1u)] = late;
		} else {
			commands.data[2u * params.cluster_count + atomicAdd(commands.counts[2], 1u)] = late;
		}
		if ((params.flags & FLAG_STATS) != 0u) {
			atomicAdd(stats.data[0], 1u);
			atomicAdd(stats.data[1], (c.page_and_triangles & 0xFFu));
			if (software) {
				atomicAdd(stats.data[4], 1u);
				atomicAdd(stats.data[5], (c.page_and_triangles & 0xFFu));
			}
		}
	}
	visibility.data[index] = now_visible ? 1u : 0u;
	return;
#else
	if ((params.flags & FLAG_OCCLUSION_PHASE1) != 0u && (visibility.data[index] == 0u || ((params.flags & FLAG_HIERARCHY) != 0u && work.data[5] != 0u))) {
		visible = false;
	}
#endif

	bool software = visible && use_software(c, scale);

	DrawCommand cmd;
	cmd.vertex_count = (c.page_and_triangles & 0xFFu) * 3u;
	cmd.first_vertex = index * 384u;
	cmd.first_instance = 0u;
	cmd.pad = 0u;

	if ((params.flags & FLAG_STATS) != 0u) {
		if (visible) {
			atomicAdd(stats.data[0], 1u);
			atomicAdd(stats.data[1], (c.page_and_triangles & 0xFFu));
		}
		if (software) {
			atomicAdd(stats.data[4], 1u);
			atomicAdd(stats.data[5], (c.page_and_triangles & 0xFFu));
		}
		if (selected) {
			atomicAdd(stats.data[2], 1u);
			atomicAdd(stats.data[3], (c.page_and_triangles & 0xFFu));
		}
	}

	if ((params.flags & FLAG_COMPACT) != 0u) {
		cmd.instance_count = 1u;
		if (software) {
			append_software(index, 0u, 8u);
			commands.data[3u * params.cluster_count + atomicAdd(commands.counts[3], 1u)] = cmd;
		} else if (visible) {
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

#endif

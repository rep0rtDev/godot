#[compute]

#version 450

#VERSION_DEFINES

// Virtual shadow maps: page marking from the camera depth buffer and copying rendered pages into the physical pool.
// A directional light has LEVELS clipmap levels; level l spans base_extent * 2^l meters as PAGES x PAGES pages of
// TEXELS x TEXELS texels. Page coordinates are absolute in light space and wrap modulo PAGES into the page table.

#define LEVELS 16
#define PAGES 128
#define TEXELS 128

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#ifdef MODE_MARK

layout(set = 0, binding = 0) uniform sampler2D depth_texture;

layout(set = 0, binding = 1, std140) uniform Params {
	mat4 inv_projection;
	mat4 inv_view;
	mat4 light_view;
	vec4 params; // base extent, levels, projection[1][1], viewport height
	ivec4 size; // width, height, spot count, directional valid
}
params;

layout(set = 0, binding = 2, std430) restrict buffer Requests {
	uint data[];
}
requests;

struct SpotParams {
	mat4 clip_from_world;
	vec4 position_range;
	vec4 info; // first level, mips, tan(half fov), unused
};

layout(set = 0, binding = 3, std430) restrict readonly buffer Spots {
	SpotParams data[];
}
spots;

void request(uint entry) {
	atomicOr(requests.data[entry >> 5], 1u << (entry & 31u));
}

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size.xy))) {
		return;
	}
	float depth = texelFetch(depth_texture, pos, 0).r;
	if (depth <= 0.0) {
		return; // Reversed Z: nothing was drawn here.
	}
	vec2 uv = (vec2(pos) + 0.5) / vec2(params.size.xy);
	vec4 view = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	view.xyz /= view.w;
	vec3 world = (params.inv_view * vec4(view.xyz, 1.0)).xyz;
	float footprint = -view.z * 2.0 / (params.params.z * params.params.w);
	if (params.size.w != 0) {
		int level = clamp(int(ceil(log2(footprint * float(PAGES * TEXELS) / params.params.x))), 0, int(params.params.y) - 1);
		vec2 lp = (params.light_view * vec4(world, 1.0)).xy;
		float page_size = params.params.x * float(1 << level) / float(PAGES);
		ivec2 page = ivec2(floor(lp / page_size)) & (PAGES - 1);
		request(uint(level) * uint(PAGES * PAGES) + uint(page.y) * uint(PAGES) + uint(page.x));
	}
	for (int s = 0; s < params.size.z; s++) {
		if (spots.data[s].info.w == 0.0) {
			continue;
		}
		vec4 clip = spots.data[s].clip_from_world * vec4(world, 1.0);
		if (clip.w <= 0.0) {
			continue;
		}
		vec2 ndc = clip.xy / clip.w;
		if (any(greaterThan(abs(ndc), vec2(1.0)))) {
			continue;
		}
		float dist = distance(world, spots.data[s].position_range.xyz);
		if (dist > spots.data[s].position_range.w) {
			continue;
		}
		int mips = int(spots.data[s].info.y);
		int mip = clamp(int(ceil(log2(footprint * float(PAGES * TEXELS) / (2.0 * dist * spots.data[s].info.z)))), 0, mips - 1);
		int pages = PAGES >> mip;
		ivec2 page = clamp(ivec2((ndc * 0.5 + 0.5) * float(pages)), ivec2(0), ivec2(pages - 1));
		request((uint(spots.data[s].info.x) + uint(mip)) * uint(PAGES * PAGES) + uint(page.y) * uint(PAGES) + uint(page.x));
	}
}

#endif

#ifdef MODE_COPY

layout(set = 0, binding = 0) uniform sampler2D scratch_depth;

layout(set = 0, binding = 1, std430) restrict readonly buffer Copies {
	uvec4 data[]; // Source texel origin in the scratch target, destination texel origin in the pool.
}
copies;

layout(set = 0, binding = 2, r32f) uniform restrict writeonly image2D pool;

void main() {
	uvec4 c = copies.data[gl_GlobalInvocationID.z];
	ivec2 t = ivec2(gl_GlobalInvocationID.xy);
	if (t.x >= TEXELS || t.y >= TEXELS) {
		return;
	}
	float d = texelFetch(scratch_depth, ivec2(c.xy) + t, 0).r;
	imageStore(pool, ivec2(c.zw) + t, vec4(d));
}

#endif

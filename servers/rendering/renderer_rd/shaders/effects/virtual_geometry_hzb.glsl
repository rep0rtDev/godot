#[compute]

#version 450

#VERSION_DEFINES

// Builds one level of the hierarchical Z buffer used for virtual geometry occlusion culling.
// Depth is reversed (1 = near), so every texel keeps the minimum, i.e. the farthest depth of its footprint.
// The footprint is rounded outwards, which keeps the result conservative when the source is not exactly twice the size.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#ifdef MODE_FROM_DEPTH
layout(set = 0, binding = 0) uniform sampler2D source_depth;
#else
layout(set = 0, binding = 0, r32f) uniform restrict readonly image2D source_image;
#endif

layout(set = 1, binding = 0, r32f) uniform restrict writeonly image2D dest_image;

layout(push_constant, std430) uniform Params {
	ivec2 source_size;
	ivec2 dest_size;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.dest_size))) {
		return;
	}

	ivec2 from = (pos * params.source_size) / params.dest_size;
	ivec2 to = min(((pos + 1) * params.source_size + params.dest_size - 1) / params.dest_size, params.source_size);

	float farthest = 1.0;
	for (int y = from.y; y < to.y; y++) {
		for (int x = from.x; x < to.x; x++) {
#ifdef MODE_FROM_DEPTH
			farthest = min(farthest, texelFetch(source_depth, ivec2(x, y), 0).r);
#else
			farthest = min(farthest, imageLoad(source_image, ivec2(x, y)).r);
#endif
		}
	}

	imageStore(dest_image, pos, vec4(farthest));
}

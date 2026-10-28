#[compute]

#version 450

#VERSION_DEFINES

// Temporal super resolution: jittered low-resolution colour, depth and motion vectors in, full-resolution colour out,
// with a full-resolution history. Stage A: dilated velocity, Catmull-Rom history, YCoCg clamp against the current
// neighbourhood, accumulation 1/(n+1) with n up to 16 (4 while moving), restart on disocclusion or off-screen.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D color_buffer; // Internal size, jittered.
layout(set = 0, binding = 1) uniform sampler2D depth_buffer; // Internal size, reverse-Z.
layout(rg16f, set = 0, binding = 2) uniform restrict readonly image2D velocity_buffer; // Internal size, uv units.
layout(set = 0, binding = 3) uniform sampler2D history_buffer; // Target size, rgb colour, a = frames / 16.
layout(rgba16f, set = 0, binding = 4) uniform restrict writeonly image2D output_buffer; // Target size.
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D history_out; // Target size.

layout(push_constant, std430) uniform Params {
	vec2 internal_size;
	vec2 target_size;
	vec2 jitter; // Current jitter in internal pixels.
	float reset; // 1 drops the history.
	float sharpness;
}
params;

vec3 rgb_to_ycocg(vec3 c) {
	return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 ycocg_to_rgb(vec3 c) {
	return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

vec3 reinhard(vec3 c) {
	return c / (1.0 + c);
}

vec3 reinhard_inverse(vec3 c) {
	return c / (1.0 - min(c, vec3(0.999)));
}

// 9-tap Catmull-Rom (5 bilinear fetches would do; 9 keeps it simple and exact).
vec4 sample_catmull_rom(sampler2D tex, vec2 uv, vec2 resolution) {
	vec2 sample_pos = uv * resolution;
	vec2 tex_pos1 = floor(sample_pos - 0.5) + 0.5;
	vec2 f = sample_pos - tex_pos1;
	vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3 = f * f * (-0.5 + 0.5 * f);
	vec2 w12 = w1 + w2;
	vec2 offset12 = w2 / w12;
	vec2 tex_pos0 = (tex_pos1 - 1.0) / resolution;
	vec2 tex_pos3 = (tex_pos1 + 2.0) / resolution;
	vec2 tex_pos12 = (tex_pos1 + offset12) / resolution;
	vec4 result = vec4(0.0);
	result += textureLod(tex, vec2(tex_pos0.x, tex_pos0.y), 0.0) * w0.x * w0.y;
	result += textureLod(tex, vec2(tex_pos12.x, tex_pos0.y), 0.0) * w12.x * w0.y;
	result += textureLod(tex, vec2(tex_pos3.x, tex_pos0.y), 0.0) * w3.x * w0.y;
	result += textureLod(tex, vec2(tex_pos0.x, tex_pos12.y), 0.0) * w0.x * w12.y;
	result += textureLod(tex, vec2(tex_pos12.x, tex_pos12.y), 0.0) * w12.x * w12.y;
	result += textureLod(tex, vec2(tex_pos3.x, tex_pos12.y), 0.0) * w3.x * w12.y;
	result += textureLod(tex, vec2(tex_pos0.x, tex_pos3.y), 0.0) * w0.x * w3.y;
	result += textureLod(tex, vec2(tex_pos12.x, tex_pos3.y), 0.0) * w12.x * w3.y;
	result += textureLod(tex, vec2(tex_pos3.x, tex_pos3.y), 0.0) * w3.x * w3.y;
	return result;
}

vec3 clip_aabb(vec3 aabb_min, vec3 aabb_max, vec3 q) {
	vec3 center = 0.5 * (aabb_max + aabb_min);
	vec3 extents = 0.5 * (aabb_max - aabb_min) + 1e-5;
	vec3 v = q - center;
	vec3 a = abs(v / extents);
	float ma = max(a.x, max(a.y, a.z));
	return ma > 1.0 ? center + v / ma : q;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(vec2(pixel), params.target_size))) {
		return;
	}
	vec2 uv = (vec2(pixel) + 0.5) / params.target_size;
	// The scene was rendered with the projection shifted by the jitter: the unjittered sample sits at uv - jitter.
	vec2 uv_src = uv + params.jitter / params.internal_size;
	ivec2 src_pixel = clamp(ivec2(uv * params.internal_size), ivec2(0), ivec2(params.internal_size) - 1);

	// Current colour and neighbourhood bounds (3x3 at internal resolution, YCoCg, tonemapped).
	vec3 current = reinhard(textureLod(color_buffer, uv_src, 0.0).rgb);
	vec3 m1 = vec3(0.0);
	vec3 m2 = vec3(0.0);
	vec3 n_min = vec3(1e9);
	vec3 n_max = vec3(-1e9);
	float nearest_depth = 0.0;
	ivec2 nearest = src_pixel;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 p = clamp(src_pixel + ivec2(x, y), ivec2(0), ivec2(params.internal_size) - 1);
			vec3 c = rgb_to_ycocg(reinhard(texelFetch(color_buffer, p, 0).rgb));
			m1 += c;
			m2 += c * c;
			n_min = min(n_min, c);
			n_max = max(n_max, c);
			float d = texelFetch(depth_buffer, p, 0).r;
			if (d > nearest_depth) {
				nearest_depth = d;
				nearest = p;
			}
		}
	}
	vec3 mean = m1 / 9.0;
	vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
	// Dilated velocity: the nearest pixel of the 3x3 carries the motion, so edges of moving objects keep their history.
	vec2 velocity = imageLoad(velocity_buffer, nearest).xy;
	vec2 uv_prev = uv + velocity;
	float speed_px = length(velocity * params.target_size);

	vec4 history = sample_catmull_rom(history_buffer, uv_prev, params.target_size);
	history.rgb = reinhard(max(history.rgb, vec3(0.0)));
	float frames = history.a * 16.0;
	bool offscreen = any(lessThan(uv_prev, vec2(0.0))) || any(greaterThan(uv_prev, vec2(1.0)));
	if (params.reset > 0.5 || offscreen || frames < 0.5) {
		frames = 0.0;
	}
	// Variance clamp: wider box when still (sub-pixel jitter is the signal), tight box when moving.
	float gamma = mix(1.25, 2.0, smoothstep(2.0, 0.0, speed_px));
	vec3 box_min = max(n_min, mean - gamma * sigma);
	vec3 box_max = min(n_max, mean + gamma * sigma);
	vec3 hist_ycocg = clip_aabb(box_min, box_max, rgb_to_ycocg(history.rgb));
	vec3 hist = ycocg_to_rgb(hist_ycocg);
	float max_frames = speed_px > 1.0 ? 4.0 : 16.0;
	frames = min(frames, max_frames);
	float alpha = 1.0 / (frames + 1.0);
	vec3 result = mix(hist, current, alpha);
	frames = min(frames + 1.0, 16.0);
	vec3 out_rgb = reinhard_inverse(result);
	imageStore(history_out, pixel, vec4(out_rgb, frames / 16.0));
	imageStore(output_buffer, pixel, vec4(out_rgb, 1.0));
}

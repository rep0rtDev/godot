#[compute]

#version 450

#VERSION_DEFINES

// Surface cache lighting. One dispatch per card in each of three modes:
//  MODE_DIRECT: every atlas texel gets Lambert direct light from the scene lights, shadowed by sphere tracing the
//               SDFGI cascades (a global SDF). Positions come from the captured depth and the capture camera.
//  MODE_PROBES: one radiosity probe per 4x4 texels traces cosine rays through the SDF; hits read the final lighting
//               of the surface cache through an object grid, misses read the sky term. Accumulated over frames.
//  MODE_RESOLVE: final = direct + albedo * bilinear(probes).

#define MAX_CASCADES 8
#define GRID 32
#define GRID_SLOTS 8
#define PROBE_RAYS 8
#define M_PI 3.14159265359

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../light_data_inc.glsl"
#include "../oct_inc.glsl"

struct CardData {
	ivec4 rect;
	mat4 view_to_world; // Capture camera to world.
	mat4 world_to_view;
	vec4 extents; // half width, half height, depth range, unused.
};

struct InstanceData {
	mat4 world_to_local;
	vec4 aabb_min;
	vec4 aabb_size;
	uvec4 info; // first card, valid, unused, unused.
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Cards {
	CardData data[];
}
cards;

layout(set = 0, binding = 1) uniform texture2D albedo_tex;
layout(set = 0, binding = 2) uniform texture2D normal_tex;
layout(set = 0, binding = 3) uniform texture2D depth_tex;
layout(set = 0, binding = 4) uniform texture2D emission_tex;
layout(set = 0, binding = 5, rgba16f) uniform restrict image2D direct_image;

#define MAX_DIRECTIONAL_LIGHTS 8

layout(set = 0, binding = 6, std140) uniform DirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHTS];
}
directional_lights;

layout(set = 0, binding = 7, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 0, binding = 8, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

layout(set = 0, binding = 9, std140) uniform Params {
	mat4 view_to_world; // Camera view space (light buffers) to world.
	uvec4 counts; // directional, omni, spot, use_sdf
	vec4 grid; // Object grid origin, cell size.
	vec4 sky; // Sky radiance for missed rays, frame number.
}
params;

layout(set = 0, binding = 10) uniform sampler linear_sampler;
layout(set = 0, binding = 11) uniform sampler nearest_sampler;

layout(set = 0, binding = 12, std430) restrict readonly buffer Instances {
	InstanceData data[];
}
instances;

layout(set = 0, binding = 13, std430) restrict readonly buffer ObjectGrid {
	uint data[]; // GRID^3 cells of GRID_SLOTS instance ids, 0xFFFFFFFF ends a cell.
}
object_grid;

layout(set = 0, binding = 14, rgba16f) uniform restrict image2D probe_image;
layout(set = 0, binding = 15, rgba16f) uniform restrict image2D final_image;

// SDFGI cascades as a global SDF. info: grid size in cells, y_mult, cascade count. cascades: world offset of cell
// (0,0,0) in y-scaled space and 1 / cell size.
layout(set = 1, binding = 0, std140) uniform Sdf {
	vec4 info;
	vec4 cascades[MAX_CASCADES];
	vec4 sky_params; // mode (0 ambient color, 1 background color, 2 sky octmap), energy, border offset, border scale.
	vec4 sky_color;
	vec4 sky_quat; // World to sky orientation.
}
sdf;

layout(set = 1, binding = 1) uniform texture3D sdf_cascades[MAX_CASCADES];
layout(set = 1, binding = 2) uniform texture2D sky_octmap;

// Radiance of the sky in a world direction, for rays that leave the scene.
vec3 sky_radiance(vec3 dir) {
	int mode = int(sdf.sky_params.x);
	if (mode == 2) {
		vec4 q = sdf.sky_quat;
		vec3 sky_dir = dir + 2.0 * cross(q.xyz, cross(q.xyz, dir) + q.w * dir);
		return textureLod(sampler2D(sky_octmap, linear_sampler), vec3_to_oct_with_border(sky_dir, sdf.sky_params.zw), 0.0).rgb * sdf.sky_params.y;
	}
	if (mode == 1) {
		return sdf.sky_color.rgb * sdf.sky_params.y;
	}
	return params.sky.rgb;
}

#if defined(MODE_SCREEN_PROBES) || defined(MODE_SCREEN_RESOLVE) || defined(MODE_SCREEN_REFLECT)
// Screen probes: one per SCREEN_PROBE_SIZE pixels, cosine rays through the SDF into the surface cache, resolved into
// the GI ambient buffer the forward pass already reads.
#define SCREEN_PROBE_SIZE 16
#define SCREEN_PROBE_RAYS 16

layout(set = 2, binding = 0) uniform texture2D depth_buffer;
layout(set = 2, binding = 1) uniform texture2D normal_roughness_buffer;
layout(set = 2, binding = 2, rgba16f) uniform restrict image2D screen_probe_image; // rgb radiance, a view depth.
layout(set = 2, binding = 3, rgba16f) uniform restrict writeonly image2D ambient_image;
layout(set = 2, binding = 5, rgba16f) uniform restrict writeonly image2D reflection_image;
layout(set = 2, binding = 6, rgba16f) uniform restrict image2D reflection_history; // rgb radiance, a view depth.

layout(set = 2, binding = 4, std140) uniform Screen {
	mat4 inv_projection;
	mat4 cam_transform;
	vec4 size; // internal width, height, ambient width, height.
	vec4 misc; // z_far, frame, screen trace enabled, unused.
	mat4 projection;
	mat4 prev_view_projection;
}
screen;
layout(set = 2, binding = 8, rgba16f) uniform restrict readonly image2D screen_probe_prev;
layout(set = 2, binding = 9, rgba16f) uniform restrict readonly image2D reflection_history_prev;

// Previous-frame history texel for a world position and the view depth it should hold.
bool reproject(vec3 world, ivec2 size, out ivec2 r_texel, out float r_depth) {
	vec4 clip = screen.prev_view_projection * vec4(world, 1.0);
	if (clip.w <= 0.0) {
		return false;
	}
	vec2 ndc = clip.xy / clip.w;
	if (any(greaterThan(abs(ndc), vec2(1.0)))) {
		return false;
	}
	r_texel = clamp(ivec2((ndc * 0.5 + 0.5) * vec2(size)), ivec2(0), size - 1);
	r_depth = clip.w;
	return true;
}
layout(set = 2, binding = 7) uniform texture2D hzb_buffer; // Reverse-Z, each mip keeps the farthest depth.
#define HZB_MIPS 6

// View-space position of an internal pixel; false for the sky.
bool screen_position(ivec2 pixel, out vec3 r_view, out vec3 r_normal) {
	float depth = texelFetch(sampler2D(depth_buffer, nearest_sampler), pixel, 0).r;
	vec4 pos = vec4((vec2(pixel) + 0.5) / screen.size.xy * 2.0 - 1.0, depth, 1.0);
	pos = screen.inv_projection * pos;
	r_view = pos.xyz / pos.w;
	if (-r_view.z >= screen.misc.x * 0.98) {
		return false;
	}
	r_normal = normalize(texelFetch(sampler2D(normal_roughness_buffer, nearest_sampler), pixel, 0).xyz * 2.0 - 1.0);
	return true;
}

// Screen-space trace against the depth pyramid before the SDF: the hit comes from the depth buffer (thin geometry
// the SDF does not resolve), lighting still from the surface cache. View-space origin and direction.
bool screen_trace(vec3 origin, vec3 dir, out vec3 r_world, out vec3 r_normal) {
	if (screen.misc.z < 0.5) {
		return false;
	}
	vec2 hzb_size = vec2(textureSize(sampler2D(hzb_buffer, nearest_sampler), 0));
	vec2 prev_px = vec2(-1.0);
	float t = 0.04;
	for (int i = 0; i < 24; i++) {
		vec3 p = origin + dir * t;
		vec4 clip = screen.projection * vec4(p, 1.0);
		if (clip.w <= 0.0) {
			return false;
		}
		vec3 ndc = clip.xyz / clip.w;
		if (any(greaterThan(abs(ndc.xy), vec2(1.0)))) {
			return false;
		}
		vec2 uv = ndc.xy * 0.5 + 0.5;
		vec2 px = uv * hzb_size;
		float step_px = prev_px.x < 0.0 ? 1.0 : length(px - prev_px);
		prev_px = px;
		float mip = clamp(floor(log2(max(step_px, 1.0))), 0.0, float(HZB_MIPS - 1));
		float far_z = textureLod(sampler2D(hzb_buffer, nearest_sampler), uv, mip).r;
		if (ndc.z < far_z) {
			// Behind the farthest surface of the footprint: check the exact pixel and its thickness.
			ivec2 pixel = ivec2(uv * screen.size.xy);
			vec3 sp, sn;
			if (screen_position(pixel, sp, sn)) {
				float thickness = 0.25 + 0.03 * (-sp.z);
				if (-p.z >= -sp.z && -p.z - (-sp.z) < thickness && dot(sn, dir) < 0.0) {
					r_world = (screen.cam_transform * vec4(sp, 1.0)).xyz;
					r_normal = normalize(mat3(screen.cam_transform) * sn);
					return true;
				}
			}
		}
		t *= 1.3;
	}
	return false;
}

float screen_roughness(ivec2 pixel) {
	float roughness = texelFetch(sampler2D(normal_roughness_buffer, nearest_sampler), pixel, 0).w;
	if (roughness > 0.5) {
		roughness = 1.0 - roughness; // Dynamic object flag.
	}
	return roughness / (127.0 / 255.0);
}
#endif

layout(push_constant, std430) uniform Push {
	uint card;
	uint debug;
	uint pad[2];
}
push;

// Finest cascade containing p (y-scaled space), or -1.
int find_cascade(vec3 p) {
	for (uint j = 0; j < uint(sdf.info.z); j++) {
		vec3 c = (p - sdf.cascades[j].xyz) * sdf.cascades[j].w;
		if (all(greaterThanEqual(c, vec3(1.0))) && all(lessThan(c, vec3(sdf.info.x - 1.0)))) {
			return int(j);
		}
	}
	return -1;
}

// The cascade index differs between threads, so the texture array is indexed with constants only (dynamic
// non-uniform indexing of texture arrays is undefined without the nonuniform extension).
float sdf_fetch(int j, vec3 uvw) {
	switch (j) {
		case 0:
			return texture(sampler3D(sdf_cascades[0], linear_sampler), uvw).r;
		case 1:
			return texture(sampler3D(sdf_cascades[1], linear_sampler), uvw).r;
		case 2:
			return texture(sampler3D(sdf_cascades[2], linear_sampler), uvw).r;
		case 3:
			return texture(sampler3D(sdf_cascades[3], linear_sampler), uvw).r;
		case 4:
			return texture(sampler3D(sdf_cascades[4], linear_sampler), uvw).r;
		case 5:
			return texture(sampler3D(sdf_cascades[5], linear_sampler), uvw).r;
		case 6:
			return texture(sampler3D(sdf_cascades[6], linear_sampler), uvw).r;
		default:
			return texture(sampler3D(sdf_cascades[7], linear_sampler), uvw).r;
	}
}

float sdf_sample(vec3 p, int j) {
	vec3 uvw = (p - sdf.cascades[j].xyz) * sdf.cascades[j].w / sdf.info.x;
	return sdf_fetch(j, uvw) * 255.0 - 1.0; // Cells.
}

// Sphere trace from origin along dir (world space). Returns true on a hit with the hit point and its cascade.
bool sdf_trace(vec3 origin, vec3 dir, float max_dist, out vec3 r_hit, out int r_cascade) {
	r_cascade = -1;
	if (params.counts.w == 0u) {
		return false;
	}
	float y_mult = sdf.info.y;
	origin.y *= y_mult;
	dir.y *= y_mult;
	float dir_len = length(dir);
	dir /= dir_len;
	max_dist *= dir_len;
	int j = find_cascade(origin);
	if (j < 0) {
		return false;
	}
	// The ray starts on a surface the SDF only resolves to a cell: a hit counts once the ray has been a full cell
	// away from any surface, so grazing rays do not hit their own surface.
	float t = 1.0 / sdf.cascades[j].w;
	bool left_surface = false;
	int prev_j = j;
	for (int i = 0; i < 96 && t < max_dist; i++) {
		vec3 p = origin + dir * t;
		j = find_cascade(p);
		if (j < 0) {
			return false;
		}
		float d = sdf_sample(p, j);
		// Distances are in cells of the current cascade: a ray that left its surface by one fine cell is still
		// inside half a coarse cell, so the test restarts after a cascade change or grazing rays hit their own surface.
		if (j != prev_j) {
			left_surface = false;
			prev_j = j;
		}
		left_surface = left_surface || d >= 1.0;
		if (d < 0.5 && left_surface) {
			r_hit = p;
			r_hit.y /= y_mult;
			r_cascade = j;
			return true;
		}
		// Distances near a cascade border do not know about surfaces outside it: never step past the border, the
		// next sample then comes from the coarser cascade.
		vec3 c = (p - sdf.cascades[j].xyz) * sdf.cascades[j].w;
		vec3 inv_dir = 1.0 / (abs(dir) + 1e-6) * sign(dir + 1e-7);
		vec3 t_exit = max((vec3(1.0) - c) * inv_dir, (vec3(sdf.info.x - 1.0) - c) * inv_dir);
		float exit_cells = max(min(t_exit.x, min(t_exit.y, t_exit.z)), 0.0);
		t += min(max(d, 1.0), exit_cells + 0.05) / sdf.cascades[j].w;
	}
	return false;
}

// 1 lit, 0 blocked.
float sdf_shadow(vec3 origin, vec3 dir, float max_dist) {
	vec3 hit;
	int c;
	return sdf_trace(origin, dir, max_dist, hit, c) ? 0.0 : 1.0;
}

// World-space normal of the SDF at a hit point.
vec3 sdf_normal(vec3 p, int j) {
	float y_mult = sdf.info.y;
	p.y *= y_mult;
	float h = 1.0 / sdf.cascades[j].w;
	vec3 g = vec3(
			sdf_sample(p + vec3(h, 0, 0), j) - sdf_sample(p - vec3(h, 0, 0), j),
			sdf_sample(p + vec3(0, h, 0), j) - sdf_sample(p - vec3(0, h, 0), j),
			sdf_sample(p + vec3(0, 0, h), j) - sdf_sample(p - vec3(0, 0, h), j));
	g.y *= y_mult;
	float l = length(g);
	return l > 0.0 ? g / l : vec3(0, 1, 0);
}

float omni_attenuation(float dist, float inv_range, float decay) {
	float nd = dist * inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(dist, 0.0001), -decay);
}

// Position, normal and albedo of a card texel. False when the texel saw no geometry.
bool card_texel(CardData card, ivec2 local, out vec3 r_world, out vec3 r_normal, out vec4 r_albedo, out ivec2 r_texel) {
	r_texel = card.rect.xy + local;
	float depth = texelFetch(sampler2D(depth_tex, nearest_sampler), r_texel, 0).r;
	if (depth <= 0.0 || depth >= card.extents.z) {
		return false;
	}
	r_albedo = texelFetch(sampler2D(albedo_tex, nearest_sampler), r_texel, 0);
	vec3 n_view = texelFetch(sampler2D(normal_tex, nearest_sampler), r_texel, 0).xyz * 2.0 - 1.0;
	vec2 ndc = (vec2(local) + 0.5) / vec2(card.rect.zw) * 2.0 - 1.0;
	ndc.y = -ndc.y; // Atlas rows go down, view y goes up.
	vec3 p_view = vec3(ndc.x * card.extents.x, ndc.y * card.extents.y, -depth);
	r_world = (card.view_to_world * vec4(p_view, 1.0)).xyz;
	r_normal = normalize(mat3(card.view_to_world) * n_view);
	if (n_view.z < 0.0) {
		// Back face captured through a single-sided surface: flip toward the card.
		r_normal = -r_normal;
	}
	return true;
}

// Final lighting of the surface cache at a world point with the given normal; false when no card covers it.
bool sample_cards(vec3 p, vec3 n, float tolerance, out vec3 r_radiance) {
	r_radiance = vec3(0.0);
	ivec3 cell = ivec3(floor((p - params.grid.xyz) / params.grid.w));
	if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(GRID)))) {
		return false;
	}
	uint base = uint((cell.z * GRID + cell.y) * GRID + cell.x) * uint(GRID_SLOTS);
	for (uint s = 0; s < uint(GRID_SLOTS); s++) {
		uint inst_id = object_grid.data[base + s];
		if (inst_id == 0xFFFFFFFFu) {
			break;
		}
		InstanceData inst = instances.data[inst_id];
		vec3 lp = (inst.world_to_local * vec4(p, 1.0)).xyz;
		vec3 size = max(inst.aabb_size.xyz, vec3(0.05));
		vec3 rel = (lp - inst.aabb_min.xyz) / size;
		vec3 margin = vec3(tolerance) / size;
		if (any(lessThan(rel, -margin)) || any(greaterThan(rel, 1.0 + margin))) {
			continue;
		}
		vec3 ln = normalize(mat3(inst.world_to_local) * n);
		vec3 a = abs(ln);
		// Try the three axes in order of alignment with the normal.
		for (int k = 0; k < 3; k++) {
			int axis = a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2);
			a[axis] = -1.0;
			uint card_index = inst.info.x + uint(axis * 2 + (ln[axis] < 0.0 ? 1 : 0));
			CardData c = cards.data[card_index];
			if (c.rect.z == 0) {
				continue;
			}
			vec3 v = (c.world_to_view * vec4(p, 1.0)).xyz;
			vec2 uv = vec2(v.x / c.extents.x, -v.y / c.extents.y) * 0.5 + 0.5;
			if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) {
				continue;
			}
			ivec2 t = c.rect.xy + clamp(ivec2(uv * vec2(c.rect.zw)), ivec2(0), c.rect.zw - 1);
			float cd = texelFetch(sampler2D(depth_tex, nearest_sampler), t, 0).r;
			if (cd <= 0.0 || cd >= c.extents.z) {
				continue;
			}
			float texel_size = 2.0 * c.extents.x / float(c.rect.z);
			if (abs(cd - (-v.z)) > max(tolerance, 2.0 * texel_size)) {
				continue; // The card sees another surface here.
			}
			r_radiance = imageLoad(final_image, t).rgb;
			return true;
		}
	}
	return false;
}

float hash1(vec3 p) {
	p = fract(p * vec3(0.1031, 0.1030, 0.0973));
	p += dot(p, p.yxz + 33.33);
	return fract((p.x + p.y) * p.z);
}

void main() {
#if defined(MODE_SCREEN_PROBES) || defined(MODE_SCREEN_RESOLVE) || defined(MODE_SCREEN_REFLECT)
	ivec2 local = ivec2(gl_GlobalInvocationID.xy);
#else
	CardData card = cards.data[push.card];
	ivec2 local = ivec2(gl_GlobalInvocationID.xy);
#endif

#ifdef MODE_SCREEN_PROBES
	ivec2 probe_count = (ivec2(screen.size.xy) + SCREEN_PROBE_SIZE - 1) / SCREEN_PROBE_SIZE;
	if (any(greaterThanEqual(local, probe_count))) {
		return;
	}
	ivec2 pixel = min(local * SCREEN_PROBE_SIZE + SCREEN_PROBE_SIZE / 2, ivec2(screen.size.xy) - 1);
	vec3 view_pos, view_normal;
	if (!screen_position(pixel, view_pos, view_normal)) {
		imageStore(screen_probe_image, local, vec4(0.0));
		return;
	}
	vec3 world = (screen.cam_transform * vec4(view_pos, 1.0)).xyz;
	vec3 normal = normalize(mat3(screen.cam_transform) * view_normal);
	vec3 origin = world + normal * 0.02;
	vec3 tangent = normalize(abs(normal.y) < 0.99 ? cross(normal, vec3(0, 1, 0)) : cross(normal, vec3(1, 0, 0)));
	vec3 bitangent = cross(normal, tangent);
	float frame = screen.misc.y;
	vec3 sum = vec3(0.0);
	int hits = 0;
	int found = 0;
	for (int r = 0; r < SCREEN_PROBE_RAYS; r++) {
		// Stratified over a 4x4 grid, jittered per frame.
		vec2 cell = vec2(r % 4, r / 4);
		vec2 u = (cell + vec2(hash1(vec3(local, frame * 16.0 + float(r))), hash1(vec3(local.yx, frame * 16.0 + float(r) + 17.0)))) / 4.0;
		float phi = 2.0 * M_PI * u.x;
		float sr = sqrt(u.y);
		vec3 dir = tangent * (cos(phi) * sr) + bitangent * (sin(phi) * sr) + normal * sqrt(max(0.0, 1.0 - u.y));
		vec3 hit;
		int cascade;
		vec3 radiance = sky_radiance(dir);
		vec3 s_hit, s_normal;
		bool traced = false;
		if (screen_trace(view_pos + view_normal * 0.02, transpose(mat3(screen.cam_transform)) * dir, s_hit, s_normal)) {
			traced = sample_cards(s_hit, s_normal, 3.0 / sdf.cascades[0].w, radiance);
		}
		if (!traced && sdf_trace(origin, dir, 100.0, hit, cascade)) {
			hits++;
			vec3 hn = sdf_normal(hit, cascade);
			float cell_size = 1.0 / sdf.cascades[cascade].w;
			if (sample_cards(hit, hn, 1.5 * cell_size, radiance)) {
				found++;
			} else {
				radiance = vec3(0.0);
			}
		}
		sum += radiance;
	}
	vec3 mean = sum / float(SCREEN_PROBE_RAYS);
	if (push.debug == 3u) {
		// Diagnostics: hit fraction, card fraction, mean radiance.
		imageStore(screen_probe_image, local, vec4(float(hits) / float(SCREEN_PROBE_RAYS), float(found) / float(SCREEN_PROBE_RAYS), dot(mean, vec3(1.0 / 3.0)), -view_pos.z));
		return;
	}
	float depth = -view_pos.z;
	// Keep history while the reprojected probe lies on the same surface.
	vec4 prev = vec4(0.0);
	ivec2 prev_texel;
	float prev_depth;
	if (reproject(world, imageSize(screen_probe_prev), prev_texel, prev_depth)) {
		prev = imageLoad(screen_probe_prev, prev_texel);
		if (abs(prev.a - prev_depth) > 0.05 * prev_depth) {
			prev.a = 0.0;
		}
	}
	float w = prev.a > 0.0 ? 0.125 : 1.0;
	imageStore(screen_probe_image, local, vec4(mix(prev.rgb, mean, w), depth));
#endif

#ifdef MODE_SCREEN_REFLECT
	// One glossy ray per reflection-buffer pixel: reflected view direction jittered inside a cone that grows with
	// roughness, traced through the SDF into the surface cache, sky on miss. Rough surfaces keep the probe result.
	if (any(greaterThanEqual(local, ivec2(screen.size.zw)))) {
		return;
	}
	ivec2 pixel = ivec2((vec2(local) + 0.5) * screen.size.xy / screen.size.zw);
	vec3 view_pos, view_normal;
	if (!screen_position(pixel, view_pos, view_normal)) {
		imageStore(reflection_image, local, vec4(0.0));
		imageStore(reflection_history, local, vec4(0.0));
		return;
	}
	float roughness = screen_roughness(pixel);
	float weight = 1.0 - smoothstep(0.5, 0.8, roughness);
	if (weight <= 0.0) {
		imageStore(reflection_image, local, vec4(0.0));
		imageStore(reflection_history, local, vec4(0.0));
		return;
	}
	vec3 world = (screen.cam_transform * vec4(view_pos, 1.0)).xyz;
	vec3 normal = normalize(mat3(screen.cam_transform) * view_normal);
	vec3 view_dir = normalize(mat3(screen.cam_transform) * view_pos);
	vec3 refl = reflect(view_dir, normal);
	float frame = screen.misc.y;
	vec2 u = vec2(hash1(vec3(local, frame)), hash1(vec3(local.yx, frame + 17.0)));
	// Cone around the reflection: half angle ~ roughness^2 * 60 degrees.
	float cone = roughness * roughness * 1.05;
	float phi = 2.0 * M_PI * u.x;
	float ct = mix(1.0, cos(cone), u.y);
	float st = sqrt(max(0.0, 1.0 - ct * ct));
	vec3 rt = normalize(abs(refl.y) < 0.99 ? cross(refl, vec3(0, 1, 0)) : cross(refl, vec3(1, 0, 0)));
	vec3 rb = cross(refl, rt);
	vec3 dir = normalize(rt * (cos(phi) * st) + rb * (sin(phi) * st) + refl * ct);
	if (dot(dir, normal) <= 0.0) {
		dir = refl;
	}
	vec3 origin = world + normal * 0.02;
	vec3 hit;
	int cascade;
	vec3 radiance = sky_radiance(dir);
	bool found = false;
	vec3 s_hit, s_normal;
	bool screen_hit = false;
	if (screen_trace(view_pos + view_normal * 0.02, transpose(mat3(screen.cam_transform)) * dir, s_hit, s_normal)) {
		found = sample_cards(s_hit, s_normal, 3.0 / sdf.cascades[0].w, radiance);
		screen_hit = found;
		hit = s_hit;
		cascade = 0;
	}
	bool hit_any = found || sdf_trace(origin, dir, 200.0, hit, cascade);
	if (hit_any && !found) {
		vec3 hn = sdf_normal(hit, cascade);
		float cell_size = 1.0 / sdf.cascades[cascade].w;
		found = sample_cards(hit, hn, 1.5 * cell_size, radiance);
		if (!found) {
			radiance = vec3(0.0);
		}
	}
	if (push.debug == 3u) {
		// Diagnostics: hit cascade (+1)/8, hit distance/32, card found, hit height (+8)/16 in alpha.
		imageStore(reflection_image, local, vec4(screen_hit ? 1.0 : (hit_any ? float(cascade + 1) / 8.0 : 0.0), hit_any ? length(hit - origin) / 32.0 : 1.0, found ? 1.0 : 0.0, hit_any ? (hit.y + 8.0) / 16.0 : 0.0));
		return;
	}
	float depth = -view_pos.z;
	vec4 prev = vec4(0.0);
	ivec2 prev_texel;
	float prev_depth;
	if (reproject(world, imageSize(reflection_history_prev), prev_texel, prev_depth)) {
		prev = imageLoad(reflection_history_prev, prev_texel);
		if (abs(prev.a - prev_depth) > 0.05 * prev_depth) {
			prev.a = 0.0;
		}
	}
	float w = prev.a > 0.0 ? mix(0.5, 0.1, clamp(roughness * 2.0, 0.0, 1.0)) : 1.0;
	vec3 acc = mix(prev.rgb, radiance, w);
	imageStore(reflection_history, local, vec4(acc, depth));
	imageStore(reflection_image, local, vec4(acc, weight));
#endif

#ifdef MODE_SCREEN_RESOLVE
	if (any(greaterThanEqual(local, ivec2(screen.size.zw)))) {
		return;
	}
	ivec2 pixel = ivec2((vec2(local) + 0.5) * screen.size.xy / screen.size.zw);
	vec3 view_pos, view_normal;
	if (!screen_position(pixel, view_pos, view_normal)) {
		imageStore(ambient_image, local, vec4(0.0));
		return;
	}
	float depth = -view_pos.z;
	ivec2 probe_count = (ivec2(screen.size.xy) + SCREEN_PROBE_SIZE - 1) / SCREEN_PROBE_SIZE;
	vec2 pf = (vec2(pixel) + 0.5) / float(SCREEN_PROBE_SIZE) - 0.5;
	ivec2 p0 = clamp(ivec2(floor(pf)), ivec2(0), probe_count - 1);
	ivec2 p1 = min(p0 + 1, probe_count - 1);
	vec2 f = clamp(pf - vec2(p0), 0.0, 1.0);
	vec3 sum = vec3(0.0);
	float wsum = 0.0;
	for (int i = 0; i < 4; i++) {
		ivec2 pc = ivec2(i % 2 == 0 ? p0.x : p1.x, i / 2 == 0 ? p0.y : p1.y);
		vec4 probe = imageLoad(screen_probe_image, pc);
		if (probe.a <= 0.0) {
			continue;
		}
		float w = (i % 2 == 0 ? 1.0 - f.x : f.x) * (i / 2 == 0 ? 1.0 - f.y : f.y);
		w *= 1.0 / (1.0 + 8.0 * abs(probe.a - depth) / depth); // Depth-aware weight.
		sum += probe.rgb * w;
		wsum += w;
	}
	imageStore(ambient_image, local, wsum > 0.0 ? vec4(sum / wsum, 1.0) : vec4(0.0));
#endif

#ifdef MODE_DIRECT
	if (any(greaterThanEqual(local, card.rect.zw))) {
		return;
	}
	vec3 world, normal;
	vec4 albedo;
	ivec2 texel;
	if (!card_texel(card, local, world, normal, albedo, texel)) {
		imageStore(direct_image, card.rect.xy + local, vec4(0.0));
		return;
	}
	vec3 emission = texelFetch(sampler2D(emission_tex, nearest_sampler), texel, 0).rgb;
	vec3 light = vec3(0.0);
	mat3 view_rot = mat3(params.view_to_world);
	vec3 origin = world + normal * 0.02;

	if (push.debug == 1u) {
		// Diagnostics: cascade containing the texel, SDF distance there (cells / 16), shadow toward the first light.
		vec3 so = origin;
		so.y *= sdf.info.y;
		int j = find_cascade(so);
		float d = j >= 0 ? sdf_sample(so, j) : -1.0;
		float sh = params.counts.x > 0u ? sdf_shadow(origin, normalize(view_rot * directional_lights.data[0].direction), 100.0) : -1.0;
		imageStore(direct_image, texel, vec4(float(j + 1) / 8.0, d / 16.0, sh, 1.0));
		return;
	}

	for (uint i = 0; i < params.counts.x; i++) {
		vec3 l = normalize(view_rot * directional_lights.data[i].direction);
		float ndotl = dot(normal, l);
		if (ndotl <= 0.0) {
			continue;
		}
		float shadow = 1.0;
		if (directional_lights.data[i].shadow_opacity > 0.0) {
			shadow = mix(1.0, sdf_shadow(origin, l, 100.0), directional_lights.data[i].shadow_opacity);
		}
		light += directional_lights.data[i].color * directional_lights.data[i].energy * ndotl * shadow;
	}
	for (uint i = 0; i < params.counts.y; i++) {
		vec3 lp = (params.view_to_world * vec4(omni_lights.data[i].position, 1.0)).xyz;
		vec3 to_l = lp - world;
		float dist = length(to_l);
		if (dist * omni_lights.data[i].inv_radius > 1.0) {
			continue;
		}
		vec3 l = to_l / dist;
		float ndotl = dot(normal, l);
		if (ndotl <= 0.0) {
			continue;
		}
		float shadow = 1.0;
		if (omni_lights.data[i].shadow_opacity > 0.0) {
			shadow = mix(1.0, sdf_shadow(origin, l, dist), omni_lights.data[i].shadow_opacity);
		}
		light += omni_lights.data[i].color * omni_attenuation(dist, omni_lights.data[i].inv_radius, omni_lights.data[i].attenuation) * ndotl * shadow;
	}
	for (uint i = 0; i < params.counts.z; i++) {
		vec3 lp = (params.view_to_world * vec4(spot_lights.data[i].position, 1.0)).xyz;
		vec3 to_l = lp - world;
		float dist = length(to_l);
		if (dist * spot_lights.data[i].inv_radius > 1.0) {
			continue;
		}
		vec3 l = to_l / dist;
		float ndotl = dot(normal, l);
		if (ndotl <= 0.0) {
			continue;
		}
		vec3 spot_dir = normalize(view_rot * spot_lights.data[i].direction);
		float cos_angle = dot(-l, spot_dir);
		float cone = spot_lights.data[i].cone_angle;
		if (cos_angle < cone) {
			continue;
		}
		float scos = max(cos_angle, cone);
		float spot_rim = max(0.0001, (1.0 - scos) / (1.0 - cone));
		float spot_att = 1.0 - pow(spot_rim, spot_lights.data[i].cone_attenuation);
		float shadow = 1.0;
		if (spot_lights.data[i].shadow_opacity > 0.0) {
			shadow = mix(1.0, sdf_shadow(origin, l, dist), spot_lights.data[i].shadow_opacity);
		}
		light += spot_lights.data[i].color * omni_attenuation(dist, spot_lights.data[i].inv_radius, spot_lights.data[i].attenuation) * spot_att * ndotl * shadow;
	}
	imageStore(direct_image, texel, vec4(albedo.rgb * light / M_PI + emission, 1.0));
#endif

#ifdef MODE_PROBES
	ivec2 probe_count = card.rect.zw / 4;
	if (any(greaterThanEqual(local, probe_count))) {
		return;
	}
	ivec2 probe_texel = card.rect.xy / 4 + local;
	// First texel of the 4x4 block that saw geometry hosts the probe.
	vec3 world, normal;
	vec4 albedo;
	ivec2 texel;
	bool valid = false;
	for (int i = 0; i < 16 && !valid; i++) {
		valid = card_texel(card, local * 4 + ivec2(i % 4, i / 4), world, normal, albedo, texel);
	}
	if (!valid) {
		imageStore(probe_image, probe_texel, vec4(0.0));
		return;
	}
	vec3 origin = world + normal * 0.02;
	vec3 tangent = normalize(abs(normal.y) < 0.99 ? cross(normal, vec3(0, 1, 0)) : cross(normal, vec3(1, 0, 0)));
	vec3 bitangent = cross(normal, tangent);
	vec3 sum = vec3(0.0);
	float frame = params.sky.w;
	int hits = 0;
	int found = 0;
	for (int r = 0; r < PROBE_RAYS; r++) {
		vec2 u = vec2(hash1(vec3(probe_texel, frame * float(PROBE_RAYS) + float(r))), hash1(vec3(probe_texel.yx, frame * float(PROBE_RAYS) + float(r) + 17.0)));
		// Cosine-weighted hemisphere.
		float phi = 2.0 * M_PI * u.x;
		float sr = sqrt(u.y);
		vec3 dir = tangent * (cos(phi) * sr) + bitangent * (sin(phi) * sr) + normal * sqrt(max(0.0, 1.0 - u.y));
		vec3 hit;
		int cascade;
		vec3 radiance = sky_radiance(dir);
		if (sdf_trace(origin, dir, 100.0, hit, cascade)) {
			hits++;
			vec3 hn = sdf_normal(hit, cascade);
			float cell = 1.0 / sdf.cascades[cascade].w;
			if (sample_cards(hit, hn, 1.5 * cell, radiance)) {
				found++;
			} else {
				radiance = vec3(0.0); // Hit something without a card: unknown, treat as dark.
			}
		}
		sum += radiance;
	}
	// Cosine sampling: irradiance = pi * mean radiance; the resolve multiplies by albedo / pi, so store the mean.
	vec3 mean = sum / float(PROBE_RAYS);
	if (push.debug == 2u) {
		// Diagnostics: fraction of rays that hit the SDF, fraction that found a card, mean radiance.
		imageStore(probe_image, probe_texel, vec4(float(hits) / float(PROBE_RAYS), float(found) / float(PROBE_RAYS), dot(mean, vec3(1.0 / 3.0)), 1.0));
		return;
	}
	vec4 prev = imageLoad(probe_image, probe_texel);
	float w = prev.a > 0.0 ? 0.25 : 1.0;
	imageStore(probe_image, probe_texel, vec4(mix(prev.rgb, mean, w), 1.0));
#endif

#ifdef MODE_RESOLVE
	if (any(greaterThanEqual(local, card.rect.zw))) {
		return;
	}
	ivec2 texel = card.rect.xy + local;
	vec4 direct = imageLoad(direct_image, texel);
	if (direct.a <= 0.0) {
		imageStore(final_image, texel, vec4(0.0));
		return;
	}
	vec4 albedo = texelFetch(sampler2D(albedo_tex, nearest_sampler), texel, 0);
	ivec2 probe_count = card.rect.zw / 4;
	ivec2 probe_base = card.rect.xy / 4;
	vec2 pf = (vec2(local) - 1.5) / 4.0;
	ivec2 p0 = clamp(ivec2(floor(pf)), ivec2(0), probe_count - 1);
	ivec2 p1 = min(p0 + 1, probe_count - 1);
	vec2 f = clamp(pf - vec2(p0), 0.0, 1.0);
	vec4 s00 = imageLoad(probe_image, probe_base + p0);
	vec4 s10 = imageLoad(probe_image, probe_base + ivec2(p1.x, p0.y));
	vec4 s01 = imageLoad(probe_image, probe_base + ivec2(p0.x, p1.y));
	vec4 s11 = imageLoad(probe_image, probe_base + p1);
	float w00 = (1.0 - f.x) * (1.0 - f.y) * s00.a;
	float w10 = f.x * (1.0 - f.y) * s10.a;
	float w01 = (1.0 - f.x) * f.y * s01.a;
	float w11 = f.x * f.y * s11.a;
	float wsum = w00 + w10 + w01 + w11;
	vec3 irradiance = wsum > 0.0 ? (s00.rgb * w00 + s10.rgb * w10 + s01.rgb * w01 + s11.rgb * w11) / wsum : vec3(0.0);
	imageStore(final_image, texel, vec4(direct.rgb + albedo.rgb * irradiance, 1.0));
#endif
}

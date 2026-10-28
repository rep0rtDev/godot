#[compute]

#version 450

#VERSION_DEFINES

// Surface cache direct lighting: one dispatch per card, one thread per atlas texel. World position and normal come
// from the captured depth and view-space normal of the card. Lights come from the scene light buffers (camera view
// space) and shadows from sphere tracing the SDFGI cascades when present.

#define MAX_CASCADES 8

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../light_data_inc.glsl"

#define M_PI 3.14159265359

struct CardData {
	ivec4 rect;
	mat4 view_to_world; // Capture camera to world.
	vec4 extents; // half width, half height, depth range, unused.
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Cards {
	CardData data[];
}
cards;

layout(set = 0, binding = 1) uniform texture2D albedo_tex;
layout(set = 0, binding = 2) uniform texture2D normal_tex;
layout(set = 0, binding = 3) uniform texture2D depth_tex;
layout(set = 0, binding = 4) uniform texture2D emission_tex;
layout(set = 0, binding = 5, rgba16f) uniform restrict writeonly image2D direct_image;

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
}
params;

layout(set = 0, binding = 10) uniform sampler linear_sampler;
layout(set = 0, binding = 11) uniform sampler nearest_sampler;

// SDFGI cascades as a global SDF. info: grid size in cells, y_mult, cascade count. cascades: world offset of cell
// (0,0,0) in y-scaled space and 1 / cell size.
layout(set = 1, binding = 0, std140) uniform Sdf {
	vec4 info;
	vec4 cascades[MAX_CASCADES];
}
sdf;

layout(set = 1, binding = 1) uniform texture3D sdf_cascades[MAX_CASCADES];

layout(push_constant, std430) uniform Push {
	uint card;
	uint debug;
	uint pad[2];
}
push;

// Finest cascade containing p, or -1.
int find_cascade(vec3 p) {
	for (uint j = 0; j < uint(sdf.info.z); j++) {
		vec3 c = (p - sdf.cascades[j].xyz) * sdf.cascades[j].w;
		if (all(greaterThanEqual(c, vec3(1.0))) && all(lessThan(c, vec3(sdf.info.x - 1.0)))) {
			return int(j);
		}
	}
	return -1;
}

// 1 lit, 0 blocked. Sphere tracing through the SDF cascades from origin toward dir up to max_dist.
float sdf_shadow(vec3 origin, vec3 dir, float max_dist) {
	if (params.counts.w == 0u) {
		return 1.0;
	}
	// The SDF is built with world y scaled by y_mult.
	origin.y *= sdf.info.y;
	dir.y *= sdf.info.y;
	float dir_len = length(dir);
	dir /= dir_len;
	max_dist *= dir_len;
	int j = find_cascade(origin);
	if (j < 0) {
		return 1.0;
	}
	// Leave the surface: two cells of the finest cascade.
	float t = 2.0 / sdf.cascades[j].w;
	for (int i = 0; i < 64 && t < max_dist; i++) {
		vec3 p = origin + dir * t;
		j = find_cascade(p);
		if (j < 0) {
			return 1.0;
		}
		vec3 uvw = (p - sdf.cascades[j].xyz) * sdf.cascades[j].w / sdf.info.x;
		float d = texture(sampler3D(sdf_cascades[j], linear_sampler), uvw).r * 255.0 - 1.0;
		if (d < 0.5) {
			return 0.0;
		}
		t += max(d, 1.0) / sdf.cascades[j].w;
	}
	return 1.0;
}

float omni_attenuation(float dist, float inv_range, float decay) {
	float nd = dist * inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(dist, 0.0001), -decay);
}

void main() {
	CardData card = cards.data[push.card];
	ivec2 local = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(local, card.rect.zw))) {
		return;
	}
	ivec2 texel = card.rect.xy + local;
	vec4 albedo = texelFetch(sampler2D(albedo_tex, nearest_sampler), texel, 0);
	float depth = texelFetch(sampler2D(depth_tex, nearest_sampler), texel, 0).r;
	if (depth <= 0.0 || depth >= card.extents.z) {
		imageStore(direct_image, texel, vec4(0.0));
		return;
	}
	vec3 n_view = texelFetch(sampler2D(normal_tex, nearest_sampler), texel, 0).xyz * 2.0 - 1.0;
	vec2 ndc = (vec2(local) + 0.5) / vec2(card.rect.zw) * 2.0 - 1.0;
	ndc.y = -ndc.y; // Atlas rows go down, view y goes up.
	vec3 p_view = vec3(ndc.x * card.extents.x, ndc.y * card.extents.y, -depth);
	vec3 world = (card.view_to_world * vec4(p_view, 1.0)).xyz;
	vec3 normal = normalize(mat3(card.view_to_world) * n_view);
	if (normal.z < 0.0 && n_view.z < 0.0) {
		// Back face captured through a single-sided surface: flip toward the card.
		normal = -normal;
	}
	vec3 emission = texelFetch(sampler2D(emission_tex, nearest_sampler), texel, 0).rgb;
	vec3 light = vec3(0.0);
	mat3 view_rot = mat3(params.view_to_world);
	vec3 origin = world + normal * 0.02;

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
	if (push.debug != 0u) {
		// Diagnostics: cascade containing the texel, SDF distance there (cells / 16), shadow toward the first light.
		vec3 so = origin;
		so.y *= sdf.info.y;
		int j = find_cascade(so);
		float d = -1.0;
		if (j >= 0) {
			vec3 uvw = (so - sdf.cascades[j].xyz) * sdf.cascades[j].w / sdf.info.x;
			d = texture(sampler3D(sdf_cascades[j], linear_sampler), uvw).r * 255.0 - 1.0;
		}
		float sh = params.counts.x > 0u ? sdf_shadow(origin, normalize(view_rot * directional_lights.data[0].direction), 100.0) : -1.0;
		imageStore(direct_image, texel, vec4(float(j + 1) / 8.0, d / 16.0, sh, 1.0));
		return;
	}
	imageStore(direct_image, texel, vec4(albedo.rgb * light / M_PI + emission, 1.0));
}

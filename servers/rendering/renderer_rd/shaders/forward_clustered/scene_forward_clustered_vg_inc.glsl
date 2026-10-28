// Virtual geometry material pass.
// Rebuilds, for the pixel's software rasterized triangle, the same outputs the vertex stage would interpolate
// for it (default transform path only), so the unmodified material fragment code can shade it.

struct VGCluster {
	vec4 cull_sphere;
	vec4 self_sphere;
	vec4 parent_sphere;
	float self_error;
	float parent_error;
	uint geometry_offset;
	uint page_and_triangles;
};

layout(set = 4, binding = 0, std430) restrict readonly buffer VGVisibility {
	uvec2 data[]; // x: (entry << 7) | triangle, y: depth bits; 0 means empty.
}
vg_visibility;

layout(set = 4, binding = 1, std430) restrict readonly buffer VGEntries {
	uint count;
	uint pad;
	uvec2 data[]; // (surface slot, cluster).
}
vg_entries;

layout(set = 4, binding = 2, std430) restrict readonly buffer VGClusters {
	VGCluster data[];
}
vg_clusters;

layout(set = 4, binding = 3, std430) restrict readonly buffer VGPool {
	uint data[];
}
vg_pool;

layout(set = 4, binding = 4, std430) restrict readonly buffer VGPages {
	uint data[];
}
vg_pages;

#define VG_POOL(w) vg_pool.data[w]
#define VG_PAGE_BASE(p) vg_pages.data[p]
#include "../effects/virtual_geometry_geometry_inc.glsl"

layout(set = 4, binding = 5, std140) uniform VGSurface {
	uvec2 size;
	uint slot;
	uint pad;
	float depth_bias;
	uint pad1;
	uint pad2;
	uint pad3;
}
vg_surface;

// Fragment-stage copy of the vertex shader's axis_angle_to_tbn.
void vg_axis_angle_to_tbn(vec3 axis, float angle, out vec3 tangent, out vec3 binormal, out vec3 normal) {
	float c = cos(angle);
	float s = sin(angle);
	vec3 omc_axis = (1.0 - c) * axis;
	vec3 s_axis = s * axis;
	tangent = omc_axis.xxx * axis + vec3(c, -s_axis.z, s_axis.y);
	binormal = omc_axis.yyy * axis + vec3(s_axis.z, c, -s_axis.x);
	normal = omc_axis.zzz * axis + vec3(-s_axis.y, s_axis.x, c);
}

float vg_cross2(vec2 a, vec2 b) {
	return a.x * b.y - a.y * b.x;
}

bool vg_pixel_visible(ivec2 p_pixel, out uvec2 r_visibility, out uvec2 r_entry) {
	if (any(lessThan(p_pixel, ivec2(0))) || any(greaterThanEqual(p_pixel, ivec2(vg_surface.size)))) {
		return false;
	}
	r_visibility = vg_visibility.data[uint(p_pixel.y) * vg_surface.size.x + uint(p_pixel.x)];
	if (r_visibility.y == 0u) {
		return false;
	}
	r_entry = vg_entries.data[r_visibility.x >> 7u];
	return r_entry.x == vg_surface.slot;
}

#define VG_NO_TRIANGLE 0xFFFFFFFFu

// Distinct triangles of this surface inside the pixel's 2x2 quad, as (visibility id, cluster). Every lane of
// the quad reads the same four pixels, so all lanes agree on the list and its order, and the material can be
// run once per triangle with the whole quad extrapolating that triangle, like a hardware quad. That keeps
// texture derivatives valid across silhouettes and UV seams.
uvec2 vg_quad_triangles[4];
uint vg_own_triangle = VG_NO_TRIANGLE;

uint vg_quad_setup() {
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	ivec2 base = pixel & ~ivec2(1);
	uint count = 0u;
	gl_FragDepth = 0.0;
	for (int i = 0; i < 4; i++) {
		ivec2 q = base + ivec2(i & 1, i >> 1);
		uvec2 visibility;
		uvec2 entry;
		if (!vg_pixel_visible(q, visibility, entry)) {
			continue;
		}
		if (q == pixel) {
			vg_own_triangle = visibility.x;
			// Same value the depth resolve wrote; the GREATER_OR_EQUAL test rejects pixels where other geometry is in front.
			gl_FragDepth = uintBitsToFloat(visibility.y) * (1.0 - vg_surface.depth_bias);
		}
		bool found = false;
		for (uint j = 0u; j < count; j++) {
			found = found || vg_quad_triangles[j].x == visibility.x;
		}
		if (!found) {
			vg_quad_triangles[count] = uvec2(visibility.x, entry.y);
			count++;
		}
	}
	if (count == 0u) {
		discard;
	}
	return count;
}

// Rebuilds the interpolated vertex outputs of triangle p_triangle at this pixel (extrapolated if it lies outside).
void vg_material_eval(in SceneData scene_data, uvec2 p_triangle) {
	VGCluster cluster = vg_clusters.data[p_triangle.y];
	VGGeometry geometry = vg_load_geometry(cluster.geometry_offset, cluster.page_and_triangles >> 8u);
	uint corner = (p_triangle.x & 127u) * 3u;
	uint ids[3] = uint[](vg_geometry_index(geometry, corner), vg_geometry_index(geometry, corner + 1u), vg_geometry_index(geometry, corner + 2u));
	uint instance_index = draw_call.instance_index;
	instance_index_interp = instance_index;

	mat4 model_matrix = transpose(mat4(instances.data[instance_index].transform[0],
			instances.data[instance_index].transform[1],
			instances.data[instance_index].transform[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	mat3 model_normal_matrix;
	if (bool(instances.data[instance_index].flags & INSTANCE_FLAGS_NON_UNIFORM_SCALE)) {
		model_normal_matrix = transpose(inverse(mat3(model_matrix)));
	} else {
		model_normal_matrix = mat3(model_matrix);
	}
	mat4 read_view_matrix = transpose(mat4(scene_data.view_matrix[0],
			scene_data.view_matrix[1],
			scene_data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	mat4 modelview = read_view_matrix * model_matrix;
	mat3 modelview_normal = mat3(read_view_matrix) * model_normal_matrix;

	vec3 view_pos[3];
	vec2 screen[3];
	vec3 inv_w;
	for (int i = 0; i < 3; i++) {
		view_pos[i] = (modelview * vec4(vg_geometry_position(geometry, ids[i]), 1.0)).xyz;
		vec4 clip = projection_matrix * vec4(view_pos[i], 1.0);
		inv_w[i] = 1.0 / clip.w;
		screen[i] = clip.xy * inv_w[i];
	}

	// Perspective-correct barycentrics of the pixel center, as hardware interpolation would produce.
	vec2 p = gl_FragCoord.xy / vec2(vg_surface.size) * 2.0 - 1.0;
	vec3 b = vec3(vg_cross2(screen[1] - p, screen[2] - p), vg_cross2(screen[2] - p, screen[0] - p), vg_cross2(screen[0] - p, screen[1] - p));
	b *= inv_w;
	b /= b.x + b.y + b.z;

	vertex_interp = view_pos[0] * b.x + view_pos[1] * b.y + view_pos[2] * b.z;
	// Godot front faces wind clockwise as seen by the camera.
	vg_front_facing = dot(cross(view_pos[1] - view_pos[0], view_pos[2] - view_pos[0]), view_pos[0]) > 0.0;

#if defined(NORMAL_USED) || defined(TANGENT_USED)
	vec3 normal_sum = vec3(0.0);
	vec3 tangent_sum = vec3(0.0);
	vec3 binormal_sum = vec3(0.0);
	for (int i = 0; i < 3; i++) {
		// The tangent frame is a rotation of the frame (x, y, z) by an axis and angle, like compressed attributes.
		vec3 axis = oct_to_vec3(vg_geometry_axis(geometry, ids[i]) * 2.0 - 1.0);
		float angle = vg_geometry_angle(geometry, ids[i]);
		float binormal_sign = angle > 0.5 ? 1.0 : -1.0;
		angle = abs(angle * 2.0 - 1.0) * M_PI;
		vec3 n;
		vec3 t;
		vec3 bn;
		vg_axis_angle_to_tbn(axis, angle, t, bn, n);
		bn *= binormal_sign;
		normal_sum += normalize(modelview_normal * n) * b[i];
		tangent_sum += normalize(mat3(modelview) * t) * b[i];
		binormal_sum += normalize(mat3(modelview) * bn) * b[i];
	}
#ifdef NORMAL_USED
	normal_interp = normal_sum;
#endif
#ifdef TANGENT_USED
	tangent_interp = tangent_sum;
	binormal_interp = binormal_sum;
#endif
#endif

#ifdef UV_USED
	uv_interp = vec2(0.0);
	if ((geometry.flags & VG_GEOMETRY_FLAG_UV) != 0u) {
		uv_interp = vg_geometry_uv(geometry, ids[0]) * b.x + vg_geometry_uv(geometry, ids[1]) * b.y + vg_geometry_uv(geometry, ids[2]) * b.z;
	}
#endif
#if defined(UV2_USED) || defined(USE_LIGHTMAP)
	uv2_interp = vec2(0.0);
	if ((geometry.flags & VG_GEOMETRY_FLAG_UV2) != 0u) {
		uv2_interp = vg_geometry_uv2(geometry, ids[0]) * b.x + vg_geometry_uv2(geometry, ids[1]) * b.y + vg_geometry_uv2(geometry, ids[2]) * b.z;
	}
#endif
#if defined(COLOR_USED)
	color_interp = vec4(1.0);
	if ((geometry.flags & VG_GEOMETRY_FLAG_COLOR) != 0u) {
		color_interp = vg_geometry_color(geometry, ids[0]) * b.x + vg_geometry_color(geometry, ids[1]) * b.y + vg_geometry_color(geometry, ids[2]) * b.z;
	}
#endif
}

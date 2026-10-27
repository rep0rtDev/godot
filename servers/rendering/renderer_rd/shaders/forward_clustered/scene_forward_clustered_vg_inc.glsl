// Virtual geometry material pass.
// Rebuilds, for the pixel's software rasterized triangle, the same outputs the vertex stage would interpolate
// for it (default transform path only), so the unmodified material fragment code can shade it.

struct VGCluster {
	vec4 cull_sphere;
	vec4 self_sphere;
	vec4 parent_sphere;
	float self_error;
	float parent_error;
	uint index_offset;
	uint index_count;
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

layout(set = 4, binding = 3, std430) restrict readonly buffer VGIndices {
	uint data[];
}
vg_indices;

layout(set = 4, binding = 4, std430) restrict readonly buffer VGVertices {
	uint data[]; // Positions (float3), then the normal/tangent stream.
}
vg_vertices;

layout(set = 4, binding = 5, std430) restrict readonly buffer VGAttributes {
	uint data[];
}
vg_attributes;

#define VG_FLAG_INDEX_16 1u
#define VG_FLAG_TANGENT 2u
#define VG_FLAG_COLOR 4u
#define VG_FLAG_UV 8u
#define VG_FLAG_UV2 16u

layout(set = 4, binding = 6, std140) uniform VGSurface {
	uvec2 size;
	uint slot;
	uint flags;
	uint vertex_count;
	uint normal_stride; // Bytes per vertex in the normal/tangent stream.
	uint attrib_stride; // Bytes per vertex in the attribute buffer.
	uint color_offset;
	uint uv_offset;
	uint uv2_offset;
	float depth_bias;
	uint normal_offset;
}
vg_surface;

uint vg_fetch_index(uint p_index) {
	if ((vg_surface.flags & VG_FLAG_INDEX_16) != 0u) {
		uint word = vg_indices.data[p_index >> 1u];
		return (p_index & 1u) != 0u ? word >> 16u : word & 0xFFFFu;
	}
	return vg_indices.data[p_index];
}

vec3 vg_position(uint p_vertex) {
	uint base = p_vertex * 3u;
	return vec3(uintBitsToFloat(vg_vertices.data[base]), uintBitsToFloat(vg_vertices.data[base + 1u]), uintBitsToFloat(vg_vertices.data[base + 2u]));
}

vec2 vg_attribute_vec2(uint p_vertex, uint p_offset) {
	uint word = (p_vertex * vg_surface.attrib_stride + p_offset) >> 2u;
	return vec2(uintBitsToFloat(vg_attributes.data[word]), uintBitsToFloat(vg_attributes.data[word + 1u]));
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
	uint first = cluster.index_offset + (p_triangle.x & 127u) * 3u;
	uint ids[3] = uint[](vg_fetch_index(first), vg_fetch_index(first + 1u), vg_fetch_index(first + 2u));

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
		view_pos[i] = (modelview * vec4(vg_position(ids[i]), 1.0)).xyz;
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
		uint word = (vg_surface.normal_offset + ids[i] * vg_surface.normal_stride) >> 2u;
		vec3 n = oct_to_vec3(unpackUnorm2x16(vg_vertices.data[word]) * 2.0 - 1.0);
		vec3 t;
		vec3 bn;
		if ((vg_surface.flags & VG_FLAG_TANGENT) != 0u) {
			vec2 signed_tangent = unpackUnorm2x16(vg_vertices.data[word + 1u]) * 2.0 - 1.0;
			t = oct_to_vec3(vec2(signed_tangent.x, abs(signed_tangent.y) * 2.0 - 1.0));
			bn = normalize(cross(n, t) * sign(signed_tangent.y));
		} else {
			t = normalize(cross(n, abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
			bn = cross(n, t);
		}
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

	vec4 uv_scale = instances.data[instance_index].uv_scale;
#ifdef UV_USED
	uv_interp = vec2(0.0);
	if ((vg_surface.flags & VG_FLAG_UV) != 0u) {
		uv_interp = vg_attribute_vec2(ids[0], vg_surface.uv_offset) * b.x + vg_attribute_vec2(ids[1], vg_surface.uv_offset) * b.y + vg_attribute_vec2(ids[2], vg_surface.uv_offset) * b.z;
		if (uv_scale != vec4(0.0)) {
			uv_interp = (uv_interp - 0.5) * uv_scale.xy;
		}
	}
#endif
#if defined(UV2_USED) || defined(USE_LIGHTMAP)
	uv2_interp = vec2(0.0);
	if ((vg_surface.flags & VG_FLAG_UV2) != 0u) {
		uv2_interp = vg_attribute_vec2(ids[0], vg_surface.uv2_offset) * b.x + vg_attribute_vec2(ids[1], vg_surface.uv2_offset) * b.y + vg_attribute_vec2(ids[2], vg_surface.uv2_offset) * b.z;
		if (uv_scale != vec4(0.0)) {
			uv2_interp = (uv2_interp - 0.5) * uv_scale.zw;
		}
	}
#endif
#if defined(COLOR_USED)
	color_interp = vec4(1.0);
	if ((vg_surface.flags & VG_FLAG_COLOR) != 0u) {
		color_interp = vec4(0.0);
		for (int i = 0; i < 3; i++) {
			color_interp += unpackUnorm4x8(vg_attributes.data[(ids[i] * vg_surface.attrib_stride + vg_surface.color_offset) >> 2u]) * b[i];
		}
	}
#endif
}

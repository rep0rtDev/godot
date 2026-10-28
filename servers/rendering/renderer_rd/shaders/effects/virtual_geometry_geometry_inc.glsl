// Cluster geometry decode, layout in VirtualGeometryClusterGeometry (servers/rendering/virtual_geometry.h).
// Needs VG_POOL(word): uint read from the page pool, and VG_PAGE_BASE(page): word offset of that page's data
// in the pool minus the page's own offset, so a cluster's geometry offset addresses the pool directly.
#define VG_GEOMETRY_FLAG_TANGENT 1u
#define VG_GEOMETRY_FLAG_UV 2u
#define VG_GEOMETRY_FLAG_UV2 4u
#define VG_GEOMETRY_FLAG_COLOR 8u
#define VG_GEOMETRY_HEADER_BYTES 80u

struct VGGeometry {
	uint base_byte; // Pool byte offset of the header.
	uint vertex_count;
	uint triangle_count;
	uint vertex_stride;
	uint flags;
	uint index_offset;
	vec3 aabb_min;
	vec3 aabb_size;
	vec2 uv_min;
	vec2 uv_size;
	vec2 uv2_min;
	vec2 uv2_size;
};

VGGeometry vg_load_geometry(uint p_geometry_offset, uint p_page) {
	uint w = VG_PAGE_BASE(p_page) + (p_geometry_offset >> 2u);
	VGGeometry g;
	g.base_byte = w * 4u;
	g.vertex_count = VG_POOL(w);
	g.triangle_count = VG_POOL(w + 1u);
	g.vertex_stride = VG_POOL(w + 2u);
	g.flags = VG_POOL(w + 3u);
	g.aabb_min = vec3(uintBitsToFloat(VG_POOL(w + 4u)), uintBitsToFloat(VG_POOL(w + 5u)), uintBitsToFloat(VG_POOL(w + 6u)));
	g.index_offset = VG_POOL(w + 7u);
	g.aabb_size = vec3(uintBitsToFloat(VG_POOL(w + 8u)), uintBitsToFloat(VG_POOL(w + 9u)), uintBitsToFloat(VG_POOL(w + 10u)));
	g.uv_min = vec2(uintBitsToFloat(VG_POOL(w + 12u)), uintBitsToFloat(VG_POOL(w + 13u)));
	g.uv_size = vec2(uintBitsToFloat(VG_POOL(w + 14u)), uintBitsToFloat(VG_POOL(w + 15u)));
	g.uv2_min = vec2(uintBitsToFloat(VG_POOL(w + 16u)), uintBitsToFloat(VG_POOL(w + 17u)));
	g.uv2_size = vec2(uintBitsToFloat(VG_POOL(w + 18u)), uintBitsToFloat(VG_POOL(w + 19u)));
	return g;
}

uint vg_pool_u8(uint p_byte) {
	return (VG_POOL(p_byte >> 2u) >> ((p_byte & 3u) * 8u)) & 0xFFu;
}

float vg_pool_unorm16(uint p_byte) {
	return float((VG_POOL(p_byte >> 2u) >> ((p_byte & 2u) * 8u)) & 0xFFFFu) / 65535.0;
}

uint vg_geometry_index(VGGeometry g, uint p_corner) {
	return vg_pool_u8(g.base_byte + g.index_offset + p_corner);
}

uint vg_geometry_vertex_byte(VGGeometry g, uint p_vertex) {
	return g.base_byte + VG_GEOMETRY_HEADER_BYTES + p_vertex * g.vertex_stride;
}

vec3 vg_geometry_position(VGGeometry g, uint p_vertex) {
	uint b = vg_geometry_vertex_byte(g, p_vertex);
	return g.aabb_min + g.aabb_size * vec3(vg_pool_unorm16(b), vg_pool_unorm16(b + 2u), vg_pool_unorm16(b + 4u));
}

// Octahedral (unorm) rotation axis of the tangent frame; same encoding as compressed vertex attributes.
vec2 vg_geometry_axis(VGGeometry g, uint p_vertex) {
	uint b = vg_geometry_vertex_byte(g, p_vertex);
	return vec2(vg_pool_unorm16(b + 6u), vg_pool_unorm16(b + 8u));
}

// Rotation angle (unorm), its half carries the binormal sign.
float vg_geometry_angle(VGGeometry g, uint p_vertex) {
	return vg_pool_unorm16(vg_geometry_vertex_byte(g, p_vertex) + 10u);
}

vec2 vg_geometry_uv(VGGeometry g, uint p_vertex) {
	uint b = vg_geometry_vertex_byte(g, p_vertex) + 12u;
	return g.uv_min + g.uv_size * vec2(vg_pool_unorm16(b), vg_pool_unorm16(b + 2u));
}

vec2 vg_geometry_uv2(VGGeometry g, uint p_vertex) {
	uint b = vg_geometry_vertex_byte(g, p_vertex) + 12u + ((g.flags & VG_GEOMETRY_FLAG_UV) != 0u ? 4u : 0u);
	return g.uv2_min + g.uv2_size * vec2(vg_pool_unorm16(b), vg_pool_unorm16(b + 2u));
}

vec4 vg_geometry_color(VGGeometry g, uint p_vertex) {
	uint b = vg_geometry_vertex_byte(g, p_vertex) + 12u + ((g.flags & VG_GEOMETRY_FLAG_UV) != 0u ? 4u : 0u) + ((g.flags & VG_GEOMETRY_FLAG_UV2) != 0u ? 4u : 0u);
	return unpackUnorm4x8(VG_POOL(b >> 2u));
}

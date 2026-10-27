#[vertex]

#version 450

#VERSION_DEFINES

void main() {
	vec2 base = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(base * 2.0 - 1.0, 0.0, 1.0);
}

#[fragment]

#version 450

#VERSION_DEFINES

// Copies the depth of the compute-rasterized pixels into the depth buffer. The depth is pushed back by a tiny
// relative bias so the color pass, which redraws these clusters in hardware, still passes its depth test.

layout(set = 0, binding = 0, std430) restrict readonly buffer VisibilityBuffer {
	uvec2 data[];
}
visibility_buffer;

layout(push_constant, std430) uniform Params {
	uvec2 size;
	float bias;
	uint pad;
}
params;

void main() {
	uvec2 p = uvec2(gl_FragCoord.xy);
	uvec2 value = visibility_buffer.data[p.y * params.size.x + p.x];
	if (value.y == 0u) {
		discard;
	}
	gl_FragDepth = uintBitsToFloat(value.y) * (1.0 - params.bias);
}

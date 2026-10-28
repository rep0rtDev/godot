#[compute]

#version 450

#VERSION_DEFINES

// Sorts the screen into 8x8 tiles per virtual geometry surface, so the visibility material pass of each surface
// only draws the tiles that hold its pixels. Three passes:
//   count:  every tile adds one instance to the draw command of each surface slot found in it;
//   prefix: turns the counts into tile list ranges and finishes the non-indexed indirect draw commands;
//   fill:   every tile appends its packed coordinate to the range of each slot found in it.
// Commands: per slot [vertex_count, instance_count, first_vertex, first_instance]. Tiles: slot_count fill cursors,
// then the tile lists. A nonzero first_instance needs an optional device feature, so the list start goes in
// first_vertex instead, as 3 * (absolute index of the first tile in the tile buffer).

#define NO_SLOT 0xFFFFFFFFu

#ifdef MODE_PREFIX
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
#else
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#endif

layout(set = 0, binding = 0, std430) restrict readonly buffer VisibilityBuffer {
	uvec2 data[];
}
visibility_buffer;

layout(set = 0, binding = 1, std430) restrict readonly buffer Entries {
	uint count;
	uint pad0;
	uvec2 data[];
}
entries;

layout(set = 0, binding = 2, std430) restrict buffer Tiles {
	uint data[];
}
tiles;

layout(set = 0, binding = 3, std430) restrict buffer Commands {
	uint data[];
}
commands;

layout(push_constant, std430) uniform Params {
	uvec2 size;
	uint slot_count;
	uint pad;
}
params;

shared uint shared_values[64];

void main() {
#ifdef MODE_PREFIX
	uint i = gl_LocalInvocationIndex;
	uint per_thread = (params.slot_count + 63u) / 64u;
	uint begin = min(i * per_thread, params.slot_count);
	uint end = min(begin + per_thread, params.slot_count);
	uint sum = 0u;
	for (uint s = begin; s < end; s++) {
		sum += commands.data[s * 4u + 1u];
	}
	shared_values[i] = sum;
	barrier();
	if (i == 0u) {
		uint running = 0u;
		for (uint j = 0u; j < 64u; j++) {
			uint value = shared_values[j];
			shared_values[j] = running;
			running += value;
		}
	}
	barrier();
	uint first = params.slot_count + shared_values[i];
	for (uint s = begin; s < end; s++) {
		commands.data[s * 4u] = 3u;
		commands.data[s * 4u + 2u] = first * 3u;
		commands.data[s * 4u + 3u] = 0u;
		first += commands.data[s * 4u + 1u];
	}
#else
	uvec2 p = gl_GlobalInvocationID.xy;
	uint slot = NO_SLOT;
	if (all(lessThan(p, params.size))) {
		uvec2 value = visibility_buffer.data[p.y * params.size.x + p.x];
		if (value.y != 0u && (value.x >> 7u) < entries.count) {
			slot = entries.data[value.x >> 7u].x;
		}
	}
	uint i = gl_LocalInvocationIndex;
	shared_values[i] = slot;
	barrier();
	if (slot >= params.slot_count) {
		return;
	}
	// Only the first pixel of each slot in this tile records it.
	for (uint j = 0u; j < i; j++) {
		if (shared_values[j] == slot) {
			return;
		}
	}
#ifdef MODE_COUNT
	atomicAdd(commands.data[slot * 4u + 1u], 1u);
#else
	uint at = commands.data[slot * 4u + 2u] / 3u + atomicAdd(tiles.data[slot], 1u);
	tiles.data[at] = gl_WorkGroupID.x | (gl_WorkGroupID.y << 16u);
#endif
#endif
}

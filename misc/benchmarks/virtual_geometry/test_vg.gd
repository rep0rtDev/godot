extends SceneTree
# Correctness test for virtual geometry: run with `godot --headless -s test_vg.gd`.

const FLT_BIG := 1.0e37

func make_sphere(rings: int, segs: int) -> Array:
	var st := SurfaceTool.new()
	var arrays := SphereMesh.new()
	arrays.rings = rings
	arrays.radial_segments = segs
	var a := arrays.get_mesh_arrays()
	return a

func cluster_at(buf: PackedByteArray, i: int) -> Dictionary:
	var o := i * 64
	return {
		"cull": Vector4(buf.decode_float(o), buf.decode_float(o + 4), buf.decode_float(o + 8), buf.decode_float(o + 12)),
		"self": Vector4(buf.decode_float(o + 16), buf.decode_float(o + 20), buf.decode_float(o + 24), buf.decode_float(o + 28)),
		"parent": Vector4(buf.decode_float(o + 32), buf.decode_float(o + 36), buf.decode_float(o + 40), buf.decode_float(o + 44)),
		"self_error": buf.decode_float(o + 48),
		"parent_error": buf.decode_float(o + 52),
		"offset": buf.decode_u32(o + 56),
		"count": buf.decode_u32(o + 60),
	}

func fine_enough(sphere: Vector4, err: float, cam: Vector3, lod_scale: float) -> bool:
	if err > FLT_BIG:
		return false
	var d: float = max((Vector3(sphere.x, sphere.y, sphere.z) - cam).length() - sphere.w, 1e-5)
	return err * lod_scale <= d

func _init() -> void:
	var failures := 0
	var arrays := make_sphere(256, 512)
	var src_tris: int = arrays[Mesh.ARRAY_INDEX].size() / 3
	var mesh := ArrayMesh.new()
	var t0 := Time.get_ticks_usec()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {}, Mesh.ARRAY_FLAG_USE_VIRTUAL_GEOMETRY)
	var build_ms := (Time.get_ticks_usec() - t0) / 1000.0
	var sd: Dictionary = RenderingServer.mesh_get_surface(mesh.get_rid(), 0)
	if not sd.has("vg_clusters"):
		print("FAIL: no vg_clusters in surface")
		quit(1)
		return
	var buf: PackedByteArray = sd["vg_clusters"]
	var leaf_count: int = sd["vg_leaf_index_count"]
	var n := buf.size() / 64
	var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var idx := PackedInt32Array()
	var ib: PackedByteArray = sd["index_data"]
	var wide: bool = sd["vertex_count"] > 65536
	var total_idx: int = sd["index_count"]
	idx.resize(total_idx)
	for i in total_idx:
		idx[i] = ib.decode_u32(i * 4) if wide else ib.decode_u16(i * 2)
	print("source tris=%d clusters=%d leaf_tris=%d total_tris=%d build_ms=%.1f" % [src_tris, n, leaf_count / 3, total_idx / 3, build_ms])
	if leaf_count / 3 != src_tris:
		print("FAIL: leaf triangle count mismatch"); failures += 1
	var surf_arrays := mesh.surface_get_arrays(0)
	if surf_arrays[Mesh.ARRAY_INDEX].size() != leaf_count:
		print("FAIL: surface_get_arrays should return leaf mesh"); failures += 1

	var clusters := []
	var roots := 0
	for i in n:
		var c := cluster_at(buf, i)
		clusters.append(c)
		if c["parent_error"] > FLT_BIG:
			roots += 1
		elif c["parent_error"] < c["self_error"]:
			print("FAIL: non-monotonic error at %d" % i); failures += 1
		if c["count"] > 128 * 3 or c["count"] % 3 != 0:
			print("FAIL: bad cluster size %d" % c["count"]); failures += 1
	var root_tris := 0
	for c in clusters:
		if c["parent_error"] > FLT_BIG:
			root_tris += c["count"] / 3
	print("roots=%d root_tris=%d" % [roots, root_tris])

	# Simulate GPU selection for several camera distances and check the cut is watertight
	# (every position-edge used exactly twice on a closed mesh) and triangle count shrinks with distance.
	var prev_tris := 1 << 30
	for dist in [1.5, 3.0, 10.0, 50.0, 500.0, 5000.0]:
		var cam := Vector3(0, 0, dist)
		var lod_scale := 1080.0 * 0.5 * (1.0 / tan(deg_to_rad(37.5))) / 1.0
		var edges := {}
		var tris := 0
		for c in clusters:
			if fine_enough(c["self"], c["self_error"], cam, lod_scale) and not fine_enough(c["parent"], c["parent_error"], cam, lod_scale):
				tris += c["count"] / 3
				for t in range(c["offset"], c["offset"] + c["count"], 3):
					for e in 3:
						var a: Vector3 = verts[idx[t + e]]
						var b: Vector3 = verts[idx[t + (e + 1) % 3]]
						if a == b:
							continue
						var key = [a, b] if str(a) < str(b) else [b, a]
						var k := str(key)
						edges[k] = edges.get(k, 0) + 1
		var bad := 0
		for k in edges:
			if edges[k] != 2:
				bad += 1
		print("dist=%7.1f tris=%7d open_edges=%d" % [dist, tris, bad])
		if tris == 0 or tris > prev_tris:
			print("FAIL: triangle count not monotonic"); failures += 1
		prev_tris = tris
		if bad > 0 and dist < 100.0:
			print("FAIL: cracks in selected cut"); failures += 1

	print("RESULT: %s" % ("PASS" if failures == 0 else "FAIL (%d)" % failures))
	quit(0 if failures == 0 else 1)

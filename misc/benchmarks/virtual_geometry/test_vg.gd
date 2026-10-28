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

const HEADER := 32

func cluster_at(buf: PackedByteArray, i: int) -> Dictionary:
	var o := HEADER + i * 64
	return {
		"cull": Vector4(buf.decode_float(o), buf.decode_float(o + 4), buf.decode_float(o + 8), buf.decode_float(o + 12)),
		"self": Vector4(buf.decode_float(o + 16), buf.decode_float(o + 20), buf.decode_float(o + 24), buf.decode_float(o + 28)),
		"parent": Vector4(buf.decode_float(o + 32), buf.decode_float(o + 36), buf.decode_float(o + 40), buf.decode_float(o + 44)),
		"self_error": buf.decode_float(o + 48),
		"parent_error": buf.decode_float(o + 52),
		"geometry": buf.decode_u32(o + 56),
		"page": buf.decode_u32(o + 60) >> 8,
		"count": (buf.decode_u32(o + 60) & 0xFF) * 3,
	}

# Decodes one cluster of the page data: positions (unorm16 in the cluster AABB) and 8-bit indices.
func decode_cluster(page_data: PackedByteArray, geometry_offset: int) -> Dictionary:
	var o := geometry_offset
	var vertex_count := page_data.decode_u32(o)
	var triangle_count := page_data.decode_u32(o + 4)
	var stride := page_data.decode_u32(o + 8)
	var aabb_min := Vector3(page_data.decode_float(o + 16), page_data.decode_float(o + 20), page_data.decode_float(o + 24))
	var index_offset := page_data.decode_u32(o + 28)
	var aabb_size := Vector3(page_data.decode_float(o + 32), page_data.decode_float(o + 36), page_data.decode_float(o + 40))
	var positions := PackedVector3Array()
	positions.resize(vertex_count)
	for v in vertex_count:
		var r := o + 80 + v * stride
		positions[v] = aabb_min + aabb_size * Vector3(page_data.decode_u16(r), page_data.decode_u16(r + 2), page_data.decode_u16(r + 4)) / 65535.0
	var indices := PackedInt32Array()
	indices.resize(triangle_count * 3)
	for k in triangle_count * 3:
		indices[k] = page_data.decode_u8(o + index_offset + k)
	return {"positions": positions, "indices": indices, "stride": stride, "flags": page_data.decode_u32(o + 12)}

func node_at(buf: PackedByteArray, cluster_count: int, i: int) -> Dictionary:
	var o := HEADER + cluster_count * 64 + i * 64
	return {
		"cull": Vector4(buf.decode_float(o), buf.decode_float(o + 4), buf.decode_float(o + 8), buf.decode_float(o + 12)),
		"lod": Vector4(buf.decode_float(o + 16), buf.decode_float(o + 20), buf.decode_float(o + 24), buf.decode_float(o + 28)),
		"max_parent_error": buf.decode_float(o + 32),
		"child_first": buf.decode_u32(o + 36),
		"child_count": buf.decode_u32(o + 40),
		"cluster_first": buf.decode_u32(o + 44),
		"cluster_count": buf.decode_u32(o + 48),
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
	if buf.decode_u32(0) != 0x33444756:
		print("FAIL: bad vg data magic")
		quit(1)
		return
	var n := buf.decode_u32(4)
	var node_count := buf.decode_u32(8)
	var page_count := buf.decode_u32(12)
	var dependency_count := buf.decode_u32(16)
	var data_size := buf.decode_u32(20)
	var page_table := HEADER + (n + node_count) * 64
	var page_data_offset := page_table + page_count * 16 + dependency_count * 4
	if buf.size() != page_data_offset + data_size:
		print("FAIL: vg data size mismatch")
		quit(1)
		return
	var page_data := buf.slice(page_data_offset)
	var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var total_idx := 0
	for i in n:
		total_idx += (buf.decode_u32(HEADER + i * 64 + 60) & 0xFF) * 3
	print("source tris=%d clusters=%d leaf_tris=%d total_tris=%d pages=%d data_kb=%d build_ms=%.1f" % [src_tris, n, leaf_count / 3, total_idx / 3, page_count, data_size / 1024, build_ms])
	# Pages: root page within 32 KB, others within 128 KB, contiguous, dependencies point at earlier pages.
	var expect_offset := 0
	for pg in page_count:
		var po := page_table + pg * 16
		var p_offset := buf.decode_u32(po)
		var p_size := buf.decode_u32(po + 4)
		var dep_first := buf.decode_u32(po + 8)
		var dep_count := buf.decode_u32(po + 12)
		if p_offset != expect_offset or p_size == 0 or p_size > (32 * 1024 if pg == 0 else 128 * 1024):
			print("FAIL: bad page %d offset %d size %d" % [pg, p_offset, p_size]); failures += 1
		expect_offset += p_size
		for d in dep_count:
			var dep := buf.decode_u32(page_table + page_count * 16 + (dep_first + d) * 4)
			if dep >= pg:
				print("FAIL: page %d depends on later page %d" % [pg, dep]); failures += 1
	if expect_offset != data_size:
		print("FAIL: pages do not cover the page data"); failures += 1
	# Decoded cluster geometry: every quantized vertex snaps to a source vertex, so cuts can be checked by position.
	var grid := {}
	for v in verts.size():
		var cell := str((verts[v] * 1000.0).round())
		if not grid.has(cell):
			grid[cell] = []
		grid[cell].append(v)
	var decoded := []
	var max_err := 0.0
	for i in n:
		var c := decode_cluster(page_data, buf.decode_u32(HEADER + i * 64 + 56))
		var snapped := PackedVector3Array()
		snapped.resize(c["positions"].size())
		for v in c["positions"].size():
			var p: Vector3 = c["positions"][v]
			var best := -1
			var best_d := 1e30
			var center := (p * 1000.0).round()
			for dz in [-1, 0, 1]:
				for dy in [-1, 0, 1]:
					for dx in [-1, 0, 1]:
						for cand in grid.get(str(center + Vector3(dx, dy, dz)), []):
							var d := p.distance_squared_to(verts[cand])
							if d < best_d:
								best_d = d
								best = cand
			if best < 0:
				print("FAIL: cluster %d vertex %d far from every source vertex" % [i, v]); failures += 1
				snapped[v] = p
			else:
				max_err = max(max_err, sqrt(best_d))
				snapped[v] = verts[best]
		c["positions"] = snapped
		decoded.append(c)
	print("max quantization error=%f" % max_err)
	if max_err > 1e-4:
		print("FAIL: quantization error too large"); failures += 1
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
	print("roots=%d root_tris=%d nodes=%d" % [roots, root_tris, node_count])

	# Hierarchy: leaves partition the cluster array, inner nodes cover exactly their children's ranges,
	# and bounds/errors are conservative for everything below.
	var nodes := []
	for j in node_count:
		nodes.append(node_at(buf, n, j))
	var covered := PackedInt32Array()
	covered.resize(n)
	for j in node_count:
		var nd: Dictionary = nodes[j]
		var first: int = nd["cluster_first"]
		var cnt: int = nd["cluster_count"]
		if cnt == 0 or first + cnt > n:
			print("FAIL: bad node range %d" % j); failures += 1
			continue
		if nd["child_count"] == 0:
			for k in range(first, first + cnt):
				covered[k] += 1
				var c: Dictionary = clusters[k]
				var ps: Vector4 = c["parent"]
				var ls: Vector4 = nd["lod"]
				if Vector3(ps.x, ps.y, ps.z).distance_to(Vector3(ls.x, ls.y, ls.z)) + ps.w > ls.w * 1.0001 + 1e-5:
					print("FAIL: node %d lod sphere does not contain cluster %d" % [j, k]); failures += 1
				if c["parent_error"] > nd["max_parent_error"]:
					print("FAIL: node %d error below cluster %d" % [j, k]); failures += 1
		else:
			var expect: int = first
			for ch in range(nd["child_first"], nd["child_first"] + nd["child_count"]):
				var cd: Dictionary = nodes[ch]
				if cd["cluster_first"] != expect:
					print("FAIL: node %d child ranges not contiguous" % j); failures += 1
				expect = cd["cluster_first"] + cd["cluster_count"]
			if expect != first + cnt:
				print("FAIL: node %d range mismatch" % j); failures += 1
	for k in n:
		if covered[k] != 1:
			print("FAIL: cluster %d covered %d times" % [k, covered[k]]); failures += 1
			break
	if nodes[0]["cluster_first"] != 0 or nodes[0]["cluster_count"] != n:
		print("FAIL: root does not cover all clusters"); failures += 1

	# Simulate GPU selection for several camera distances and check the cut is watertight
	# (every position-edge used exactly twice on a closed mesh) and triangle count shrinks with distance.
	var prev_tris := 1 << 30
	for dist in [1.5, 3.0, 10.0, 50.0, 500.0, 5000.0]:
		var cam := Vector3(0, 0, dist)
		var lod_scale := 1080.0 * 0.5 * (1.0 / tan(deg_to_rad(37.5))) / 1.0
		var edges := {}
		var tris := 0
		# Traverse like the GPU: only clusters under expanded nodes are tested.
		var queue := [0]
		var candidates := []
		var visited := 0
		while not queue.is_empty():
			var nd: Dictionary = nodes[queue.pop_back()]
			visited += 1
			if fine_enough(nd["lod"], nd["max_parent_error"], cam, lod_scale):
				continue
			if nd["child_count"] == 0:
				for ci in range(nd["cluster_first"], nd["cluster_first"] + nd["cluster_count"]):
					candidates.append(ci)
			else:
				for ch in range(nd["child_first"], nd["child_first"] + nd["child_count"]):
					queue.append(ch)
		var flat_selected := 0
		for c in clusters:
			if fine_enough(c["self"], c["self_error"], cam, lod_scale) and not fine_enough(c["parent"], c["parent_error"], cam, lod_scale):
				flat_selected += 1
		var hier_selected := 0
		for ci in candidates:
			var c: Dictionary = clusters[ci]
			if fine_enough(c["self"], c["self_error"], cam, lod_scale) and not fine_enough(c["parent"], c["parent_error"], cam, lod_scale):
				hier_selected += 1
				tris += c["count"] / 3
				var geom: Dictionary = decoded[ci]
				var gp: PackedVector3Array = geom["positions"]
				var gi: PackedInt32Array = geom["indices"]
				for t in range(0, gi.size(), 3):
					for e in 3:
						var a: Vector3 = gp[gi[t + e]]
						var b: Vector3 = gp[gi[t + (e + 1) % 3]]
						if a == b:
							continue
						var key = [a, b] if str(a) < str(b) else [b, a]
						var k := str(key)
						edges[k] = edges.get(k, 0) + 1
		var bad := 0
		for k in edges:
			if edges[k] != 2:
				bad += 1
		print("dist=%7.1f tris=%7d open_edges=%d nodes_visited=%d candidates=%d selected=%d" % [dist, tris, bad, visited, candidates.size(), hier_selected])
		if hier_selected != flat_selected:
			print("FAIL: hierarchy selected %d clusters, flat selection %d" % [hier_selected, flat_selected]); failures += 1
		if tris == 0 or tris > prev_tris:
			print("FAIL: triangle count not monotonic"); failures += 1
		prev_tris = tris
		if bad > 0 and dist < 100.0:
			print("FAIL: cracks in selected cut"); failures += 1

	print("RESULT: %s" % ("PASS" if failures == 0 else "FAIL (%d)" % failures))
	quit(0 if failures == 0 else 1)

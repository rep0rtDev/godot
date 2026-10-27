extends SceneTree
# Benchmark: N instances of a dense mesh, regular vs virtual geometry.
# Usage: godot --rendering-driver vulkan -s bench_vg.gd -- --vg=1 --count=400 --frames=300 [--wall=1 [--wall_off=<frame>]] [--mat=tex]
# --wall_off hides the occluder at the given frame, so the final frame checks that newly revealed geometry appears.
# Prints average CPU frame time and GPU render time reported by the RenderingServer.

var frames := 300
var warmup := 30
var frame := 0
var cam: Camera3D
var wall: MeshInstance3D
var wall_off := -1
var times := PackedFloat64Array()
var gpu_times := PackedFloat64Array()
var last := 0

func arg(name: String, def: String) -> String:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--" + name + "="):
			return a.split("=")[1]
	return def

func textured_material() -> StandardMaterial3D:
	var albedo := Image.create(256, 256, false, Image.FORMAT_RGBA8)
	var normal := Image.create(256, 256, false, Image.FORMAT_RGBA8)
	for y in 256:
		for x in 256:
			var checker := ((x / 16) + (y / 16)) % 2 == 0
			albedo.set_pixel(x, y, Color(0.9, 0.8, 0.2) if checker else Color(0.1, 0.3, 0.8))
			var dx := cos(x * TAU / 32.0) * 0.6
			var dy := cos(y * TAU / 32.0) * 0.6
			var n := Vector3(-dx, -dy, 1.0).normalized()
			normal.set_pixel(x, y, Color(n.x * 0.5 + 0.5, n.y * 0.5 + 0.5, n.z * 0.5 + 0.5))
	albedo.generate_mipmaps()
	normal.generate_mipmaps()
	var mat := StandardMaterial3D.new()
	mat.albedo_texture = ImageTexture.create_from_image(albedo)
	mat.normal_enabled = true
	mat.normal_texture = ImageTexture.create_from_image(normal)
	mat.roughness = 0.4
	return mat

func _init() -> void:
	var use_vg := arg("vg", "1") == "1"
	var count := int(arg("count", "400"))
	frames = int(arg("frames", "300"))
	warmup = int(arg("warmup", "30"))
	var sm := SphereMesh.new()
	sm.rings = int(arg("rings", "256"))
	sm.radial_segments = sm.rings * 2
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, sm.get_mesh_arrays(), [], {}, Mesh.ARRAY_FLAG_USE_VIRTUAL_GEOMETRY if use_vg else 0)
	if arg("mat", "") == "tex":
		# Checker albedo plus a bump normal map, to compare UV, normal and tangent reconstruction.
		mesh.surface_set_material(0, textured_material())
	var root := Node3D.new()
	var side := int(ceil(sqrt(count)))
	for i in count:
		var mi := MeshInstance3D.new()
		mi.mesh = mesh
		mi.position = Vector3((i % side) * 3.0, 0, -(i / side) * 3.0)
		root.add_child(mi)
	if arg("wall", "0") == "1":
		# Opaque occluder in front of the grid, leaving only its right edge visible, for occlusion culling checks.
		wall = MeshInstance3D.new()
		var box := BoxMesh.new()
		box.size = Vector3(side * 2.25, 12, 0.3)
		wall.mesh = box
		wall.position = Vector3((side - 1) * 1.5 - side * 0.375, 2, 3)
		root.add_child(wall)
	wall_off = int(arg("wall_off", "-1"))
	if arg("backdrop", "0") == "1":
		# Transparent plane behind the grid. The transparent pass depth-tests against the finished opaque depth,
		# so it covers any pixel whose depth was lost, regardless of draw order.
		var backdrop := MeshInstance3D.new()
		var quad := QuadMesh.new()
		quad.size = Vector2(200, 200)
		var mat := StandardMaterial3D.new()
		mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		mat.albedo_color = Color(1, 0, 0, 0.5)
		quad.material = mat
		backdrop.mesh = quad
		backdrop.position = Vector3(0, 0, -(side + 2) * 3.0)
		root.add_child(backdrop)
	var light := DirectionalLight3D.new()
	light.shadow_enabled = arg("shadows", "1") == "1"
	light.rotation_degrees = Vector3(-50, 30, 0)
	root.add_child(light)
	cam = Camera3D.new()
	cam.position = Vector3(side * 1.5, 6, 8)
	cam.rotation_degrees = Vector3(-15, 0, 0)
	if arg("view", "grid") == "row":
		# Low camera looking down the columns, so spheres hide each other (depth resolve and occlusion checks).
		cam.position = Vector3(1.5, 0.4, 6)
		cam.rotation_degrees = Vector3(-2, 0, 0)
	root.add_child(cam)
	get_root().add_child(root)
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	RenderingServer.viewport_set_measure_render_time(get_root().get_viewport_rid(), true)
	print("bench vg=%s count=%d tris_per_mesh=%d" % [use_vg, count, sm.get_mesh_arrays()[Mesh.ARRAY_INDEX].size() / 3])

func _process(_delta: float) -> bool:
	frame += 1
	if wall and frame == wall_off:
		wall.visible = false
	var now := Time.get_ticks_usec()
	if frame > warmup and last != 0:
		times.append((now - last) / 1000.0)
		gpu_times.append(RenderingServer.viewport_get_measured_render_time_gpu(get_root().get_viewport_rid()))
	last = now
	if frame >= warmup + frames:
		times.sort()
		gpu_times.sort()
		var avg := 0.0
		for t in times:
			avg += t
		avg /= times.size()
		var gavg := 0.0
		for t in gpu_times:
			gavg += t
		gavg /= gpu_times.size()
		var shot := arg("shot", "")
		if shot != "":
			get_root().get_texture().get_image().save_png(shot)
		print("RESULT frame_ms avg=%.3f p50=%.3f p95=%.3f gpu_ms avg=%.3f p50=%.3f device=%s" % [avg, times[times.size() / 2], times[int(times.size() * 0.95)], gavg, gpu_times[gpu_times.size() / 2], RenderingServer.get_video_adapter_name()])
		print("RESULT vg_sw_clusters=%d vg_sw_tris=%d" % [
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_GEOMETRY_SOFTWARE_CLUSTERS_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_GEOMETRY_SOFTWARE_PRIMITIVES_IN_FRAME)])
		print("RESULT vg_clusters=%d vg_tris=%d vg_shadow_tris=%d total_prims=%d draw_calls=%d" % [
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_GEOMETRY_CLUSTERS_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_GEOMETRY_PRIMITIVES_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_GEOMETRY_SHADOW_PRIMITIVES_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME)])
		return true
	return false

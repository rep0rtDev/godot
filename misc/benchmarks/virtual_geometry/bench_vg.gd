extends SceneTree
# Benchmark: N instances of a dense mesh, regular vs virtual geometry.
# Usage: godot --rendering-driver vulkan -s bench_vg.gd -- --vg=1 --count=400 --frames=300 [--wall=1 [--wall_off=<frame>]] [--mat=tex] [--compress=1] [--open=1 --cull=back|front|disabled]
# --wall_off hides the occluder at the given frame, so the final frame checks that newly revealed geometry appears.
# Prints average CPU frame time and GPU render time reported by the RenderingServer.

var frames := 300
var warmup := 30
var frame := 0
var cam: Camera3D
var wall: MeshInstance3D
var wall_off := -1
var move_frame := -1
var cam_move := 0.0
var cam_x := 0.0
var first_mi: MeshInstance3D
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
	var flags := Mesh.ARRAY_FLAG_USE_VIRTUAL_GEOMETRY if use_vg else 0
	if arg("compress", "0") == "1":
		flags |= Mesh.ARRAY_FLAG_COMPRESS_ATTRIBUTES
	var arrays := sm.get_mesh_arrays()
	if arg("open", "0") == "1":
		# Cut away the triangles facing the camera, so the inside of the sphere (its back faces) is in view.
		var pos: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var idx: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		var kept := PackedInt32Array()
		for t in idx.size() / 3:
			var c := pos[idx[t * 3]] + pos[idx[t * 3 + 1]] + pos[idx[t * 3 + 2]]
			if c.z < 0.9 or c.y < -0.6:
				kept.append(idx[t * 3])
				kept.append(idx[t * 3 + 1])
				kept.append(idx[t * 3 + 2])
		arrays[Mesh.ARRAY_INDEX] = kept
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {}, flags)
	# Several distinct meshes (different ring counts), so that page streaming has to share its pool between surfaces.
	var meshes: Array[ArrayMesh] = [mesh]
	for m in range(1, int(arg("meshes", "1"))):
		var sm2 := SphereMesh.new()
		sm2.rings = sm.rings - 4 * m
		sm2.radial_segments = sm2.rings * 2
		var mesh2 := ArrayMesh.new()
		mesh2.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, sm2.get_mesh_arrays(), [], {}, flags)
		meshes.append(mesh2)
	if arg("mat", "") == "tex":
		# Checker albedo plus a bump normal map, to compare UV, normal and tangent reconstruction.
		var tex_mat := textured_material()
		for m in meshes:
			m.surface_set_material(0, tex_mat)
	match arg("cull", ""):
		"back", "front", "disabled":
			var mat: StandardMaterial3D = mesh.surface_get_material(0)
			if mat == null:
				mat = StandardMaterial3D.new()
				mat.albedo_color = Color(0.7, 0.75, 0.8)
				mesh.surface_set_material(0, mat)
			mat.cull_mode = {"back": BaseMaterial3D.CULL_BACK, "front": BaseMaterial3D.CULL_FRONT, "disabled": BaseMaterial3D.CULL_DISABLED}[arg("cull", "")]
	if arg("pages_file", "0") == "1":
		# Pages move to a file; the renderer reads them back on request.
		for m in meshes.size():
			meshes[m].save_virtual_geometry_pages("user://bench_%d.vgpages" % m)
	var root := Node3D.new()
	if arg("floor", "0") == "1":
		# A shadow receiver under the spheres, for shadow comparisons.
		var floor_mesh := PlaneMesh.new()
		floor_mesh.size = Vector2(400, 400)
		var floor_mi := MeshInstance3D.new()
		floor_mi.mesh = floor_mesh
		floor_mi.position = Vector3(0, -3, 0)
		if arg("rough", "") != "":
			var fm := StandardMaterial3D.new()
			fm.albedo_color = Color(0.8, 0.8, 0.8)
			fm.roughness = float(arg("rough", "1"))
			fm.metallic = 0.0
			floor_mi.material_override = fm
		root.add_child(floor_mi)
	if arg("spot", "0") == "1":
		var spot := SpotLight3D.new()
		spot.position = Vector3(5, 10, -5)
		spot.rotation_degrees = Vector3(-90, 0, 0)
		spot.spot_range = 40
		spot.spot_angle = 50
		spot.light_energy = 8
		spot.shadow_enabled = true
		spot.light_size = float(arg("spot_size", "0"))
		spot.shadow_blur = 1.0
		root.add_child(spot)
	var side := int(ceil(sqrt(count)))
	for i in count:
		var mi := MeshInstance3D.new()
		mi.mesh = meshes[i % meshes.size()]
		if i == 0:
			first_mi = mi
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
	if arg("plate", "0") == "1":
		# Thin bright plate standing on the floor: the SDF resolves it to a whole cell, the screen trace to its true position.
		var plate := MeshInstance3D.new()
		var pbox := BoxMesh.new()
		pbox.size = Vector3(6, 2, 0.02)
		plate.mesh = pbox
		plate.position = Vector3(side * 1.5, -2, -(side - 1) * 3.0 - 2.0)
		var pm := StandardMaterial3D.new()
		pm.albedo_color = Color(1, 0.1, 0.1)
		plate.material_override = pm
		root.add_child(plate)
	wall_off = int(arg("wall_off", "-1"))
	move_frame = int(arg("move", "-1"))
	cam_move = float(arg("cam_move", "0"))
	cam_x = float(arg("cam_x", "0"))
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
	light.light_angular_distance = float(arg("soft", "0"))
	if arg("sdfgi", "0") == "1":
		var we := WorldEnvironment.new()
		var env := Environment.new()
		env.background_mode = Environment.BG_COLOR
		env.background_color = Color(0.2, 0.2, 0.25)
		env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		var amb := float(arg("ambient", "-1"))
		env.ambient_light_color = Color(0.2, 0.2, 0.25) if amb < 0.0 else Color(amb, amb, amb)
		if arg("sky", "0") == "1":
			env.background_mode = Environment.BG_SKY
			var sky := Sky.new()
			var sky_mat := ProceduralSkyMaterial.new()
			sky_mat.sky_top_color = Color(0.1, 0.3, 1.0)
			sky_mat.sky_horizon_color = Color(0.1, 0.3, 1.0)
			sky_mat.ground_bottom_color = Color(0.1, 0.3, 1.0)
			sky_mat.ground_horizon_color = Color(0.1, 0.3, 1.0)
			sky.sky_material = sky_mat
			env.sky = sky
		env.sdfgi_enabled = true
		env.sdfgi_cascades = 4
		env.sdfgi_min_cell_size = 0.2
		we.environment = env
		root.add_child(we)
	light.shadow_enabled = arg("shadows", "1") == "1"
	light.rotation_degrees = Vector3(-50, 30, 0)
	root.add_child(light)
	cam = Camera3D.new()
	cam.position = Vector3(side * 1.5 + cam_x, 6, 8)
	cam.rotation_degrees = Vector3(-15, 0, 0)
	if arg("view", "grid") == "row":
		# Low camera looking down the columns, so spheres hide each other (depth resolve and occlusion checks).
		cam.position = Vector3(1.5, 0.4, 6)
		cam.rotation_degrees = Vector3(-2, 0, 0)
	root.add_child(cam)
	get_root().add_child(root)
	get_root().debug_draw = int(arg("debug_draw", "0")) as Viewport.DebugDraw
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	RenderingServer.viewport_set_measure_render_time(get_root().get_viewport_rid(), true)
	print("bench vg=%s count=%d tris_per_mesh=%d" % [use_vg, count, sm.get_mesh_arrays()[Mesh.ARRAY_INDEX].size() / 3])

func _process(_delta: float) -> bool:
	if cam_move != 0.0 and cam and frame > 0:
		cam.position.x += cam_move
	frame += 1
	if first_mi and frame == move_frame:
		# Moves one caster so that its cached shadow has to be redrawn.
		first_mi.position += Vector3(6, 0, 0)
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

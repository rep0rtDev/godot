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
var anim := false
var cut_frame := -1
var anim_base := Vector3()
var shadow_tris_max := 0
var cam_move := 0.0
var skin_skel: Skeleton3D
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
	if arg("omni", "0") == "1":
		var omni := OmniLight3D.new()
		omni.position = Vector3(5, 5, -5)
		omni.omni_range = 40
		omni.light_energy = 8
		omni.shadow_enabled = true
		omni.light_size = float(arg("omni_size", "0"))
		omni.shadow_blur = 1.0
		if arg("omni_cube", "0") == "1":
			omni.omni_shadow_mode = OmniLight3D.SHADOW_CUBE
		root.add_child(omni)
	if arg("particles", "0") == "1":
		# Slow emissive-free particle spheres above the grid: surface-cache cards for particles.
		var gp := GPUParticles3D.new()
		var pm := ParticleProcessMaterial.new()
		pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_BOX
		pm.emission_box_extents = Vector3(4, 0.5, 4)
		pm.gravity = Vector3.ZERO
		pm.initial_velocity_min = 0.0
		pm.initial_velocity_max = 0.0
		gp.process_material = pm
		var pmesh := SphereMesh.new()
		pmesh.radius = 0.6
		pmesh.height = 1.2
		var pmat := StandardMaterial3D.new()
		pmat.albedo_color = Color(1.0, 0.2, 0.1)
		pmesh.material = pmat
		gp.draw_pass_1 = pmesh
		gp.amount = 24
		gp.lifetime = 100.0
		gp.preprocess = 5.0
		gp.position = Vector3(4.5, 2.5, -4.5)
		gp.visibility_aabb = AABB(Vector3(-6, -3, -6), Vector3(12, 6, 12))
		root.add_child(gp)
	if arg("skin", "0") == "1":
		# A skinned 1x4x1 box with two bones; the upper bone bends every frame (dynamic surface-cache cards).
		skin_skel = Skeleton3D.new()
		skin_skel.add_bone("root")
		skin_skel.set_bone_rest(0, Transform3D())
		skin_skel.add_bone("upper")
		skin_skel.set_bone_parent(1, 0)
		skin_skel.set_bone_rest(1, Transform3D(Basis(), Vector3(0, 2, 0)))
		skin_skel.reset_bone_poses()
		var skin_verts := PackedVector3Array()
		var skin_norms := PackedVector3Array()
		var skin_bones := PackedInt32Array()
		var skin_weights := PackedFloat32Array()
		var skin_idx := PackedInt32Array()
		var skin_segs := 8
		for i in range(skin_segs + 1):
			var y := 4.0 * float(i) / float(skin_segs)
			var w1 := clampf((y - 1.0) / 2.0, 0.0, 1.0)
			for c in [Vector3(-0.5, 0, -0.5), Vector3(0.5, 0, -0.5), Vector3(0.5, 0, 0.5), Vector3(-0.5, 0, 0.5)]:
				skin_verts.push_back(Vector3(c.x, y, c.z))
				skin_norms.push_back(Vector3(c.x, 0, c.z).normalized())
				skin_bones.append_array(PackedInt32Array([0, 1, 0, 0]))
				skin_weights.append_array(PackedFloat32Array([1.0 - w1, w1, 0.0, 0.0]))
		for i in range(skin_segs):
			for k in range(4):
				var a := i * 4 + k
				var b := i * 4 + (k + 1) % 4
				skin_idx.append_array(PackedInt32Array([a, b, a + 4, b, b + 4, a + 4]))
		var skin_arrays := []
		skin_arrays.resize(Mesh.ARRAY_MAX)
		skin_arrays[Mesh.ARRAY_VERTEX] = skin_verts
		skin_arrays[Mesh.ARRAY_NORMAL] = skin_norms
		skin_arrays[Mesh.ARRAY_BONES] = skin_bones
		skin_arrays[Mesh.ARRAY_WEIGHTS] = skin_weights
		skin_arrays[Mesh.ARRAY_INDEX] = skin_idx
		var skin_amesh := ArrayMesh.new()
		skin_amesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, skin_arrays)
		var skin_smat := StandardMaterial3D.new()
		skin_smat.albedo_color = Color(0.1, 0.9, 0.2)
		skin_amesh.surface_set_material(0, skin_smat)
		var skin_smi := MeshInstance3D.new()
		skin_smi.mesh = skin_amesh
		skin_skel.add_child(skin_smi)
		skin_smi.skeleton = skin_smi.get_path_to(skin_skel)
		skin_skel.position = Vector3(4.5, 0, -4.5)
		root.add_child(skin_skel)
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
	if arg("room", "0") == "1":
		# Closed room around the camera (walls down to the floor, ceiling): a light leak test, nothing outside should show.
		var cx := side * 1.5
		var walls := [
			[Vector3(0.3, 13, 12.3), Vector3(cx - 6, 3.5, 8)], [Vector3(0.3, 13, 12.3), Vector3(cx + 6, 3.5, 8)],
			[Vector3(12.3, 13, 0.3), Vector3(cx, 3.5, 2)], [Vector3(12.3, 13, 0.3), Vector3(cx, 3.5, 14)],
			[Vector3(12.3, 0.3, 12.3), Vector3(cx, 10, 8)]]
		for wdef in walls:
			var bm := BoxMesh.new()
			bm.size = wdef[0]
			var wmi := MeshInstance3D.new()
			wmi.mesh = bm
			wmi.position = wdef[1]
			root.add_child(wmi)
	if arg("multimesh", "0") == "1":
		# A MultiMesh row behind the grid: its cards come from the dynamic-instance path.
		var mm := MultiMesh.new()
		mm.transform_format = MultiMesh.TRANSFORM_3D
		mm.mesh = meshes[0]
		mm.instance_count = 8
		for k in 8:
			mm.set_instance_transform(k, Transform3D(Basis(), Vector3(side * 1.5 - 10.5 + k * 3.0, 0, -(side - 1) * 3.0 - 3.0)))
		var mmi := MultiMeshInstance3D.new()
		mmi.multimesh = mm
		root.add_child(mmi)
	var scaling := arg("scaling", "")
	if scaling != "":
		var modes := {"bilinear": Viewport.SCALING_3D_MODE_BILINEAR, "fsr": Viewport.SCALING_3D_MODE_FSR, "fsr2": Viewport.SCALING_3D_MODE_FSR2, "tsr": Viewport.SCALING_3D_MODE_TSR}
		get_root().scaling_3d_mode = modes[scaling]
		get_root().scaling_3d_scale = float(arg("scale", "0.5"))
	wall_off = int(arg("wall_off", "-1"))
	move_frame = int(arg("move", "-1"))
	anim = arg("anim", "0") == "1"
	cut_frame = int(arg("cut", "-1"))
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
	if skin_skel:
		skin_skel.set_bone_pose_rotation(1, Quaternion(Vector3(0, 0, 1), sin(float(frame) * 0.1) * 0.8))
	if cam_move != 0.0 and cam and frame > 0:
		cam.position.x += cam_move
	frame += 1
	if cam and frame == cut_frame:
		cam.position.x += 5.0 # Camera cut: same jump as --cam_x=5.
	if first_mi and anim:
		# Keeps one caster moving so that its shadow pages are redrawn every frame.
		if frame == 1:
			anim_base = first_mi.position
		first_mi.position = anim_base + Vector3(sin(float(frame) * 0.2) * 0.5, 0, 0)
	if frame > warmup:
		shadow_tris_max = maxi(shadow_tris_max, RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_GEOMETRY_SHADOW_PRIMITIVES_IN_FRAME))
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
		print("RESULT vg_shadow_tris_max=%d" % shadow_tris_max)
		print("RESULT residency vg_pool=%d/%d vg_installed=%d vsm_pages=%d/%d vsm_req=%d vsm_miss=%d vsm_rend=%d vsm_drop=%d sc_inst=%d sc_cards=%d sc_captured=%d sc_texels=%d" % [
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_GEOMETRY_PAGE_POOL_SLOTS_USED),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_GEOMETRY_PAGE_POOL_SLOTS),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_GEOMETRY_PAGES_INSTALLED_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_SHADOW_PAGES_RESIDENT),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_SHADOW_POOL_PAGES),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_SHADOW_PAGES_REQUESTED_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_SHADOW_PAGES_MISSING_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_SHADOW_PAGES_RENDERED_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_SHADOW_PAGES_DROPPED_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_SURFACE_CACHE_INSTANCES),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_SURFACE_CACHE_CARDS),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_SURFACE_CACHE_CARDS_CAPTURED_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_SURFACE_CACHE_ATLAS_TEXELS_USED)])
		print("RESULT vg_clusters=%d vg_tris=%d vg_shadow_tris=%d total_prims=%d draw_calls=%d" % [
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_GEOMETRY_CLUSTERS_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_GEOMETRY_PRIMITIVES_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIRTUAL_GEOMETRY_SHADOW_PRIMITIVES_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME),
			RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME)])
		return true
	return false

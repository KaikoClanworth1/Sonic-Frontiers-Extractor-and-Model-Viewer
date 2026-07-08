"""Render an FBX to a PNG using headless Blender (bpy), for visual verification."""
import sys, math
import bpy


def render(fbx_path, out_png):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=fbx_path)

    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    # compute bounds
    import mathutils
    mn = mathutils.Vector((1e9, 1e9, 1e9))
    mx = mathutils.Vector((-1e9, -1e9, -1e9))
    for o in meshes:
        for v in o.bound_box:
            wv = o.matrix_world @ mathutils.Vector(v)
            mn = mathutils.Vector((min(mn[i], wv[i]) for i in range(3)))
            mx = mathutils.Vector((max(mx[i], wv[i]) for i in range(3)))
    center = (mn + mx) / 2
    size = max((mx - mn)) or 1.0
    print(f"bounds min={tuple(round(x,2) for x in mn)} max={tuple(round(x,2) for x in mx)} "
          f"center={tuple(round(x,2) for x in center)} size={size:.2f}")

    # camera
    cam_data = bpy.data.cameras.new("Cam")
    cam = bpy.data.objects.new("Cam", cam_data)
    bpy.context.scene.collection.objects.link(cam)
    bpy.context.scene.camera = cam
    cam.location = (center[0] + size * 1.6, center[1] - size * 1.8, center[2] + size * 0.7)
    d = center - cam.location
    cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
    cam_data.clip_start = size * 1e-3
    cam_data.clip_end = size * 1000.0

    # light
    light_data = bpy.data.lights.new("Sun", type="SUN")
    light_data.energy = 3.0
    light = bpy.data.objects.new("Sun", light_data)
    bpy.context.scene.collection.objects.link(light)
    light.rotation_euler = (math.radians(50), math.radians(20), math.radians(30))

    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"       # flat solid shading, no lights needed
    try:
        sc.display.shading.light = "STUDIO"
        sc.display.shading.color_type = "SINGLE"
        sc.display.shading.single_color = (0.6, 0.6, 0.62)
    except Exception:
        pass
    sc.world = bpy.data.worlds.new("W")
    sc.world.color = (0.15, 0.15, 0.18)
    sc.render.resolution_x = 800
    sc.render.resolution_y = 800
    sc.render.film_transparent = False
    sc.render.filepath = out_png
    bpy.ops.render.render(write_still=True)
    print("rendered", out_png)


if __name__ == "__main__":
    render(sys.argv[1], sys.argv[2])

"""Blender script: the exported banner model as the HOME menu would draw it, for checking before a console test.
    blender -b --factory-startup -P tools/build/art/preview.py -- --gltf build/art/banner.gltf --out-dir build/art
Imports the glTF, shows every texture unlit (as make_art.py sets the CGFX materials), and renders through the
HOME menu's banner camera at the top screen's 400 x 240: preview_3ds.png from the centre, preview_left.png and
preview_right.png a little apart, converging on the banner's middle depth. The eye spacing is a guess, so the
stereo pair shows which parts stand out and whether anything hidden shows at the edges, not the exact depth.
"""
import argparse
import math
import os
import sys

import bpy

ap = argparse.ArgumentParser()
ap.add_argument("--gltf", required=True)
ap.add_argument("--out-dir", required=True)
ap.add_argument("--eye-spacing", type=float, default=1.2)
args = ap.parse_args(sys.argv[sys.argv.index("--") + 1:])

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
bpy.ops.import_scene.gltf(filepath=os.path.abspath(args.gltf))

for mat in bpy.data.materials:
    if not mat.use_nodes:
        continue
    nt = mat.node_tree
    tex = next((n for n in nt.nodes if n.type == "TEX_IMAGE"), None)
    out = next(n for n in nt.nodes if n.type == "OUTPUT_MATERIAL")
    emit = nt.nodes.new("ShaderNodeEmission")
    if tex:
        nt.links.new(tex.outputs["Color"], emit.inputs["Color"])
    nt.links.new(emit.outputs["Emission"], out.inputs["Surface"])

# the HOME menu's banner camera, glTF (0, 1, 44.786) looking down -Z, in Blender's Z-up axes
HOME_DIST = 44.786
cam_data = bpy.data.cameras.new("home")
cam_data.sensor_fit = "VERTICAL"
cam_data.sensor_height = 24
cam_data.lens = 12 / math.tan(math.radians(15))
cam = bpy.data.objects.new("home", cam_data)
scene.collection.objects.link(cam)
cam.rotation_euler = (math.radians(90), 0, 0)
scene.camera = cam
frame_width = 2 * HOME_DIST * math.tan(math.radians(15)) * 400 / 240

scene.render.engine = "CYCLES"
scene.cycles.samples = 16
scene.cycles.device = "CPU"
scene.view_settings.view_transform = "Standard"
scene.world = bpy.data.worlds.new("black")
scene.world.color = (0, 0, 0)
scene.render.resolution_x = 400
scene.render.resolution_y = 240

for name, eye in (("3ds", 0.0), ("left", -1.0), ("right", 1.0)):
    x = eye * args.eye_spacing / 2
    cam.location = (x, -HOME_DIST, 1.0)
    # off-axis, so both eyes frame the same rectangle at the camera's distance from the origin
    cam_data.shift_x = -x / frame_width
    scene.render.filepath = os.path.join(os.path.abspath(args.out_dir), f"preview_{name}.png")
    bpy.ops.render.render(write_still=True)

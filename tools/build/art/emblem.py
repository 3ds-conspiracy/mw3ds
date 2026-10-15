"""Blender scene for the CIA's HOME menu art: gold "MW", "DS" and a "3" rune on an inverted-triangle emblem.
Runs inside Blender (3.6); tools/build/make_art.py calls it and turns the renders into banner.png and icon.png.
    blender -b --factory-startup -P tools/build/art/emblem.py -- --font tools/build/art/Cinzel-650.ttf --out x.png
        [--mode banner|icon|model] [--out-dir folder]
  banner  the whole title over the emblem on dark stone, 1024 x 512, seen a little from below so the depth shows
  icon    the emblem alone, square, straight on, its rune larger, on a plain dark backdrop (stone is mud at 48 px)
  model   the 3D banner: the scene framed by the HOME menu's banner camera, rendered as three layers (title, emblem,
          wall) each projected onto its own low-poly mesh, written to --out-dir as banner.gltf with one texture per
          layer (placeholders; make_art.py fills them from <layer>_render.png and layers.json) and preview.png
Everything is drawn here: the emblem and rune are original shapes, the lettering is Cinzel (SIL OFL, see OFL.txt).
"""
import argparse
import math
import os
import sys

import bpy

ap = argparse.ArgumentParser()
ap.add_argument("--font", required=True)
ap.add_argument("--out")
ap.add_argument("--out-dir")
ap.add_argument("--mode", choices=("banner", "icon", "model"), default="banner")
args = ap.parse_args(sys.argv[sys.argv.index("--") + 1:])
ICON = args.mode == "icon"
MODEL = args.mode == "model"

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene


def node(nt, kind, loc, **inputs):
    n = nt.nodes.new(kind)
    n.location = loc
    for k, v in inputs.items():
        n.inputs[k].default_value = v
    return n


def link(obj):
    scene.collection.objects.link(obj)
    return obj


# --- materials ----------------------------------------------------------------------------------------------------
def metal(name, base, tarnish, tarnish_amount, rough_lo, rough_hi, grunge_scale=7.0):
    """Worn metal: tarnish in the recesses and in blotches, fine pitting on the faces."""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = node(nt, "ShaderNodeOutputMaterial", (900, 0))
    bsdf = node(nt, "ShaderNodeBsdfPrincipled", (600, 0))
    coord = node(nt, "ShaderNodeTexCoord", (-900, 0))
    grunge = node(nt, "ShaderNodeTexNoise", (-600, 200), Scale=grunge_scale, Detail=14.0, Roughness=0.7)
    pits = node(nt, "ShaderNodeTexVoronoi", (-600, -200), Scale=55.0)
    fine = node(nt, "ShaderNodeTexNoise", (-600, -450), Scale=120.0, Detail=8.0)
    for t in (grunge, pits, fine):
        nt.links.new(coord.outputs["Object"], t.inputs["Vector"])
    ao = node(nt, "ShaderNodeAmbientOcclusion", (-600, 450), Distance=0.05)
    ao.samples = 8
    cav = node(nt, "ShaderNodeMath", (-350, 450))
    cav.operation = "SUBTRACT"
    cav.inputs[0].default_value = 1.0
    nt.links.new(ao.outputs["AO"], cav.inputs[1])
    gr = node(nt, "ShaderNodeMapRange", (-350, 200), **{"From Min": 0.5, "From Max": 0.78})
    nt.links.new(grunge.outputs["Fac"], gr.inputs["Value"])
    mask = node(nt, "ShaderNodeMath", (-100, 300))
    mask.operation = "MAXIMUM"
    mask.use_clamp = True
    nt.links.new(cav.outputs["Value"], mask.inputs[0])
    nt.links.new(gr.outputs["Result"], mask.inputs[1])
    amount = node(nt, "ShaderNodeMath", (100, 300))
    amount.operation = "MULTIPLY"
    amount.use_clamp = True
    amount.inputs[1].default_value = tarnish_amount
    nt.links.new(mask.outputs["Value"], amount.inputs[0])
    col = node(nt, "ShaderNodeMixRGB", (300, 250))
    col.inputs["Color1"].default_value = base + (1,)
    col.inputs["Color2"].default_value = tarnish + (1,)
    nt.links.new(amount.outputs["Value"], col.inputs["Fac"])
    nt.links.new(col.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Metallic"].default_value = 1.0
    rough = node(nt, "ShaderNodeMapRange", (300, -100), **{"To Min": rough_lo, "To Max": rough_hi})
    nt.links.new(amount.outputs["Value"], rough.inputs["Value"])
    nt.links.new(rough.outputs["Result"], bsdf.inputs["Roughness"])
    bumpmix = node(nt, "ShaderNodeMath", (100, -300))
    bumpmix.operation = "ADD"
    nt.links.new(pits.outputs["Distance"], bumpmix.inputs[0])
    nt.links.new(fine.outputs["Fac"], bumpmix.inputs[1])
    bump = node(nt, "ShaderNodeBump", (350, -300), Strength=0.22, Distance=0.003)
    nt.links.new(bumpmix.outputs["Value"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    return m


# deep yellow gold for the title and rune
gold = metal("gold", (0.88, 0.56, 0.10), (0.20, 0.09, 0.015), 0.45, 0.22, 0.6)
# the emblem's frame: the same gold, darker and more worn so the title stays brightest
frame_gold = metal("frame", (0.60, 0.37, 0.07), (0.12, 0.05, 0.015), 0.75, 0.32, 0.75, 4.0)
# the emblem's face: dark bronze, mostly tarnished
bronze = metal("face", (0.05, 0.032, 0.018), (0.025, 0.015, 0.01), 0.8, 0.6, 0.9, 3.0)

# --- title -------------------------------------------------------------------------------------------------------
# an absolute path: Blender reads a relative one against its own folder
font = bpy.data.fonts.load(os.path.abspath(args.font))
TITLE_Y = -0.55  # depth of the title and rune, in front of the emblem
TEXT_MID = 0.1   # height of the title's centre line


def title_letter(ch):
    cu = bpy.data.curves.new("title_" + ch, "FONT")
    cu.body = ch
    cu.font = font
    cu.align_x = "LEFT"
    cu.align_y = "BOTTOM_BASELINE"
    cu.size = 1.25
    cu.extrude = 0.11
    cu.bevel_depth = 0.022
    cu.bevel_resolution = 4
    cu.materials.append(gold)
    obj = link(bpy.data.objects.new(cu.name, cu))
    obj.rotation_euler = (math.radians(90), 0, 0)
    return obj


def local_box(obj):
    bb = obj.evaluated_get(bpy.context.evaluated_depsgraph_get()).bound_box
    xs = [v[0] for v in bb]
    ys = [v[1] for v in bb]
    return min(xs), max(xs), min(ys), max(ys)


# one object per letter so the spacing is set by hand: M W [rune] D S
letters = [title_letter(ch) for ch in "MWDS"]
bpy.context.view_layer.update()
_, _, cap_lo, cap_hi = local_box(letters[0])


# --- emblem ------------------------------------------------------------------------------------------------------
def flat_shape(name, loops, extrude, bevel, mat, y, bevel_resolution=3):
    """A filled, extruded 2D shape standing upright; each loop is a closed list of (x, z) points."""
    cu = bpy.data.curves.new(name, "CURVE")
    cu.dimensions = "2D"
    cu.fill_mode = "BOTH"
    cu.extrude = extrude
    cu.bevel_depth = bevel
    cu.bevel_resolution = bevel_resolution
    for pts in loops:
        sp = cu.splines.new("POLY")
        sp.points.add(len(pts) - 1)
        for p, (x, z) in zip(sp.points, pts):
            p.co = (x, z, 0, 1)
        sp.use_cyclic_u = True
    cu.materials.append(mat)
    obj = link(bpy.data.objects.new(name, cu))
    obj.rotation_euler = (math.radians(90), 0, 0)
    obj.location = (0, y, 0)
    return obj


def inset(tri, d):
    """Shrink a triangle about its incentre so every edge moves in by d (an even border on all three sides)."""
    a, b, c = tri
    la, lb, lc = math.dist(b, c), math.dist(a, c), math.dist(a, b)
    s = (la + lb + lc) / 2
    r = math.sqrt(s * (s - la) * (s - lb) * (s - lc)) / s
    k = (r - d) / r
    ix = (la * a[0] + lb * b[0] + lc * c[0]) / (2 * s)
    iz = (la * a[1] + lb * b[1] + lc * c[1]) / (2 * s)
    return [(ix + (x - ix) * k, iz + (z - iz) * k) for x, z in tri]


def stroke(points, widths):
    """Outline of a polyline drawn with a varying pen width, mitred at the corners."""
    left, right = [], []
    for i, (px, pz) in enumerate(points):
        dirs = []
        if i > 0:
            dirs.append((px - points[i - 1][0], pz - points[i - 1][1]))
        if i < len(points) - 1:
            dirs.append((points[i + 1][0] - px, points[i + 1][1] - pz))
        norms = [(-dz / math.hypot(dx, dz), dx / math.hypot(dx, dz)) for dx, dz in dirs]
        nx = sum(v[0] for v in norms)
        nz = sum(v[1] for v in norms)
        length = math.hypot(nx, nz)
        nx, nz = nx / length, nz / length
        # mitre length, capped so sharp corners do not spike
        w = widths[i] / 2 / max(0.45, nx * norms[0][0] + nz * norms[0][1])
        left.append((px + nx * w, pz + nz * w))
        right.append((px - nx * w, pz - nz * w))
    return left + right[::-1]


tri = [(-1.2, 1.28), (1.2, 1.28), (0.0, -1.42)]
flat_shape("frame", [tri, inset(tri, 0.14)], 0.07, 0.015, frame_gold, 0.25)
flat_shape("face", [inset(tri, 0.1)], 0.02, 0.0, bronze, 0.32)
flat_shape("tail", [[(0.0, -0.72), (0.07, -0.84), (0.0, -0.98), (-0.07, -0.84)]], 0.05, 0.01, frame_gold, 0.27)

# the "3" rune: a flat top stroke, a diagonal into the waist, a sharp out-turn and a hooked tail, cut like a carved
# glyph with tapered ends, a barb on the top stroke and a dot in its open side
rune = stroke([(-0.30, 1.22), (0.26, 1.22), (-0.04, 0.88), (0.30, 0.62), (0.10, 0.30), (-0.28, 0.22)],
              [0.04, 0.13, 0.12, 0.13, 0.11, 0.02])
barb = stroke([(-0.42, 1.30), (-0.30, 1.22), (-0.38, 1.10)], [0.02, 0.07, 0.02])
dot = [(-0.36, 0.72), (-0.27, 0.62), (-0.36, 0.52), (-0.45, 0.62)]
xs = [x for pts in (rune, barb, dot) for x, _ in pts]
zs = [z for pts in (rune, barb, dot) for _, z in pts]
# a little taller than the capitals, level with them
k = (cap_hi - cap_lo) * 1.18 / (max(zs) - min(zs))
half = (max(xs) - min(xs)) * k / 2
# the 3 is open on the left, so its visual weight sits right of its box: a hair right of the frame's centre line
RUNE_X = 0.06
rx, rz = (max(xs) + min(xs)) / 2, (max(zs) + min(zs)) / 2
rune, barb, dot = [[(RUNE_X + (x - rx) * k, TEXT_MID + (z - rz) * k) for x, z in pts] for pts in (rune, barb, dot)]
for name, pts, bevel in (("rune", rune, 0.022), ("barb", barb, 0.015), ("dot", dot, 0.02)):
    flat_shape(name, [pts], 0.11, bevel, gold, TITLE_Y, 4)

# Kerning. The emblem is centred and so is the word, with the rune between W and D. MW is much wider than DS:
# M and W are drawn 10% narrower and D and S 12% wider (too little to see on Roman capitals), the left gaps are
# set by eye (the W's slanted sides read as extra space, the rune's dot as less) and the two right gaps share what
# is left so both ends of the word reach equally far.
GAP_MW, GAP_W3 = 0.14, 0.22
for obj, sx in zip(letters, (0.9, 0.9, 1.12, 1.12)):
    obj.scale = (sx, 1, 1)
boxes = [local_box(o) for o in letters]
boxes = [(bx[0] * o.scale[0], bx[1] * o.scale[0]) for o, bx in zip(letters, boxes)]
wM, wW, wD, wS = [b1 - b0 for b0, b1 in boxes]
gap_r = (GAP_W3 + wW + GAP_MW + wM - wD - wS - 2 * RUNE_X) / 2
lift = TEXT_MID - (cap_lo + cap_hi) / 2
x = RUNE_X - half - GAP_W3 - wW
letters[1].location = (x - boxes[1][0], TITLE_Y, lift)
x -= GAP_MW + wM
letters[0].location = (x - boxes[0][0], TITLE_Y, lift)
x = RUNE_X + half + gap_r
letters[2].location = (x - boxes[2][0], TITLE_Y, lift)
x += wD + gap_r
letters[3].location = (x - boxes[3][0], TITLE_Y, lift)

# --- backdrop ----------------------------------------------------------------------------------------------------
bpy.ops.mesh.primitive_plane_add(size=1, location=(0, 0.45, 0), rotation=(math.radians(90), 0, 0))
wall = bpy.context.object
wall.name = "wall"
wall.scale = (12, 7, 1)
bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
stone = bpy.data.materials.new("stone")
stone.use_nodes = True
nt = stone.node_tree
nt.nodes.clear()
out = node(nt, "ShaderNodeOutputMaterial", (900, 0))
sb = node(nt, "ShaderNodeBsdfPrincipled", (600, 0), Roughness=0.9)
nt.links.new(sb.outputs["BSDF"], out.inputs["Surface"])
if ICON:
    sb.inputs["Base Color"].default_value = (0.012, 0.011, 0.011, 1)
    sb.inputs["Roughness"].default_value = 1.0
else:
    # dark ash stone
    coord = node(nt, "ShaderNodeTexCoord", (-700, 0))
    n1 = node(nt, "ShaderNodeTexNoise", (-450, 150), Scale=1.5, Detail=12.0, Roughness=0.6)
    n2 = node(nt, "ShaderNodeTexNoise", (-450, -150), Scale=12.0, Detail=6.0)
    nt.links.new(coord.outputs["Object"], n1.inputs["Vector"])
    nt.links.new(coord.outputs["Object"], n2.inputs["Vector"])
    ramp = node(nt, "ShaderNodeValToRGB", (-200, 150))
    cr = ramp.color_ramp
    cr.elements[0].position = 0.35
    cr.elements[0].color = (0.004, 0.004, 0.005, 1)
    cr.elements[1].position = 0.7
    cr.elements[1].color = (0.016, 0.015, 0.015, 1)
    nt.links.new(n1.outputs["Fac"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], sb.inputs["Base Color"])
    sbump = node(nt, "ShaderNodeBump", (300, -150), Strength=0.4)
    nt.links.new(n2.outputs["Fac"], sbump.inputs["Height"])
    nt.links.new(sbump.outputs["Normal"], sb.inputs["Normal"])
wall.data.materials.append(stone)


# --- lights ------------------------------------------------------------------------------------------------------
def area_light(name, loc, rot, energy, color, size):
    data = bpy.data.lights.new(name, "AREA")
    data.energy = energy
    data.color = color
    data.size = size
    obj = link(bpy.data.objects.new(name, data))
    obj.location = loc
    obj.rotation_euler = [math.radians(a) for a in rot]


# a raking key from the upper left lights the letters' top bevels and throws their sides into shade
area_light("key", (-2.5, -5.0, 2.6), (62, 0, -27), 750, (1.0, 0.95, 0.85), 2.0)
area_light("fill", (3.5, -3.5, -1.0), (100, 0, 45), 70, (0.75, 0.82, 1.0), 4.0)

# the world shows only in reflections: a bright warm top fading to dark, for the gold to mirror
world = bpy.data.worlds.new("world")
scene.world = world
world.use_nodes = True
nt = world.node_tree
wc = node(nt, "ShaderNodeTexCoord", (-700, 0))
ws = node(nt, "ShaderNodeSeparateXYZ", (-500, 0))
nt.links.new(wc.outputs["Generated"], ws.inputs["Vector"])
wr = node(nt, "ShaderNodeValToRGB", (-300, 0))
wr.color_ramp.elements[0].position = 0.45
wr.color_ramp.elements[0].color = (0.02, 0.018, 0.016, 1)
wr.color_ramp.elements[1].position = 0.75
wr.color_ramp.elements[1].color = (1.3, 1.15, 0.95, 1)
nt.links.new(ws.outputs["Z"], wr.inputs["Fac"])
nt.links.new(wr.outputs["Color"], nt.nodes["Background"].inputs["Color"])

# --- camera and render -------------------------------------------------------------------------------------------
cam_data = bpy.data.cameras.new("cam")
cam = link(bpy.data.objects.new("cam", cam_data))
scene.camera = cam
if ICON:
    for obj in letters:
        obj.hide_render = True
    # with no title beside it the rune grows, centred where the inverted triangle's weight is
    grow, cx, cz = 1.35, 0.02, 0.3
    for name in ("rune", "barb", "dot"):
        obj = bpy.data.objects[name]
        obj.scale = (grow, grow, 1)
        obj.location.x = cx - grow * RUNE_X
        obj.location.z = cz - grow * TEXT_MID
    # straight on and close, the triangle filling the square
    cam_data.lens = 100
    cam.location = (0.0, -8.0, -0.07)
    cam.rotation_euler = (math.radians(90), 0, 0)
    scene.render.resolution_x = scene.render.resolution_y = 384
else:
    # a little from below so the letters' depth shows
    cam_data.lens = 46
    cam.location = (0.0, -8.0, -0.9)
    cam.rotation_euler = (math.radians(96.5), 0, 0)
    scene.render.resolution_x = 1024
    scene.render.resolution_y = 512

scene.render.engine = "CYCLES"
scene.cycles.samples = 96
scene.cycles.use_denoising = True
scene.cycles.device = "CPU"
scene.view_settings.view_transform = "Filmic"
scene.view_settings.look = "Medium High Contrast"
if not MODEL:
    scene.render.filepath = args.out
    bpy.ops.render.render(write_still=True)
    sys.exit(0)

# --- the 3D banner ----------------------------------------------------------------------------------------------
# The HOME menu draws the banner's CGFX with its own camera (pycgfx's banner-camera.gltf): at (0, 1, 44.786) looking
# down -Z, 30 degrees vertical field of view, 5:3. Blender's camera here matches it at a distance of CAM_DIST scene
# units; the export then scales the scene by HOME_DIST / CAM_DIST, which leaves the picture exactly as framed here.
import json
from pathlib import Path

from bpy_extras.object_utils import world_to_camera_view

HOME_DIST, HOME_Y = 44.786, 1.0
CAM_DIST = 6.9
CENTER_Z = -0.05  # the composition's centre height, which lands on the HOME camera's axis
out_dir = Path(args.out_dir)
out_dir.mkdir(parents=True, exist_ok=True)
cam_data.sensor_fit = "VERTICAL"
cam_data.sensor_height = 24
cam_data.lens = 12 / math.tan(math.radians(15))
cam.location = (0.0, -CAM_DIST, CENTER_Z)
cam.rotation_euler = (math.radians(90), 0, 0)
scene.render.resolution_x = 1000
scene.render.resolution_y = 600
scene.cycles.samples = 64
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "RGBA"

layers = {
    "title": [o.name for o in letters] + ["rune", "barb", "dot"],
    "emblem": ["frame", "face", "tail"],
    "wall": ["wall"],
}
# texture size per layer, inside the 512 KB the HOME menu allows a banner (RGB8: 3 bytes a texel)
tex_size = {"title": (256, 64), "emblem": (128, 256), "wall": (256, 128)}

scene.render.filepath = str(out_dir / "preview.png")
bpy.ops.render.render(write_still=True)

# each layer alone in front of the camera, the others still casting their shadows and showing in reflections, so a
# layer's texture holds nothing that belongs to another (in stereo the eyes see a little behind the layers in front)
scene.render.film_transparent = True
for layer, names in layers.items():
    for obj in scene.objects:
        if obj.type in ("MESH", "CURVE", "FONT"):
            obj.visible_camera = obj.name in names
    scene.render.filepath = str(out_dir / f"{layer}_render.png")
    bpy.ops.render.render(write_still=True)

# curves to meshes, one mesh per layer. The meshes go to the 3DS as they are, so they are lighter than what was
# rendered: fewer steps along the curves and a single chamfer for the rounded bevel, the same outline either way
# (the texture carries the detail)
bpy.ops.object.select_all(action="DESELECT")
for obj in scene.objects:
    if obj.type in ("CURVE", "FONT"):
        obj.data.resolution_u = 3
        obj.data.bevel_resolution = 0
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
bpy.ops.object.convert(target="MESH")
meshes = {}
for layer, names in layers.items():
    bpy.ops.object.select_all(action="DESELECT")
    for name in names:
        bpy.data.objects[name].select_set(True)
    bpy.context.view_layer.objects.active = bpy.data.objects[names[0]]
    if len(names) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    obj.name = layer
    meshes[layer] = obj

# project each layer's render onto its mesh: a vertex's UV is where the camera sees it, within the layer's crop
crops = {}
for layer, obj in meshes.items():
    me = obj.data
    seen = [world_to_camera_view(scene, cam, obj.matrix_world @ v.co) for v in me.vertices]
    if layer == "wall":
        x0, y0, x1, y1 = 0.0, 0.0, 1.0, 1.0
    else:
        pad = 0.004
        x0 = max(0.0, min(p.x for p in seen) - pad)
        x1 = min(1.0, max(p.x for p in seen) + pad)
        y0 = max(0.0, min(p.y for p in seen) - pad)
        y1 = min(1.0, max(p.y for p in seen) + pad)
    crops[layer] = (x0, y0, x1, y1)
    # converted curves come with a UV map of their own, which the export would take as the first
    while me.uv_layers:
        me.uv_layers.remove(me.uv_layers[0])
    uv = me.uv_layers.new(name="UVMap")
    for loop in me.loops:
        p = seen[loop.vertex_index]
        uv.data[loop.index].uv = ((p.x - x0) / (x1 - x0), (p.y - y0) / (y1 - y0))
    me.materials.clear()
    mat = bpy.data.materials.new(layer)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    bsdf.inputs["Metallic"].default_value = 0.0
    bsdf.inputs["Roughness"].default_value = 1.0
    w, h = tex_size[layer]
    img = bpy.data.images.new(layer, w, h)
    img.filepath_raw = str(out_dir / f"{layer}.png")
    img.file_format = "PNG"
    img.save()
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    tex.extension = "EXTEND"
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    me.materials.append(mat)

# into the HOME menu's space: the composition's centre onto the camera's axis, then the scale
scale = HOME_DIST / CAM_DIST
bpy.ops.object.select_all(action="DESELECT")
for obj in meshes.values():
    obj.location.z += HOME_Y / scale - CENTER_Z
    obj.location *= scale
    obj.scale *= scale
    obj.select_set(True)
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
for obj in meshes.values():
    obj.data.calc_loop_triangles()
    print(f"layer {obj.name}: {len(obj.data.vertices)} vertices, {len(obj.data.loop_triangles)} triangles")
bpy.ops.export_scene.gltf(filepath=str(out_dir / "banner.gltf"), export_format="GLTF_SEPARATE", use_selection=True,
                          export_cameras=False, export_lights=False, export_texcoords=True, export_normals=True,
                          export_materials="EXPORT")
layout = {layer: {"crop": crops[layer], "size": tex_size[layer]} for layer in layers}
(out_dir / "layers.json").write_text(json.dumps(layout, indent=1))

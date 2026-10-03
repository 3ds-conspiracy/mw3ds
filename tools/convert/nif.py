"""NetImmerse 4.0.0.2 (Morrowind) NIF reader.

Blocks carry no size field, so every block type that occurs must be parsed
exactly. Geometry, nodes, properties and textures are kept; animation,
particle and effect blocks are parsed only to be skipped.
Layouts follow niftools nif.xml filtered to version 4.0.0.2.
"""
import struct


class NifError(Exception):
    pass


class Reader:
    def __init__(self, data):
        self.d, self.p = data, 0

    def u8(self):
        v = self.d[self.p]; self.p += 1; return v

    def u16(self):
        v = struct.unpack_from("<H", self.d, self.p)[0]; self.p += 2; return v

    def i16(self):
        v = struct.unpack_from("<h", self.d, self.p)[0]; self.p += 2; return v

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.p)[0]; self.p += 4; return v

    def i32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]; self.p += 4; return v

    def f32(self):
        v = struct.unpack_from("<f", self.d, self.p)[0]; self.p += 4; return v

    def floats(self, n):
        v = struct.unpack_from(f"<{n}f", self.d, self.p); self.p += 4 * n; return v

    def u16s(self, n):
        v = struct.unpack_from(f"<{n}H", self.d, self.p); self.p += 2 * n; return v

    def skip(self, n):
        self.p += n

    def boolean(self):  # 32-bit before 4.1.0.1
        return self.u32() != 0

    def string(self):
        n = self.u32()
        if n > 100000:
            raise NifError(f"bad string length {n} at {self.p - 4}")
        s = self.d[self.p:self.p + n].decode("latin-1"); self.p += n; return s

    def ref(self):
        return self.i32()

    def refs(self):
        return [self.i32() for _ in range(self.u32())]


# ---- shared pieces ----

def obj_net(r, b):
    b["name"] = r.string()
    b["extra"] = r.ref()
    b["controller"] = r.ref()


def bounding_volume(r):
    t = r.u32()
    if t == 0:
        r.skip(16)                 # sphere
    elif t == 1:
        r.skip(60)                 # box: center, 3 axes, extents
    elif t == 2:
        r.skip(32)                 # capsule
    elif t == 4:
        for _ in range(r.u32()):   # union
            bounding_volume(r)
    elif t == 5:
        r.skip(28)                 # half space: plane + center
    else:
        raise NifError(f"bounding volume type {t}")


def av_object(r, b):
    obj_net(r, b)
    b["flags"] = r.u16()
    b["translation"] = r.floats(3)
    b["rotation"] = r.floats(9)    # row-major, column-vector convention
    b["scale"] = r.f32()
    r.skip(12)                     # velocity
    b["properties"] = r.refs()
    if r.boolean():
        bounding_volume(r)


def node(r, b):
    av_object(r, b)
    b["children"] = r.refs()
    b["effects"] = r.refs()


def geometry(r, b):
    av_object(r, b)
    b["data"] = r.ref()
    b["skin"] = r.ref()


def geometry_data(r, b):
    n = r.u16()
    b["num_vertices"] = n
    b["vertices"] = r.floats(3 * n) if r.boolean() else None
    b["normals"] = r.floats(3 * n) if r.boolean() else None
    r.skip(16)                     # bounding sphere
    b["colors"] = r.floats(4 * n) if r.boolean() else None
    num_uv = r.u16() & 63
    has_uv = r.boolean()
    b["uv_sets"] = [r.floats(2 * n) for _ in range(num_uv)] if has_uv else []


def key_group(r, size):
    """KeyGroup<T>: size = floats per value (1 float, 3 vec3, 4 color).
    Returns [(time, value tuple)]; tangents / TBC parameters are dropped."""
    n = r.u32()
    if n == 0:
        return []
    interp = r.u32()
    per_key = {1: 1 + size, 2: 1 + 3 * size, 3: 1 + size + 3, 5: 1 + size}.get(interp)
    if per_key is None:
        raise NifError(f"key interpolation {interp}")
    v = r.floats(per_key * n)
    return [(v[i * per_key], v[i * per_key + 1:i * per_key + 1 + size]) for i in range(n)]


def time_controller(r, b):
    b["next_controller"] = r.ref()
    r.skip(2 + 16)                 # flags, frequency, phase, start, stop
    b["target"] = r.ref()


def extra_data(r, b):
    b["next_extra"] = r.ref()
    b["num_bytes"] = r.u32()


def tex_desc(r):
    src = r.ref()
    clamp = r.u32()
    r.skip(4)                      # filter mode
    uv_set = r.u32()
    r.skip(6)                      # PS2 L/K, unknown short
    return {"source": src, "clamp": clamp, "uv_set": uv_set}


def particle_modifier(r, b):
    b["next_modifier"] = r.ref()
    r.skip(4)                      # controller ptr


# ---- block readers ----

def rd_node(r, b):
    node(r, b)


def rd_geometry(r, b):
    geometry(r, b)


def rd_tri_shape_data(r, b):
    geometry_data(r, b)
    ntri = r.u16()
    r.skip(4)                      # num triangle points
    b["triangles"] = r.u16s(3 * ntri)
    for _ in range(r.u16()):       # match groups
        r.skip(2 * r.u16())


def rd_tri_strips_data(r, b):
    geometry_data(r, b)
    r.skip(2)                      # num triangles
    lengths = r.u16s(r.u16())
    b["strips"] = [r.u16s(n) for n in lengths]


def rd_particles_data(r, b):
    geometry_data(r, b)
    r.skip(2 + 4 + 2)              # num particles, radius, num active
    if r.boolean():
        r.skip(4 * b["num_vertices"])


def rd_rotating_particles_data(r, b):
    rd_particles_data(r, b)
    if r.boolean():
        r.skip(16 * b["num_vertices"])


def rd_texturing_property(r, b):
    obj_net(r, b)
    b["flags"] = r.u16()
    b["apply"] = r.u32()
    count = r.u32()
    b["textures"] = []
    for i in range(count):
        t = tex_desc(r) if r.boolean() else None
        b["textures"].append(t)
        if i == 5 and t:
            r.skip(24)             # bump luma scale/offset + 2x2 matrix


def rd_source_texture(r, b):
    obj_net(r, b)
    external = r.u8()
    b["file"] = None
    if external == 1:
        b["file"] = r.string()
    else:
        internal = r.u8()
        if internal == 1:
            b["pixel_data"] = r.ref()
    r.skip(12 + 1)                 # format prefs, is static


def rd_material_property(r, b):
    obj_net(r, b)
    b["flags"] = r.u16()
    b["ambient"] = r.floats(3)
    b["diffuse"] = r.floats(3)
    b["specular"] = r.floats(3)
    b["emissive"] = r.floats(3)
    b["glossiness"] = r.f32()
    b["alpha"] = r.f32()


def rd_alpha_property(r, b):
    obj_net(r, b)
    b["flags"] = r.u16()
    b["threshold"] = r.u8()


def rd_flags_property(r, b):
    obj_net(r, b)
    b["flags"] = r.u16()


def rd_vertex_color_property(r, b):
    obj_net(r, b)
    b["flags"] = r.u16()
    b["vertex_mode"] = r.u32()
    b["lighting_mode"] = r.u32()


def rd_stencil_property(r, b):
    obj_net(r, b)
    b["flags"] = r.u16()
    b["enabled"] = r.u8()
    b["function"], b["ref"], b["mask"] = r.u32(), r.u32(), r.u32()
    r.skip(12)                     # fail / z fail / pass actions
    b["draw_mode"] = r.u32()       # 3 = draw both faces


def rd_fog_property(r, b):
    obj_net(r, b)
    b["flags"] = r.u16()
    r.skip(16)


def rd_string_extra(r, b):
    extra_data(r, b)
    b["string"] = r.string()


def rd_text_key_extra(r, b):
    extra_data(r, b)
    b["keys"] = [(r.f32(), r.string()) for _ in range(r.u32())]


def rd_extra(r, b):
    extra_data(r, b)
    r.skip(b["num_bytes"])


def rd_vert_weights_extra(r, b):
    extra_data(r, b)
    r.skip(4 * r.u16())


def rd_data_controller(r, b):      # controllers that are just base + data ref
    time_controller(r, b)
    b["data"] = r.ref()


def rd_uv_controller(r, b):
    time_controller(r, b)
    r.skip(2)
    b["data"] = r.ref()


def rd_geom_morpher_controller(r, b):
    time_controller(r, b)
    b["data"] = r.ref()
    r.skip(1)


def rd_path_controller(r, b):
    time_controller(r, b)
    r.skip(4 + 4 + 4 + 2 + 4 + 4)


def rd_look_at_controller(r, b):
    time_controller(r, b)
    r.skip(4)


def rd_flip_controller(r, b):
    time_controller(r, b)
    r.skip(4 + 4 + 4)              # texture slot, accum time, delta
    b["sources"] = r.refs()


def rd_particle_system_controller(r, b):
    time_controller(r, b)
    v = r.floats(16)   # speed, var, declination, var, planar angle, var, normal[3], color[4], size, emit start/stop
    b["speed"], b["declination"] = v[0], v[2]
    b["initial_color"], b["initial_size"] = v[9:13], v[13]
    r.skip(1)                              # reset particle system
    b["birth_rate"], b["lifetime"] = r.f32(), r.f32()
    r.skip(4 + 1 + 1)                      # lifetime variation, use birth rate, spawn on death
    b["emitter_dims"] = r.floats(3)
    b["emitter"] = r.ref()
    r.skip(2 + 4 + 2 + 4 + 4)             # spawn settings
    num = r.u16()
    r.skip(2 + 40 * num)                   # num valid, particle infos
    r.skip(4)                              # emitter modifier
    b["particle_modifier"] = r.ref()
    b["particle_collider"] = r.ref()
    r.skip(1)


def rd_keyframe_data(r, b):
    """Rotation keys are (time, (w, x, y, z)); XYZ rotations are three float key groups."""
    n = r.u32()
    b["rotations"], b["xyz_rotations"] = [], None
    if n:
        t = r.u32()
        if t == 4:
            r.skip(4)                      # order
            b["xyz_rotations"] = [key_group(r, 1) for _ in range(3)]
        elif t in (1, 2, 5, 3):
            per_key = 8 if t == 3 else 5
            v = r.floats(per_key * n)
            b["rotations"] = [(v[i * per_key], v[i * per_key + 1:i * per_key + 5]) for i in range(n)]
        else:
            raise NifError(f"rotation key type {t}")
    b["translations"] = key_group(r, 3)
    b["scales"] = key_group(r, 1)


def rd_vis_data(r, b):
    r.skip(5 * r.u32())


def rd_uv_data(r, b):
    for _ in range(4):
        key_group(r, 1)


def rd_float_data(r, b):
    key_group(r, 1)


def rd_pos_data(r, b):
    key_group(r, 3)


def rd_color_data(r, b):
    b["keys"] = key_group(r, 4)


def rd_morph_data(r, b):
    """Morph targets: morph 0 is the base shape; with relative targets the rest are offsets.
    Each morph has float weight keys over time: [(time, weight)]."""
    nmorphs, nverts = r.u32(), r.u32()
    b["relative"] = r.u8()
    b["morphs"] = []
    for _ in range(nmorphs):
        nkeys = r.u32()
        interp = r.u32()
        per_key = {1: 2, 2: 4, 3: 5, 5: 2}.get(interp)
        if nkeys and per_key is None:
            raise NifError(f"morph interpolation {interp}")
        v = r.floats((per_key or 0) * nkeys)
        keys = [(v[i * per_key], v[i * per_key + 1]) for i in range(nkeys)] if nkeys else []
        b["morphs"].append({"keys": keys, "vectors": r.floats(3 * nverts)})


def rd_skin_instance(r, b):
    b["data"] = r.ref()
    b["root"] = r.ref()
    b["bones"] = r.refs()


def nif_transform(r):
    rot, trans, scale = r.floats(9), r.floats(3), r.f32()
    return {"rotation": rot, "translation": trans, "scale": scale}


def rd_skin_data(r, b):
    b["transform"] = nif_transform(r)
    nbones = r.u32()
    r.skip(4)                      # skin partition ref
    b["bones"] = []
    for _ in range(nbones):
        bone = {"transform": nif_transform(r)}   # skin space -> bone space
        r.skip(16)                 # bounding sphere
        nw = r.u16()
        w = struct.unpack_from("<" + "Hf" * nw, r.d, r.p)
        r.skip(6 * nw)
        bone["indices"], bone["weights"] = w[0::2], w[1::2]
        b["bones"].append(bone)


def rd_pixel_data(r, b):
    b["format"] = r.u32()
    r.skip(16 + 4 + 8)             # masks, bpp, fast compare
    b["palette"] = r.ref()
    nmip = r.u32()
    b["bytes_per_pixel"] = r.u32()
    b["mipmaps"] = [(r.u32(), r.u32(), r.u32()) for _ in range(nmip)]
    n = r.u32()
    b["pixels"] = r.d[r.p:r.p + n]
    r.skip(n)


def rd_palette(r, b):
    r.skip(1)
    n = r.u32()
    r.skip(4 * (16 if n == 16 else 256))


def dynamic_effect(r, b):
    av_object(r, b)
    r.skip(4 * r.u32())


def rd_light(r, b):
    dynamic_effect(r, b)
    r.skip(4 + 36)                 # dimmer, ambient/diffuse/specular


def rd_point_light(r, b):
    rd_light(r, b)
    r.skip(12)


def rd_spot_light(r, b):
    rd_point_light(r, b)
    r.skip(8)


def rd_texture_effect(r, b):
    dynamic_effect(r, b)
    r.skip(36 + 12 + 16)           # projection matrix/translation, 4 enums
    b["source"] = r.ref()
    r.skip(1 + 16 + 6)             # enable plane, plane, PS2 L/K, unknown


def rd_camera(r, b):
    av_object(r, b)
    r.skip(4 * 11)
    r.skip(4)                      # scene ref
    r.skip(4)                      # num screen polygons


def rd_switch_node(r, b):
    node(r, b)
    b["switch_index"] = r.u32()


def rd_lod_node(r, b):
    rd_switch_node(r, b)
    r.skip(12)
    r.skip(8 * r.u32())


def rd_gravity(r, b):
    particle_modifier(r, b); r.skip(4 + 4 + 4 + 12 + 12)


def rd_grow_fade(r, b):
    particle_modifier(r, b); r.skip(8)


def rd_color_modifier(r, b):
    particle_modifier(r, b)
    b["color_data"] = r.ref()


def rd_particle_rotation(r, b):
    particle_modifier(r, b); r.skip(1 + 12 + 4)


def rd_planar_collider(r, b):
    particle_modifier(r, b); r.skip(4 + 4 * 15)


def rd_spherical_collider(r, b):
    particle_modifier(r, b); r.skip(4 + 4 + 12)


def rd_particle_bomb(r, b):
    particle_modifier(r, b); r.skip(4 * 4 + 4 + 12 + 12)


def rd_obj_net_only(r, b):
    obj_net(r, b)


READERS = {
    "NiNode": rd_node, "RootCollisionNode": rd_node, "NiBSAnimationNode": rd_node,
    "NiBSParticleNode": rd_node, "NiBillboardNode": rd_node, "AvoidNode": rd_node,
    "NiCollisionSwitch": rd_node, "NiSwitchNode": rd_switch_node, "NiLODNode": rd_lod_node,
    "NiTriShape": rd_geometry, "NiTriStrips": rd_geometry, "NiParticles": rd_geometry,
    "NiAutoNormalParticles": rd_geometry, "NiRotatingParticles": rd_geometry,
    "NiTriShapeData": rd_tri_shape_data, "NiTriStripsData": rd_tri_strips_data,
    "NiParticlesData": rd_particles_data, "NiAutoNormalParticlesData": rd_particles_data,
    "NiRotatingParticlesData": rd_rotating_particles_data,
    "NiTexturingProperty": rd_texturing_property, "NiSourceTexture": rd_source_texture,
    "NiMaterialProperty": rd_material_property, "NiAlphaProperty": rd_alpha_property,
    "NiZBufferProperty": rd_flags_property, "NiShadeProperty": rd_flags_property,
    "NiSpecularProperty": rd_flags_property, "NiWireframeProperty": rd_flags_property,
    "NiDitherProperty": rd_flags_property, "NiVertexColorProperty": rd_vertex_color_property,
    "NiStencilProperty": rd_stencil_property, "NiFogProperty": rd_fog_property,
    "NiStringExtraData": rd_string_extra, "NiTextKeyExtraData": rd_text_key_extra,
    "NiExtraData": rd_extra, "NiVertWeightsExtraData": rd_vert_weights_extra,
    "NiKeyframeController": rd_data_controller, "NiVisController": rd_data_controller,
    "NiAlphaController": rd_data_controller, "NiMaterialColorController": rd_data_controller,
    "NiRollController": rd_data_controller, "NiUVController": rd_uv_controller,
    "NiGeomMorpherController": rd_geom_morpher_controller, "NiPathController": rd_path_controller,
    "NiLookAtController": rd_look_at_controller, "NiFlipController": rd_flip_controller,
    "NiParticleSystemController": rd_particle_system_controller,
    "NiBSPArrayController": rd_particle_system_controller,
    "NiKeyframeData": rd_keyframe_data, "NiVisData": rd_vis_data, "NiUVData": rd_uv_data,
    "NiFloatData": rd_float_data, "NiPosData": rd_pos_data, "NiColorData": rd_color_data,
    "NiMorphData": rd_morph_data,
    "NiSkinInstance": rd_skin_instance, "NiSkinData": rd_skin_data,
    "NiPixelData": rd_pixel_data, "NiPalette": rd_palette,
    "NiAmbientLight": rd_light, "NiDirectionalLight": rd_light, "NiPointLight": rd_point_light,
    "NiSpotLight": rd_spot_light, "NiTextureEffect": rd_texture_effect, "NiCamera": rd_camera,
    "NiGravity": rd_gravity, "NiParticleGrowFade": rd_grow_fade,
    "NiParticleColorModifier": rd_color_modifier, "NiParticleRotation": rd_particle_rotation,
    "NiPlanarCollider": rd_planar_collider, "NiSphericalCollider": rd_spherical_collider,
    "NiParticleBomb": rd_particle_bomb, "NiSequenceStreamHelper": rd_obj_net_only,
}


class Nif:
    def __init__(self, data):
        r = Reader(data)
        nl = data.index(b"\n")
        header = data[:nl]
        if not header.startswith(b"NetImmerse File Format"):
            raise NifError(f"bad header {header[:40]!r}")
        r.p = nl + 1
        if r.u32() != 0x04000002:
            raise NifError("not version 4.0.0.2")
        self.blocks = []
        for i in range(r.u32()):
            btype = r.string()
            reader = READERS.get(btype)
            if reader is None:
                raise NifError(f"unknown block {btype} (#{i})")
            b = {"type": btype, "index": i}
            try:
                reader(r, b)
            except (struct.error, IndexError) as e:
                raise NifError(f"{btype} (#{i}) overran: {e}")
            self.blocks.append(b)
        self.roots = r.refs()
        if r.p != len(data):
            raise NifError(f"{len(data) - r.p} trailing bytes")

    def get(self, ref):
        return self.blocks[ref] if 0 <= ref < len(self.blocks) else None

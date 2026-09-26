"""Hand-written names for what tools/recover_types.py can't work out from the code alone: facts already
established in viper-mod-manager/docs/reference (file-formats.md = FF, runtime.md = RT), vrmod's patches,
the hook DLL (hook/viperport.cpp = VP) and the port notes.

NAMES[class][offset] = (name, type or None, note). Types are the ones tools/ApplyTypes.java reads:
float double u1 u2 u4 int uint byte bool ptr ptr:<T> struct:<T> arr:<elem>:<n>.
"""

# structs with no methods of their own (or only trivial ones), laid out from the code that uses them
PLAIN = {
    "P3DBase": (12, [(0, "float", "x"), (4, "float", "y"), (8, "float", "z")]),     # metres; y is up
    "P3D": (12, [(0, "struct:P3DBase", "super_P3DBase")]),
    "Point2D": (8, [(0, "float", "x"), (4, "float", "z")]),                          # ground plane
    # MatrixMakeIdentity writes 1.0 at +0, +0x10, +0x20; MatrixMulPoint reads nine floats, no translation
    "Matrix": (36, [(0, "arr:float:9", "m")]),
    # a pose: Obstacle::Reset copies one as 12 dwords into PhobRoot +0x38; mrModelDraw takes one
    "Frame": (48, [(0, "struct:Matrix", "rot"), (36, "struct:P3DBase", "pos")]),
    # an ideal-line node: the .ili/.ild record copied in whole (FF 4.2, vrmod/ili.py); 512 x 68 pool
    "ILSeg": (68, [(0x00, "ptr:ILSeg", "next"), (0x04, "float", "x"), (0x08, "float", "z"),
                   (0x0c, "float", "dir_x"), (0x10, "float", "dir_z"), (0x14, "float", "corridor_half_width"),
                   (0x18, "float", "target_speed"), (0x1c, "float", "curvature"), (0x20, "float", "step"),
                   (0x24, "float", "cum_distance"), (0x28, "u2", "index"), (0x2a, "byte", "sector"),
                   (0x2b, "byte", "pad_ff"), (0x2c, "float", "field_11"), (0x30, "float", "dist_ahead"),
                   (0x34, "float", "dist_ahead_signed"), (0x38, "float", "cum_time"), (0x3c, "float", "lateral"),
                   (0x40, "uint", "pool_guard")]),
    # the texture cache: 250 x 40 bytes at 0x522fa0 (VP relocate_texture_table)
    "TexEntry": (40, [(0x00, "byte", "in_use"), (0x04, "int", "refcount"), (0x08, "arr:char:16", "name"),
                      (0x1c, "byte", "loaded_to_video"), (0x20, "ptr", "sys_info"), (0x24, "ptr", "vid_info")]),
    # one queued impulse (PhobDyno::QueueExternalImpulse): impulse x weight, where, and who from
    "PhobDyno::ExternalImpulse": (28, [(0, "struct:P3DBase", "impulse"), (12, "struct:P3DBase", "point"),
                             (24, "float", "source")]),
    # the deferred-draw list (VP lift_texture_limit)
    "DeferredEntry": (16, [(0, "ptr", "surf_info"), (4, "ptr", "surface"), (8, "ptr", "model_info"),
                           (12, "ptr:DeferredEntry", "next")]),
}

# classes whose size the code gives wrongly or not at all
SIZES = {"Frame": 48, "ILSeg": 68}

NAMES = {
    "PhobRoot": {   # base of every physics object; PhobStatic adds nothing (both 0x6c, FF 5.2.6)
        0x10: ("tick_count", "u2", "PhobDyno::Update adds 1 each step"),
        0x14: ("volumes", "arr:ptr:CollisionVolume:4", "VP phob_vol: bounds-checked by +0x24"),
        0x24: ("num_volumes", "int", "VP phob_nvol"),
        0x38: ("frame", "struct:Frame", "rotation, then position; y (+0x60) is height"),
    },
    "PhobDyno": {   # every moving body: cars, balls, obstacles, wobbles (from its constructor, reset, Apply*)
        0x06c: ("corners", "arr:struct:Corner:8", "the constructor builds 8; Update runs num_corners of them"),
        0x1ec: ("num_corners", "int", None),
        0x1f0: ("mass", "float", "kg: the record's lb x 0.4545 (FF 'How an obstacle ball rolls')"),
        0x1f4: ("inertia", "struct:P3DBase", "principal moments: the record's x 0.04228"),
        0x200: ("inv_mass", "float", "1 / mass"),
        0x204: ("inv_inertia", "struct:P3DBase", "1 / each moment"),
        0x210: ("inv_inertia_body", "struct:Matrix", "diagonal of inv_inertia"),
        0x234: ("velocity", "struct:P3DBase", "m/s"),
        0x240: ("angular_velocity", "struct:P3DBase", "rad/s"),
        0x24c: ("force", "struct:P3DBase", "accumulated by ApplyForce (Update adds mass x g to y)"),
        0x258: ("torque", "struct:P3DBase", "accumulated by ApplyForce / ApplyTorque"),
        0x264: ("inv_inertia_world", "struct:Matrix", "starts as inv_inertia_body"),
        0x294: ("impulse", "struct:P3DBase", "summed by ApplyImpulse, weighted"),
        0x2a0: ("angular_impulse", "struct:P3DBase", "summed by ApplyImpulse"),
        0x2ac: ("impulse_weight", "float", "sum of ApplyImpulse weights; Update divides by it"),
        0x2b0: ("external_impulses", "arr:struct:PhobDyno::ExternalImpulse:16", "QueueExternalImpulse; 16 at most"),
        0x470: ("num_external_impulses", "int", "the docs' wobble 'contact count' (FF 4.3)"),
        0x474: ("collide_ground", "bool", "Update calls volumes[0]->CollideGround only when set"),
    },
    "Obstacle": {
        0x478: ("spawn_frame", "struct:Frame", "Reset copies it back to frame (rep movsd x 12)"),
        0x4a8: ("last_perturb_time", "float", "physics clock; asleep 1 s after it (FF)"),
        0x4ac: ("awake", "bool", "Perturb sets 1; Update clears it on sleep"),
    },
    "Ball": {
        0x478: ("last_throw_time", "float", "Ball::Throw cooldown (vrmod/hornball.py)"),
    },
    "Wobble": {
        0x4b0: ("settle_timer", "int", "20 steps (FF 5.2.6)"),
    },
    "IdealLine": {
        0x04: ("bead_seg", "ptr:ILSeg", "the car's segment; NULL -> the advance_bead crash (enginefix)"),
        0x08: ("bead_t", "float", "0..1 along bead_seg"),
        0x18: ("car_pos", "struct:Point2D", "update_car_info's first argument"),
        0x20: ("car_dir", "struct:Point2D", "update_car_info's second argument"),
        0x28: ("reset_pending", "bool", "update_car_info calls reset_bead_position when set"),
        0x2c: ("head", "ptr:ILSeg", "the loop's first node"),
        0x30: ("bead_on_line", "bool", "advance_bead: false once the car is too far from the bead"),
    },
    "CollisionVolume": {
        0x04: ("owner", "ptr:PhobRoot", "collide_sphere_sphere reads it as the PhobDyno to push"),
        0x0c: ("default_a", "float", "50000.0 class default (FF 4.8)"),
        0x10: ("default_b", "float", "0.2 class default (FF 4.8)"),
        0x14: ("type_tag", "uint", "FourCC: 'BOX ', 'SPHR', 'TUBE'"),
    },
    "SphereVolume": {
        0x1c: ("radius", "float", "m"),
        0x20: ("local_center", "struct:P3DBase", None),
        0x2c: ("world_center", "struct:P3DBase", "cached each Update"),
    },
    "BoxVolume": {
        0x1c: ("half_extent", "struct:P3DBase", None),
        0x28: ("max_half_extent", "float", None),
        0x2c: ("mode", "int", "0..3, meaning open"),
    },
    "CylinderVolume": {
        0x1c: ("radius", "float", None),
        0x20: ("half_length", "float", "along local z"),
    },
    "TubeVolume": {
        0x24: ("cap_pos", "struct:SphereVolume", "centre (0,0,+h)"),
        0x64: ("cap_neg", "struct:SphereVolume", "centre (0,0,-h)"),
    },
}

# globals the map leaves unnamed (statics), and typed ones the decompiler should see through
GLOBALS = {
    0x520bb4: ("g_phobs", "ptr:ptr:PhobRoot", "every physics object this race; PhysTaskBegin allocates it"),
    0x521090: ("g_num_phobs", "int", None),
    0x5512a0: ("g_wobs", "arr:ptr:WorldObject:1024", "entries from [1]; count at 0x5522d0"),
    0x5522d0: ("g_num_wobs", "int", None),
    0x5522d8: ("g_gobs", "arr:ptr:GraphObject:1024", "entries from [1]; count at 0x553368"),
    0x553368: ("g_num_gobs", "int", None),
    0x522fa0: ("g_textures", "arr:struct:TexEntry:250", "the texture cache"),
    0x522b40: ("g_deferred_buckets", "arr:ptr:DeferredEntry:120", "by texture number + 1; overflows past 118"),
}


def _load_areas():
    """tools/names/*.py: one file per area (car, aicar, wheel...), each with NAMES and optionally PLAIN
    and SIZES in the formats above. Loaded after this file's own, and a later entry for the same
    class and offset replaces an earlier one."""
    import importlib.util
    from pathlib import Path
    for f in sorted((Path(__file__).resolve().parent / "names").glob("*.py")):
        spec = importlib.util.spec_from_file_location("names_" + f.stem, f)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        for c, fields in getattr(mod, "NAMES", {}).items():
            NAMES.setdefault(c, {}).update(fields)
        PLAIN.update(getattr(mod, "PLAIN", {}))
        SIZES.update(getattr(mod, "SIZES", {}))


_load_areas()


def apply(classes):
    for c, size in SIZES.items():
        if c in classes:
            classes[c]["size"], classes[c]["size_from"] = size, "type_names.py"
    for c, fields in NAMES.items():
        if c not in classes:
            continue
        by_off = {f["offset"]: f for f in classes[c]["fields"]}
        for off, (name, typ, note) in fields.items():
            f = by_off.setdefault(off, {"offset": off, "kind": "int32", "size": 4, "uses": 0})
            f["name"], f["from"] = name, "type_names.py" + (f": {note}" if note else "")
            if typ:
                f["type"] = typ
        classes[c]["fields"] = [by_off[o] for o in sorted(by_off)]

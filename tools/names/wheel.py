"""Wheel / Tire / Damper / Corner field names for tools/type_names.py (v1.0 race.exe).

Evidence is from the decompiled methods (Wheel::Setup/Update/update_wheel_position/GetMessage/
UpdateReplay/UpdateHeat/ResetPosition/ctor, Tire::*, setup_tire, TireCreate, Damper::*, Corner::*),
plus the Car code that drives them (Car::Setup, Car::Update, Car::Set*, PlayCar/NetCar ctors,
aicar.obj helpers). CarData offsets were mapped to .cf keys through CarFileCombine
(CarData = lerp of the .cf min/max by the .ccs slider; .cf offsets as in vrmod/cf.py).
Physics step is 0.016 s (62.5 Hz): the damper velocity uses x62.5, the wheel spin integrates x0.016,
UpdateHeat runs every 8 ticks with dt 0.128.

Wheel order (Car +0x554, stride 0x1a4): 0,1 front (SetSteering writes both), 2,3 rear (e-brake
negates their brake input); 0,2 are the -x side. Wheel derives from TorqueInput (the ctor sets
TorqueInput's vtable first), so +0 is the vtable.
"""

PLAIN = {
    # Wheel::GetMessage fills one per wheel; Car::GetMessage steps the pointer by 0x38
    "WheelMessage": (56, [
        (0x00, "struct:P3DBase", "hub_pos"),        # (droop_offset + hub vertex) - car +0x28.., y includes min(compression, travel)
        (0x0c, "float", "hub_height"),              # same y again
        (0x10, "float", "steer_angle"),
        (0x14, "float", "camber"),
        (0x18, "float", "omega"),
        (0x1c, "float", "spin_angle"),
        (0x20, "float", "slide"),
        (0x24, "float", "lat_force_ratio"),
        (0x28, "float", "long_force_ratio"),
        (0x2c, "float", "compression_fraction"),    # min(compression / travel, 1)
        (0x30, "float", "brake_temp"),
        (0x34, "u1", "fx_flags"),
    ]),
    # Wheel::MakeReplayPacket / UpdateReplay: five quantised bytes per wheel (Car steps by 5)
    "Wheel::ReplayPacket": (5, [
        (0, "byte", "spin_angle"), (1, "byte", "omega"), (2, "byte", "compression"),
        (3, "byte", "slide"), (4, "byte", "fx_flags"),
    ]),
    # the .tir resource ('TIRE'), as TireDescGet returns it; TireCreate copies 0x78 bytes.
    # Units are the file's (lb, lb/deg, lb-ft^2, deg); setup_tire converts. 0x00-0x1f unread.
    "TireDesc": (0x78, [
        (0x20, "float", "ref_width_mm"),            # TireCreate: load_factor = car tyre width / this
        (0x24, "float", "aspect_pct"),              # ? TireCreate overwrites 0x24/0x28 in its copy with the car's
        (0x28, "float", "diameter_in"),             # ?
        (0x2c, "float", "mass_lb"),                 # x 0.4545 -> Tire.mass
        (0x30, "float", "inertia_lbft2"),           # x 0.04228 -> Tire.inertia
        (0x34, "float", "pacejka_b"), (0x38, "float", "pacejka_c"),
        (0x3c, "float", "pacejka_d"), (0x40, "float", "pacejka_e"),
        (0x44, "float", "friction_load1_lb"), (0x48, "float", "friction_mu1"),
        (0x4c, "float", "friction_load2_lb"), (0x50, "float", "friction_mu2"),
        (0x54, "float", "cornering_load1_lb"), (0x58, "float", "cornering_stiffness1"),  # lb/deg
        (0x5c, "float", "cornering_load2_lb"), (0x60, "float", "cornering_stiffness2"),
        (0x64, "float", "camber_stiffness_factor"), (0x68, "float", "long_stiffness_factor"),
        (0x6c, "float", "long_friction_factor"),
        (0x70, "float", "camber_peak_deg"), (0x74, "float", "camber_peak_gain"),
    ]),
}

SIZES = {"Wheel": 420, "Tire": 64, "Damper": 16, "Corner": 48, "WheelMessage": 56}

NAMES = {
    "Wheel": {
        # --- geometry and mounting (Wheel::Setup) ---
        0x004: ("droop_offset", "struct:P3DBase",
                "Setup: (+-track/2, -(cm_height + travel), wheelbase x (1-wd) front / -wd rear) m, then minus the hub "
                "vertex; Update adds the vertex back and GetArmPosition -> droop_point_world (bottom of travel)"),
        0x010: ("hub_vertex", "ptr:P3D",
                "Setup's P3D*: the body-mesh vertex nearest the hub (Car::Setup), or a static zero point with damage off"),
        0x014: ("hub_vertex_rest", "struct:P3DBase",
                "copy of *hub_vertex at Setup; Update sets broken if the vertex moves > 5 cm (d^2 > 0.0025), damage on"),
        0x020: ("broken", "bool",
                "damaged() (aicar), Car::AnyWheelsAreDamaged, replay flag 0x40; bump-stop impulse > 2892.5 sets it; "
                "a broken wheel wobbles (sin spin_angle), brakes at 20%, keeps 10% spring"),
        0x024: ("damper", "struct:Damper", "Setup -> Damper::Setup; Update -> GetDampingRate(compression speed)"),
        0x034: ("inertia", "float",
                "kg m^2: Tire.inertia, +10 on a driven axle (front if torque_balance <= 0.9, rear if >= 0.1)"),
        0x038: ("inv_inertia", "float", "1 / inertia; spin integration omega += drive x inv_inertia x 0.016"),
        # --- suspension ---
        0x03c: ("spring_rate", "float", "N/m: (f|r)springs lb/in (CarData 0xcc/0xd0) x 175.197"),
        0x040: ("spring_preload", "float", "? N: Setup writes 0; Update adds it to the spring force"),
        0x044: ("travel", "float",
                "m: ride height + static_deflection; compression clamps at travel + 3.0, bump stop from travel - 0.0254 (RT)"),
        0x048: ("static_deflection", "float", "m: static corner load (mass x axle share x 4.905) / spring_rate"),
        0x04c: ("bump_stop_rate", "float", "N/m: = spring_rate; x (compression - (travel - 1 in))"),
        0x050: ("brake_torque_max", "float",
                "CarData 0x148/0x14c (fbrake/rbrake lerped by brake bias) x 4.45; x |brake_input|, ABS-limited, x0.2 broken"),
        0x054: ("roll_resist_speed", "float",
                "rolling_resistance (cf 0x1d8) / 0.305 x 2.225 x ~0.5 / static load; drag torque ~ coeff x omega x r x load"),
        0x058: ("roll_resist_static", "float", "cf 0x1dc x 1.1125 / static load; x min(|omega| x 0.1, 1) x load"),
        0x05c: ("radius", "float", "m: rim_in x 0.0127 + width_mm x 0.001 x aspect% x 0.01 (CarData 0x130..0x144)"),
        0x060: ("static_camber", "float", "rad: -(f|r)camber deg x side"),
        0x064: ("static_toe", "float", "rad: -(f|r)toe deg x side"),
        0x068: ("steer_quadratic", "float", "? Setup writes 0; steer term is (k x steer_input + 1) x steer_input"),
        0x06c: ("bump_steer", "float", "rad per m of travel: (f|r)bump_toe deg/in x side"),
        0x070: ("bump_camber", "float", "rad per m of travel: (f|r)bump_camber deg/in x side"),
        0x074: ("toe_compliance", "float", "rad/N: cf 0x188/0x18c (deg per 1000 lbf) x lat_force_filtered"),
        0x078: ("camber_compliance", "float", "rad/N: cf 0x190/0x194 (deg per 1000 lbf) x lat_force_filtered"),
        0x07c: ("caster", "float", "rad, front only (rear 0); camber -= caster x sin(steer)"),
        0x080: ("susp_axis", "struct:P3DBase",
                "body-space unit axis (0, -z, y x anti), anti = anti_dive front / anti_squat rear; the tire-force "
                "component along it goes to the spring, not the chassis"),
        # --- tire link ---
        0x08c: ("tire", "ptr:Tire", "TireCreate(<car>.tir or def_tire.tir)"),
        0x090: ("tire_load_factor", "float", "Tire.load_factor; normal load / it -> tire, tire force x it -> N"),
        0x094: ("grip_scale", "float", "CarData 0x150/0x154 (cf 0x1b8/0x1bc); multiplies lateral and long friction"),
        0x098: ("stiffness_scale", "float", "CarData 0x158/0x15c (cf 0x1c0/0x1c4); multiplies cornering and long stiffness"),
        0x09c: ("road_noise_quad", "float", "from GetLoadFluctuation; random +-(v^2 x this + |v| x lin) x spring force"),
        0x0a0: ("road_noise_lin", "float", "see road_noise_quad; surface 10 (bumpy) x9, other off-road x3, road 0"),
        # --- per-tick state ---
        0x0a4: ("aligning_torque", "float", "Car::Update sums the front pair into Car +0xeb0 (steering feedback)"),
        0x0a8: ("surface_speed", "float", "m/s: omega x radius (ABS/TC only act above 1 m/s)"),
        0x0ac: ("spin_angle", "float", "rad, wraps at +-2pi; += omega x 0.008; replay byte 0"),
        0x0b0: ("heading", "struct:P3DBase", "body-space rolling direction (sin steer, 0, cos steer)"),
        0x0bc: ("replay_velocity", "struct:P3DBase", "? only UpdateReplay writes it (the car's velocity)"),
        0x0c8: ("droop_point_world", "struct:P3DBase",
                "world point TerrainGetHeight is asked at; suspension/tire forces are applied here"),
        0x0d4: ("steer_angle", "float", "rad: toe + bump steer + compliance + steer_input (update_wheel_position)"),
        0x0d8: ("camber", "float", "rad: (bump camber + compliance + static) x cos(steer) - caster x sin(steer)"),
        0x0dc: ("steer_input", "float", "rad: Car::SetSteering, wheel_lock deg x 0.0174 x input (front wheels)"),
        0x0e0: ("brake_input", "float", "0..1 from Car::Update; negative = handbrake on the rears (no ABS)"),
        0x0e4: ("drive_torque", "float", "N m: Wheel::ApplyTorque (TorqueInput vtable slot, from the differential)"),
        0x0e8: ("compression", "float", "m: ground distance into the travel, <= travel + 3; ResetPosition zeroes"),
        0x0ec: ("lat_force_ratio", "float", "lateral force / (mu x load); 0 in the air"),
        0x0f0: ("long_force_ratio", "float", "longitudinal force / (mu x load)"),
        0x0f4: ("tire_force", "struct:P3DBase", "N, world; magnitude capped at 10,000"),
        0x100: ("chassis_tire_force", "struct:P3DBase",
                "tire_force minus its susp_axis_world part; PhobDyno::ApplyForce, and next tick's normal load"),
        0x10c: ("lat_force_filtered", "float", "lateral tire force low-passed (0.9 old + 0.1 new); feeds compliance"),
        0x110: ("susp_axis_world", "struct:P3DBase", "rot x susp_axis (update_wheel_position)"),
        0x11c: ("slide", "float",
                "max(0, combined normalised slip x 2/3 - 1); > 1.45 = skidding (Car::WheelsSkidding); squeal; replay 0..10"),
        0x120: ("fx_flags", "u1",
                "1/2 = light/heavy squeal on pavement, 0x20 = off-road, 4 = off-road spray, 0x40 = broken; replay byte 4"),
        0x121: ("surface_class", "u1", "0 on pavement or airborne, 5 off-road; RealTireSound squeals only at 0"),
        0x124: ("omega", "float", "rad/s wheel spin; replay byte 1 (clamped +-100)"),
        0x128: ("rpm", "float", "omega x 9.5493 (Wheel::GetRPM)"),
        0x12c: ("brake_power", "float", "W: applied brake torque x |omega|; UpdateHeat's heat input"),
        0x130: ("contact_point", "struct:P3DBase", "world: TerrainGetHeight's hit point"),
        0x13c: ("prev_omega", "float", "last tick's omega; gyroscopic reaction torque"),
        0x140: ("prev_axle", "struct:P3DBase", "last tick's world axle (up x heading); gyro torque x inertia x 12.5"),
        0x158: ("anti_roll_force", "float",
                "N: Car::Update, (compression - partner's compression) x sway rate; added to the spring force"),
        0x15c: ("traction_control", "int",
                "enum TractionControl (Car::SetTractionControl): 1 long slip, 2 long + 5 x lat; target 0.35"),
        0x160: ("digital_throttle", "bool",
                "PlayCar ctor sets all 4 when DriverIsThrottleDigital(); with TC off, limits slip at 2.0"),
        0x164: ("abs_mode", "int", "enum ABSBraking (Car::SetABSBraking); long-slip target 1.333 if 1 else 1.111"),
        0x168: ("on_ground", "bool", "off_ground() (aicar); set when the ground reaches into the travel (compression >= 0)"),
        0x170: ("lat_slip", "float", "normalised lateral slip, filtered (TC mode 2 input)"),
        0x174: ("long_slip", "float", "normalised longitudinal slip, filtered (ABS / TC input)"),
        0x178: ("susp_force", "float",
                "N: spring + damper + bump stop + anti-roll + noise, capped 17,800 (RT); rolling-resistance load"),
        0x17c: ("drag_torque", "float", "(rolling resistance + brake + previous) x 0.5; opposes omega"),
        0x180: ("long_slip_raw", "float", "normalised longitudinal slip before filtering"),
        0x184: ("tire_sound", "ptr:TireSound", "TireSound::Create; UpdateCommon calls its Update"),
        0x188: ("brake_cooling", "float", "W/K: 80.4 x 1.667; x (ambient - brake_temp)"),
        0x18c: ("brake_heat_capacity", "float", "J/K: 900 x 2"),
        0x190: ("inv_brake_heat_capacity", "float", "1 / brake_heat_capacity"),
        0x194: ("brake_temp", "float", "K: 290 at start; ambient = PhysicsGetTemperature(); WheelMessage +0x30"),
        0x198: ("heat_accum", "float", "J: UpdateHeat's accumulator, emptied into brake_temp each call"),
        0x19c: ("surface", "int",
                "TerrainGetHeight's surface code: 0 road, 10 bumpy, 14 water (wheel ignores it), 11/13/15 grip 0.8 "
                "+ drag 2.5; HackPaveTheWorld zeroes it; on_water / off_road / wheels_on_bumpy read it"),
        0x1a0: ("is_remote", "bool", "NetCar ctor sets all 4; a remote car's bumpy (10) surface is not x9 road noise"),
    },
    "Tire": {
        0x00: ("cornering_quad", "float",
               "cornering stiffness N/rad = quad x Fz^2 + lin x Fz (Fz N), through two .tir points (SetCorneringStiffness)"),
        0x04: ("cornering_lin", "float", "see cornering_quad"),
        0x08: ("friction_slope", "float", "lateral mu = slope x Fz + base (SetLateralFriction, two .tir points)"),
        0x0c: ("friction_base", "float", "see friction_slope"),
        0x10: ("camber_stiffness_factor", "float", "camber stiffness = cornering stiffness x this"),
        0x14: ("long_stiffness_factor", "float", "longitudinal stiffness = cornering stiffness x this"),
        0x18: ("long_friction_factor", "float", "longitudinal mu = lateral mu x this"),
        0x1c: ("pacejka_b", "float", "GetResultant = D sin(C atan(B s - E (B s - atan(B s)))) on normalised slip s"),
        0x20: ("pacejka_c", "float", "SetMagicNumbers arg 2"),
        0x24: ("pacejka_d", "float", "SetMagicNumbers arg 3 (peak)"),
        0x28: ("pacejka_e", "float", "SetMagicNumbers arg 4 (curvature)"),
        0x2c: ("load_factor", "float", "car tyre width / .tir reference width (TireCreate); SetMagicNumbers resets to 1"),
        0x30: ("mass", "float", "kg: .tir lb x 0.4545; Wheel::Setup adds 9.09 kg for the unsprung mass"),
        0x34: ("inertia", "float", "kg m^2: .tir x 0.04228; copied to Wheel.inertia"),
        0x38: ("camber_peak", "float", "sin(peak camber angle); GetCamberGripFactor clamps camber to +-this"),
        0x3c: ("camber_peak_gain", "float", "grip factor = 1 + gain x clamp(camber) / camber_peak"),
    },
    "Damper": {   # rate(v) in N s/m; force = rate x v; v > 0 is compression (bump); rate clamped >= 0
        0x0: ("bump_slope", "float",
              "bump rate = slope x v + base, through (4 in/s, bump x 175.2) and (30 in/s, bump x 105.1): falls with speed"),
        0x4: ("bump_base", "float", "bump rate at v = 0"),
        0x8: ("rebound_slope", "float", "rebound rate = base - slope x v (v <= 0); 0 as Wheel sets it up"),
        0xc: ("rebound_base", "float", "N s/m: (f|r)rebound lb s/in x 175.2"),
    },
    "Corner": {   # a ground-contact spring point on a PhobDyno (obstacles; Obstacle ctor builds them)
        0x00: ("local_pos", "struct:P3DBase", "body-space contact point (Corner::Setup arg 1)"),
        0x0c: ("ray_origin_local", "struct:P3DBase", "Setup arg 3, (0,0,0) = body origin at every call site"),
        0x18: ("stiffness", "float", "N/m (ctor 1000); x0.04 on water (14); Setup arg 2"),
        0x1c: ("prev_depth", "float",
               "signed penetration last tick: full spring pushing in, 1% coming out (lossy)"),
        0x20: ("ray_origin_world", "struct:P3DBase",
               "last tick's world ray_origin_local; the swept test runs from 10 m above it to local_pos"),
        0x2c: ("first_update", "bool", "ctor sets 1; first Update snaps ray_origin_world to the body position"),
    },
}

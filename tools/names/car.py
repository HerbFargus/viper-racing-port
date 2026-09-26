"""Car + drivetrain field names for tools/type_names.py, from the v1.0 race.exe decompilation (agent copy of the
Ghidra project, out/agents/car). Offsets are from the start of each class, base part included.

Units the code settles: physics step dt = 0.016 s (62.5 Hz); shaft speeds are rpm (x 0.10472 -> rad/s);
temperatures are kelvin (start 290 K; the HUD converts to F); CarData engine/aero inputs arrive in imperial
(hp, ft-lb, lb-ft^2, inches, ft^2) and Car::Setup converts them (1.35152 ft-lb -> N m, 0.0254 in -> m,
0.04228 lb ft^2 -> kg m^2, 0.093 ft^2 -> m^2, 0.645 = 0.5 x 1.29 kg/m^3 air, 175.2 lb/in -> N/m).
"""

# structs with no methods of their own
PLAIN = {
    # a lumped heat store; Engine has two (block, coolant), Wheel has one at +0x188 (tyre): Engine::UpdateHeat /
    # Wheel::UpdateHeat do heat += (ambient - temperature) * conductance * dt; temperature += heat * inv_capacity;
    # heat = 0. HeatTube::Update moves heat between two of them.
    "ThermalMass": (20, [(0x00, "float", "conductance"), (0x04, "float", "heat_capacity"),
                         (0x08, "float", "inv_heat_capacity"), (0x0c, "float", "temperature"),
                         (0x10, "float", "heat")]),
}

SIZES = {"PowerCurve": 24}

NAMES = {
    "Shaft": {      # base of Engine/Transmission/Propeller; vtable (TorqueInput: GetRPM, ApplyTorque) at +0
        0x04: ("owner", "ptr:PhobDyno", "Setup arg 1; ApplyTorque feeds the reaction torque into it (about frame row 6..8)"),
        0x08: ("inertia", "float", "kg m^2; Setup arg 2 (engine: CarData lb ft^2 x 0.04228; gearbox: 0.2114 const)"),
        0x0c: ("inv_inertia", "float", "1 / inertia (Setup)"),
        0x10: ("rpm", "float", "rpm; ApplyTorque: rpm += (T - drag*rpm) * inv_inertia / 0.10472 * 0.016"),
        0x14: ("drag", "float", "N m per rpm; GetDragTorque = drag * rpm; Setup arg 3"),
    },
    "Engine": {     # Engine::Engine, Setup, ApplyTorque, get_engine_torque, UpdateHeat
        0x18: ("block", "struct:ThermalMass", "ctor: 237 x 0.625 W/K to air, 900 J/kgK x 80 kg, starts 290 K; "
                                              "ApplyTorque adds (0.1 x torque + drag torque) x rad/s x dt as heat"),
        0x2c: ("throttle", "float", "SetThrottle; 0..1"),
        0x30: ("idle_throttle", "float", "Setup: the throttle that makes curve torque equal drag at idle_rpm; "
                                         "ApplyTorque uses idle_throttle + (1 - idle_throttle) * throttle"),
        0x34: ("idle_rpm", "float", "Setup arg 6 (CarData+0x78); Transmission anti-stall and auto-shift read it"),
        0x38: ("smoothed_throttle", "float", "ApplyTorque: 0.3 x throttle + 0.7 x old; GetPerceivedThrottle, "
                                             "PlayCar brake-light flags, SetGearAuto hysteresis"),
        0x3c: ("redline", "float", "rpm, ctor 6000, Setup arg 5 (CarData+0x7c); get_engine_torque returns 0 at or above "
                                   "it (rev limiter); torque scale = min(1, redline / max(1000, rpm) x throttle)"),
        0x40: ("cranking", "bool", "Crank sets it when stalled; get_engine_torque gives 80 N m starter torque"),
        0x41: ("stalled", "bool", "ApplyTorque: set when rpm <= 400, cleared (with cranking) above; "
                                  "get_engine_torque gives no combustion torque while set"),
        0x44: ("curve", "struct:PowerCurve", "copied (6 dwords) from Setup arg 4"),
        0x5c: ("coolant", "struct:ThermalMass", "ctor: radiator 80.4 x 20 W/K, 4180 J/kgK x 12.8 kg (water), 290 K; "
                                                "the HUD's 'R:' temperature (GetMessage +0x184)"),
        0x70: ("thermostat", "struct:HeatTube", "block <-> coolant; ApplyTorque sets conductance = "
                                                "clamp((block.temperature - 365.33 K) x 0.18, 0, 1) x 10000 W/K, runs it at dt 0.016"),
    },
    "PowerCurve": {  # Setup(power_hp, power_rpm, torque_max ft-lb, torque_rpm, drag) from CarData+0x6c/74/68/70; read by get_engine_torque
        0x00: ("a", "float", "rpm^2 coefficient: (T@power - T_max) / (power_rpm - torque_rpm)^2, N m"),
        0x04: ("b", "float", "rpm coefficient: -2 a torque_rpm + drag (adds back shaft friction)"),
        0x08: ("c", "float", "constant: a torque_rpm^2 + T_max; T(r) = a r^2 + b r + c - max(0, r - falloff_rpm) x falloff_slope"),
        0x0c: ("drag", "float", "Setup arg 5 = engine drag (N m / rpm) folded into b"),
        0x10: ("falloff_rpm", "float", "the power-peak rpm (Setup arg 2, CarData+0x74)"),
        0x14: ("falloff_slope", "float", "N m lost per rpm past falloff_rpm: T@power x 0.000125"),
    },
    "Clutch": {     # Clutch::Setup/Connect/Update/SetClutch; not a Shaft, no vtable
        0x00: ("engagement", "float", "0..1, SetClutch (driven by Transmission::Update's clutch_applied); 1 = locked"),
        0x04: ("input", "ptr:TorqueInput", "Connect arg 1: the Engine"),
        0x08: ("output", "ptr:TorqueInput", "Connect arg 2: the Transmission (a plane's Propeller)"),
        0x0c: ("owner", "ptr:PhobDyno", "Setup arg 1"),
        0x10: ("max_torque", "float", "Setup arg 2 = CarData torque_max x 4 (the ft-lb number, unconverted)"),
        0x14: ("stiffness", "float", "max_torque x 0.0025: torque per rpm of slip"),
        0x18: ("torque", "float", "Update: slip x stiffness clamped to +-max_torque x engagement, then 0.15 new + 0.85 old; "
                                  "applied as engagement^2 x torque"),
    },
    "Transmission": {   # Shaft base; Setup/Update/SetGearAuto/GetRPM/ApplyTorque
        0x18: ("ratio", "float", "current overall ratio: gear_ratios[engaged_gear-1], -gear_ratios[0] in reverse, 0 in neutral"),
        0x1c: ("engaged_gear", "int", "the gear actually in mesh (-1 R, 0 N, 1..); lags gear during a shift"),
        0x20: ("num_gears", "int", "Setup arg 4: count of CarData ratios > 0.01, at most 7"),
        0x24: ("gear", "int", "requested gear, SetGear"),
        0x28: ("input_rpm", "float", "ApplyTorque: output rpm x ratio (or own shaft rpm in neutral); SetGearAuto reads it"),
        0x2c: ("gear_ratios", "arr:float:8", "Setup copies num_gears (<= 7) from CarData+0x94; index gear-1; slot 8 never read"),
        0x4c: ("output", "ptr:TorqueInput", "Connect: front, rear or centre Differential by torque_balance"),
        0x50: ("engine", "ptr:Engine", "Setup arg 6"),
        0x54: ("clutch_unit", "ptr:Clutch", "Setup arg 7; Update calls Clutch::SetClutch(clutch_applied)"),
        0x58: ("in_gear", "bool", "Update: engaged_gear != 0; GetRPM/ApplyTorque couple to output only when set"),
        0x5c: ("clutch", "float", "driver clutch (1 = engaged), SetClutch"),
        0x60: ("clutch_applied", "float", "what Update hands the Clutch: the driver's value, or the auto-clutch ramp "
                                          "(0.128 per step) and anti-stall cap (rpm - idle) / 1000"),
        0x64: ("throttle", "float", "driver throttle, SetThrottle; Update passes it (or 0 / 1 during a shift) to the Engine"),
        0x68: ("shift_state", "int", "auto-clutch state: 0 settled, 1 clutch out, 2 upshift in neutral, 3 downshift "
                                     "rev-match blip (throttle 1), 4 clutch back in (ctor value), 5 never set here"),
    },
    "Differential": {   # vtable (TorqueInput) at +0
        0x04: ("input_a", "ptr:TorqueInput", "Connect arg 1 (left wheel / front diff)"),
        0x08: ("input_b", "ptr:TorqueInput", "Connect arg 2 (right wheel / rear diff)"),
        0x0c: ("rpm_a", "float", "ApplyTorque caches input_a->GetRPM()"),
        0x10: ("rpm_b", "float", "ApplyTorque caches input_b->GetRPM()"),
        0x14: ("owner", "ptr:PhobDyno", "Setup arg 1"),
        0x18: ("lock_stiffness", "float", "Setup arg 3 = CarData diff_stiff x 1.357: N m per rpm of a/b difference "
                                          "(limited slip)"),
        0x1c: ("ratio", "float", "Setup arg 2: final drive (CarData+0x90) for axles, 1.0 for the centre; "
                                 "GetRPM = (rpm_a + rpm_b) x ratio / 2; each side gets torque x ratio / 2"),
    },
    "HeatTube": {   # vtable (Update(dt)) at +0
        0x04: ("conductance", "float", "W/K; Engine::ApplyTorque sets it (thermostat)"),
        0x08: ("a", "ptr:ThermalMass", "Update: a.heat += (b.temperature - a.temperature) x conductance x dt (Engine: &block)"),
        0x0c: ("b", "ptr:ThermalMass", "and b.heat -= the same (Engine: &coolant)"),
    },
    "Car": {
        0x478: ("body_volume", "ptr:SphereGroupVolume", "Setup: 13 spheres fitted to the body model, appended to volumes[]; "
                                                        "reset/teleport call its vtable+0x14, reset_damage +0x4"),
        0x47c: ("cockpit_eye", "struct:P3DBase", "Setup: <name>cockpit.tab row 0 fields 1..3, else (-0.3, 0.8, 0.0)"),
        0x488: ("last_damage_time", "float", "ResolveExternalImpulse: PhysicsGetTime(); with damage off, Update "
                                             "calls reset_damage 2 s after it"),
        0x48c: ("damaged", "bool", "set by ActuallyApplyDamage/ResolveExternalImpulse, cleared by reset_damage; "
                                   "replay flag 0x20"),
        0x48e: ("damage_grid", "arr:u2:16", "16x16 bit grid over the body texture's UV: apply_rect_damage ORs "
                                            "column bits into rows; copied to the net message +0x162"),
        0x4b0: ("lod_models", "arr:int:5", "model handles from PhobData+0x1b8, pristine; reset_damage copies their verts back"),
        0x4c4: ("live_models", "arr:int:5", "PhobData+0x1cc: the dentable copies; ActuallyApplyDamage moves [0]'s verts"),
        0x4d8: ("lod_vertex_maps", "arr:ptr:5", "ctor: u2[nverts] per LOD, each vertex's nearest vertex in LOD 0, "
                                                "so dents in live_models[0] propagate; freed in ~Car"),
        0x4ec: ("look_back", "bool", "PlayCar::Update: DriverGetLookBack(); net message +0x160"),
        0x4f0: ("look_side", "float", "PlayCar::Update: DriverGetLookSide(); net message +0x15c"),
        0x4f4: ("realism", "int", "PlayCar: PhysicsGetRealism(); AICar 2; Update adds a yaw-stabilising "
                                  "force at 70 m ahead only in modes 0 and 1; Wheel::Update reads it too"),
        0x4f8: ("yaw_control", "int", "option 'yaw_control' / AICar::set_yaw_control; apply_yaw_control brakes single "
                                      "wheels toward the bicycle-model yaw rate, gain 0.5 if 1 else 5"),
        0x4fc: ("race_state", "int", "0 held on the grid (Update: clutch 0, brakes 1, no xz velocity; "
                                     "RaceDeity::Reset), 2 racing (ctor; CanTeleport needs > 1), 3 finished "
                                     "(RaceDeity::UpdateCar after the last lap; PlayCar brakes)"),
        0x500: ("steering", "float", "-1..1, SetSteering (rate-limited to 0.08 per step); x max_steer_angle -> "
                                     "front wheels' +0xdc (a plane steers the rear pair, negated)"),
        0x508: ("pitch", "float", "SetPitch (plane only: PlayCar DriverGetPitch); elevator angle = pitch x -7 deg"),
        0x50c: ("opacity", "float", "1 normally; 0 on Teleport then post_teleport_fadein ramps it; IsSolid = "
                                    "opacity > 0.99 (a fading car can't be hit or drafted); replay byte 0x3a"),
        0x510: ("car_index", "int", "PhobData+0x194; PhysTaskRegisterCar/FindCar, sound owner, replay/net id"),
        0x514: ("name", "arr:char:32", "Setup: CarData+0x198; '<name>cockpit.tab', '<name>.sfx' horn"),
        0x534: ("auto_clutch", "bool", "option 'auto_clutch' (forced on by auto_shifting); AICar 1; "
                                       "Update passes it to Transmission::Update"),
        0x538: ("drag_long", "float", "0.5 rho x frontal_area x drag_coefficient; Update's body-drag force along local z"),
        0x53c: ("drag_lat", "float", "same with CarData lateral drag (local x)"),
        0x540: ("drag_vert", "float", "same x 0.5 with CarData vertical drag (local y)"),
        0x544: ("front_lift", "float", "x forward speed^2 along local up, applied at front_axle (negative = downforce)"),
        0x548: ("rear_lift", "float", "same at rear_axle"),
        0x54c: ("front_spoiler_drag", "float", "x |v| x v, opposed, at front_axle"),
        0x550: ("rear_spoiler_drag", "float", "same at rear_axle"),
        0x554: ("wheels", "arr:struct:Wheel:4", "ctor/dtor loop 4 x 0x1a4 to +0xbe4; 0,1 front (front_diff), "
                                                "2,3 rear (rear_diff); 0 and 2 on the -x side"),
        0xbe4: ("is_plane", "bool", "Setup: CarData+0x54 negative; owns propeller + 4 wings, rear-wheel steering, "
                                    "clutch forced in above 2000 rpm; ~Car frees the parts"),
        0xbe8: ("propeller", "ptr:Propeller", "plane only; Clutch output instead of the gearbox"),
        0xbec: ("wing_left", "ptr:Wing", "plane only; angle 2 deg + steering x 5 deg (aileron)"),
        0xbf0: ("wing_right", "ptr:Wing", "plane only; angle 2 deg - steering x 5 deg"),
        0xbf4: ("elevator", "ptr:Wing", "plane only; angle = pitch x -7 deg"),
        0xbf8: ("rudder", "ptr:Wing", "plane only; WingAxis 1, angle 0"),
        0xbfc: ("engine", "struct:Engine", None),
        0xc7c: ("clutch_unit", "struct:Clutch", "Connect(engine, transmission)"),
        0xc98: ("transmission", "struct:Transmission", None),
        0xd04: ("center_diff", "struct:Differential", "ratio 1, joins front_diff and rear_diff; used when "
                                                      "0.01 < torque_balance < 0.99"),
        0xd24: ("front_diff", "struct:Differential", "wheels 0,1"),
        0xd44: ("rear_diff", "struct:Differential", "wheels 2,3"),
        0xd64: ("gear", "int", "transmission.gear after SetGear/SetGearAuto (requested)"),
        0xd68: ("engaged_gear", "int", "transmission.engaged_gear; net message +0x48, replay byte 0x23"),
        0xd6c: ("shift_sound_gear", "int", "UpdateCommon plays shift1.sfx when engaged_gear differs"),
        0xd70: ("perceived_rpm", "float", "Update: engine rpm; replay: 16-bit, -2000..12000"),
        0xd74: ("digital_throttle", "bool", "PlayCar: DriverIsThrottleDigital()"),
        0xd78: ("heat_step", "int", "starts at car_index to stagger cars; UpdateHeat(0.128 s) every 8th step"),
        0xd7c: ("front_axle", "struct:P3DBase", "body frame: (0, -cm_height, (1 - weight_front) x wheelbase); front "
                                                "aero forces, wheel setup, RaceDeity's lap-line point"),
        0xd88: ("rear_axle", "struct:P3DBase", "(0, -cm_height, -weight_front x wheelbase); rear aero forces, draft_point"),
        0xd94: ("center_of_pressure", "struct:P3DBase", "front_axle + CarData cp height/long (in); body drag applied here"),
        0xdc4: ("torque_balance", "float", "CarData+0xb4 rear share: <= 0.01 FWD, >= 0.99 RWD, else centre diff"),
        0xdcc: ("max_steer_angle", "float", "rad: CarData wheel_lock (deg) x 0.0174"),
        0xdd0: ("rpm_to_speed", "float", "driven wheel radius (torque_balance-weighted) / final drive x 0.10472"),
        0xdd4: ("front_anti_roll", "float", "N/m (CarData lb/in x 175.2): (wheels[0].+0xe8 - wheels[1].+0xe8) x it "
                                            "-> each wheel's +0x158, only with a front wheel down"),
        0xdd8: ("rear_anti_roll", "float", "same for wheels 2,3"),
        0xddc: ("speed", "float", "m/s: transmission output rpm x rpm_to_speed (replay: |velocity|); "
                                  "HUD/telemetry speed, net message +0x4c"),
        0xde0: ("throttle", "float", "SetThrottle"),
        0xde4: ("braking", "float", "SetBraking; front wheels' +0xe0"),
        0xde8: ("ebrake", "float", "SetEBrake; rear wheels get max(braking, ebrake), negated when ebrake > 0.1"),
        0xdec: ("clutch", "float", "SetClutch, 1 = engaged (reset value)"),
        0xdf0: ("air_velocity", "struct:P3DBase", "body frame: velocity - draft_velocity (Update); drives body drag "
                                                  "when airborne and road-noise volume"),
        0xdfc: ("draft_velocity", "struct:P3DBase", "world: UpdateDraft sums leaders' velocity x (r^2 - d^2) / r^2; "
                                                    "cleared after each Update"),
        0xe08: ("draft_point", "struct:P3DBase", "world: rear_axle + forward x (-0.5 x draft_radius); others test "
                                                 "their distance to it"),
        0xe14: ("draft_radius", "float", "m, 5.0 on reset"),
        0xe18: ("fuel", "float", "?litres: fuel_capacity x 3.79 on reset; net message +0x50; nothing found burning it"),
        0xe1c: ("fuel_burn_rate", "float", "?CarData fuel consumption x 0.00105 on reset; no reader found"),
        0xe28: ("horn", "bool", "SetHorn (horn-ball hack throws on the rising edge); replay flag; horn.sfx volume"),
        0xe30: ("lap_top_speed", "float", "m/s: Update keeps the max while all 4 wheels are down and none skid; "
                                          "RaceDeity::UpdateCar records and zeroes it per lap"),
        0xe3c: ("reset_event", "bool", "set by reset/Teleport, sent once as replay flag 2 then cleared"),
        0xe3d: ("lowering", "bool", "set by reset/Teleport: Update damps vertical motion until 3+ wheels touch, "
                                    "then DoneLowering clears it; blocks HelpOut/CanTeleport"),
        0xe40: ("scrape_energy", "float", "ApplyExternalForce sums |F|^2 x a speed ramp; UpdateCommon turns it into "
                                          "scrape.sfx volume and zeroes it"),
        0xe44: ("teleport_time", "float", "Teleport: PhysicsGetTime(); -100 on reset; post_teleport_fadein holds "
                                          "it back while another car sits within ~5 m"),
        0xe49: ("starter_on", "bool", "Update: engine.cranking; plays start.sfx"),
        0xe4c: ("start_sound", "ptr:Sound3D", "start.sfx"),
        0xe50: ("engine_sound", "ptr:EngineSound", None),
        0xe54: ("shift_sound", "ptr:SoundDash", "shift1.sfx"),
        0xe58: ("road_sound", "ptr:Sound3D", "road1.sfx, pitched by airspeed"),
        0xe5c: ("road_sound_2", "ptr:Sound3D", "road2.sfx, when 2+ wheels have flag 0x20 at +0x120"),
        0xe60: ("scrape_sound", "ptr:Sound3D", "scrape.sfx"),
        0xe64: ("horn_sound", "ptr:Sound3D", "'<name>.sfx' else horn.sfx"),
        0xe68: ("horn_volume", "float", "UpdateCommon ramps it up 0.512 / down 0.128 per update"),
        0xe6c: ("start_frame", "struct:Frame", "Setup: CarData+0x18; reset copies it to frame"),
        0xe9c: ("fuel_capacity", "float", "?CarData+0x8c (gallons, x 3.79 -> fuel)"),
        0xea0: ("fuel_consumption", "float", "?CarData+0x88"),
        0xea4: ("lat_g", "float", "Update: acceleration (PhobDyno +0x288) on the horizontal right axis / 9.81, "
                                  "low-passed 0.9; HUD LAT, telemetry +0xc"),
        0xea8: ("long_g", "float", "same on the horizontal forward axis; HUD LONG, force feedback"),
        0xeac: ("vert_g", "float", "same on the body up axis; force feedback"),
        0xeb0: ("steer_feedback", "float", "?wheels[0].+0xa4 + wheels[1].+0xa4 each Update; DriverSetForce's first "
                                           "argument (steering force feedback); AICar::crash_aftershock reads it"),
        0xeb4: ("aero_g", "float", "sum of the two lift forces' y / mass / -9.81; the HUD's AERO G"),
    },
}

# Not in NAMES (Wheel is someone else's class): offsets inside one Wheel that the Car code above pins down.
WHEEL_HINTS = {
    0x020: ("damaged", "bool", "reset_damage clears; AnyWheelsAreDamaged; blocks the realism assist; net flag 0x40"),
    0x05c: ("radius", "float", "?Car::Setup builds rpm_to_speed from it (m)"),
    0x0a4: ("?", "float", "front pair summed into Car.steer_feedback (self-aligning force?)"),
    0x0dc: ("steer_angle", "float", "rad; SetSteering writes the front pair (plane: rear pair, negated)"),
    0x0e0: ("brake", "float", "Car::Update writes all four (front braking, rear max(braking, ebrake))"),
    0x0e8: ("?compression", "float", "anti-roll input: (left - right) x anti_roll stiffness"),
    0x11c: ("slip", "float", "?WheelsSkidding: > 1.45 skids; > 0.1 scales the realism assist"),
    0x120: ("flags", "byte", "bit 0x20 counted by UpdateCommon for road2.sfx"),
    0x158: ("anti_roll_force", "float", "Car::Update writes +-(axle difference x stiffness)"),
    0x15c: ("traction_control", "int", "SetTractionControl"),
    0x160: ("digital_throttle", "bool", "PlayCar sets when the throttle is digital"),
    0x164: ("abs_braking", "int", "SetABSBraking"),
    0x168: ("on_ground", "bool", "Update counts these (lowering, assists, aero model, anti-roll gating)"),
    0x188: ("tyre", "struct:ThermalMass", "Wheel::UpdateHeat: same update as the engine's, plus +0x12c as heat input"),
}

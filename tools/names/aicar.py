"""AICar (and LocalCar, the AI's helper structs, the skill parameters AICar reads) named from the v1.0
race.exe decompilation. Offsets are from the start of the object, base part included.

Units: time is PhysicsGetTime() seconds. The physics step is 0.016 s (62.5 Hz: PhobDyno and Wheel,
and measured in game); the AI's own rates suggest AICar::Update runs at half that, 31.25 Hz
(AISetSteering's 31.25; the 64-tick stranded check ~2 s) -- unconfirmed. Speeds are m/s and the code's thresholds are round mph (0.4444 = 1 mph,
8.889 = 20 mph, 22.22 = 50 mph, 26.67 = 60 mph). "centre line" = the per-car CenterLine (track.ild) at
RaceDeity::global +0xa4 + car*0x74; "the line" = the car's racing line (IdealLine, .ili) at +0xfe8.

Three names already in type_names.py were guesses from headon_panic and are REPLACED here:
+0xf0c is the clock (not a panic value), +0xf18 is the car's lateral offset from the centre line
(its sign is what headon_panic steers by), +0xfd0 is the freeze start time.
"""

PLAIN = {
    # one per racing-line segment, allocated num_segs x 24 in the ctor; GetRelSegment indexes it
    "AICar::SegmentInfo::Notes": (8, [(0, "byte", "hit_wall"), (1, "byte", "off_road_a"),
                                      (2, "byte", "off_road_b"), (4, "float", "speed_note")]),
    "AICar::SegmentInfo": (24, [(0x00, "struct:AICar::SegmentInfo::Notes", "notes"),
                                (0x08, "float", "lateral_note"), (0x0c, "byte", "locked"),
                                (0x10, "float", "lookahead"), (0x14, "ptr:ILSeg", "seg")]),
    # one per AI car (AICarCount), 8-byte stride in the ctor; only +0 is ever touched
    "AICar::ProxerInfo": (8, [(0, "float", "level")]),
    # 16 x 20 bytes at 0x4ec6c0 (AIGetTrackInfo masks the track number with 0xf)
    "AITrackInfo": (20, [(0x00, "float", "launch_time_base"), (0x04, "float", "launch_time_per_car"),
                         (0x08, "byte", "tc_always"), (0x0c, "int", "field_0c"),
                         (0x10, "float", "first_lap_allowance")]),
}

SIZES = {"AICar::SegmentInfo": 24, "AICar::SegmentInfo::Notes": 8, "AICar::ProxerInfo": 8, "AITrackInfo": 20}

NAMES = {
    "LocalCar": {
        0xec0: ("teleported", "bool", "LocalCar::Teleport sets 1; FillNetPacket sends it (packet +0x39) then clears it"),
    },
    "AICar": {
        # --- mode: slow_mode/fast_mode swap these two member-function pointers (only fast_mode is ever used;
        #     slow_drive/slow_interact are empty stubs) ---
        0xec8: ("drive_fn", "ptr", "fast_drive or slow_drive; Update calls it on think ticks when not frozen"),
        0xecc: ("interact_fn", "ptr", "fast_interact or slow_interact; socialize calls it per nearby car"),

        # --- car directly ahead (checker -> dont_push, consumed by lonslam) ---
        0xed0: ("ahead_valid", "bool", "dont_push; socialize clears it each think"),
        0xed4: ("ahead_dist", "float", "m along my heading; the nearest car within 17.4 m ahead and 2.5 m sideways"),
        0xed8: ("ahead_rel_speed", "float", "m/s along my heading, other minus me (negative = closing); lonslam brakes on (dist-4.9)/rel"),

        # --- car alongside (checker -> dont_check, consumed by latslam); the docs' 'threat fields' ---
        0xedc: ("side_lat", "float", "m to my right (signed) of a car 1.8-4.6 m beside me and within 5.4 m fore/aft"),
        0xee0: ("side_lat_rel_speed", "float", "m/s, its lateral closing speed; dont_check keeps the most urgent car"),
        0xee4: ("side_lon", "float", "m along my heading (negative = behind me)"),
        0xee8: ("side_lon_rel_speed", "float", "m/s along my heading"),
        0xeec: ("side_i_lead", "bool", "checker: the side car is behind me (humans only if well back and not gaining); latslam then ignores it"),
        0xef0: ("slam_steer", "float", "steering bias added in latslam (+-0.0029/tick away from the side car, x0.992 decay); "
                                       "ApplyExternalForce car contact (type 0x6d) nudges it +-0.005"),
        0xef4: ("side_valid", "bool", "dont_check; cleared by socialize"),
        0xef5: ("side_squeezed", "bool", "a second side car on the other side -> latslam 'squeezebrake' (brake >= 0.8)"),

        # --- predicted contact with a car in my path (passer -> add_contact, consumed by get_mpath_target) ---
        0xef8: ("contact_time", "float", "s until contact: in_path distance / closing speed; the soonest one wins"),
        0xefc: ("contact_line_lat", "float", "m, where the other car will be relative to the racing line"),
        0xf00: ("pass_offset", "float", "m sideways added to the target to pass (CONTACT / CONTACT-FLIP, +-6.7 m); x0.986 per think"),
        0xf04: ("contact_center_lat", "float", "m, the other car's predicted offset from the centre line (near an edge -> pass on the other side)"),
        0xf08: ("contact_valid", "bool", "add_contact"),
        0xf09: ("contact_multiple", "bool", "a second contact this think -> target velocity halved"),

        # --- per-Update state (init_rtinfo / init_rt_lat) ---
        0xf0c: ("now", "float", "PhysicsGetTime() at the start of each Update; every *_time field is compared to it"),
        0xf10: ("speed", "float", "m/s, |velocity| including y (init_rtinfo)"),
        0xf14: ("track_half_width", "float", "m: centre-line segment +0x14 x 0.5 (that field is the FULL width on the centre line); 10 m if no bead"),
        0xf18: ("center_lat", "float", "m, signed offset from the centre line (CenterLine +0x54, calculate_lat); headon_panic "
                                       "steers full lock toward its sign, i.e. to the side the car is already on"),
        0xf1c: ("center_lat_norm", "float", "center_lat / ((full width - 1.905) / 2): -1..1 at the edge less a car width"),
        0xf20: ("center_lat_delta", "float", "m per centre-line update (CenterLine +0x58 = new lat - old lat), i.e. drift outward/inward"),
        0xf24: ("line_lat", "float", "m, the car's offset right of the racing line at the bead (init_rt_lat)"),
        0xf2c: ("race_fraction", "float", "current lap / number of laps"),
        0xf30: ("finished", "bool", "Car +0x4fc == 3: after the flag it pulls to its side, brakes and ramps throttle to 0.2 over 30 s"),
        0xf34: ("lap", "int", "check_for_lapchange copies the deity's lap count and calls begin_lap on change"),
        0xf38: ("race_begun", "bool", "Update calls begin_race once, then sets it; reset clears it"),
        0xf3c: ("start_lat", "float", "center_lat at begin_race: the grid lane held during the launch blend"),
        0xf40: ("tick", "uint", "16-bit Update counter: too_fast every 16, stranded/damage every 64, roll_dice every 256"),
        0xf44: ("progress_seg", "ptr:ILSeg", "bead segment at the last progress; check_for_stranded"),
        0xf48: ("steer_damping", "float", "+0.12 (to 0.7) when the command reverses the wheel; x0.99 per tick (decay); AISetSteering"),
        0xf4c: ("steer", "float", "-1..1, the slew-limited command AISetSteering writes to Car +0x500"),
        0xf50: ("steer_gain", "float", "per-track skill: skill +0xa0 + track*0x18; bearing/max_angle x this"),
        0xf54: ("tc_always", "bool", "AITrackInfo +8: traction control at any speed (else only < 33.3 m/s)"),
        0xf58: ("skill", "ptr:SkillInfo2", "AIGetSkill(car): the drivers.res skill/personality record"),

        # --- the steering target (get_mpath_target / get_real_target) ---
        0xf5c: ("target_world", "struct:Point2D", "target point in world x,z before localising; the only AI data in the replay packet (+0x3c)"),
        0xf64: ("target_pos", "struct:Point2D", "rabbit point (IdealLine::get_rabbit_position, lookahead x speed, >= 20 m), then car-local"),
        0xf6c: ("target_vel", "struct:Point2D", "line tangent x target_speed of the rabbit's segment, then car-local"),
        0xf74: ("target_pos_dir", "struct:Point2D", "target_pos normalised; fast_drive steers by its bearing"),
        0xf7c: ("target_vel_dir", "struct:Point2D", "target_vel normalised; .z <= 0 (target behind) -> full lock"),
        0xf84: ("target_speed", "float", "m/s, |target_vel|; obey_speed_limit brakes on speed - this"),

        # --- the centre-line frame at the car (init_rt_lat), used by the recovery / launch blends ---
        0xf88: ("center_pos", "struct:Point2D", "centre-line point at the car's progress (world x,z)"),
        0xf90: ("center_recover_vel", "struct:Point2D", "centre tangent x 26.67 m/s (60 mph): the 'fouroff' recovery velocity"),
        0xf98: ("center_tan", "struct:Point2D", "centre-line unit tangent"),
        0xfa0: ("edge_offset", "struct:Point2D", "center_right x (half width - 5 m); x recover_side = the recovery point"),
        0xfa8: ("side_edge_offset", "struct:Point2D", "edge_offset already signed toward the car's own side"),
        0xfb0: ("center_right", "struct:Point2D", "(tan.z, -tan.x): lateral notes, start_lat and pass_offset move the target along it"),

        # --- timers (all absolute times in s) ---
        0xfb8: ("progress_time", "float", "last time the bead changed segment on the ground, upright, dry; ages 1 s extra per "
                                          "check when upside down or in water; stranded when now - it > skill+0 + 6"),
        0xfbc: ("finish_time", "float", "set once when finished; drives the post-race throttle ramp"),
        0xfc0: ("crash_time", "float", "ResolveExternalImpulse: |J| > 5000 x skill+0x40; within skill+0x14 s -> crash_aftershock"),
        0xfc4: ("bump_time", "float", "ResolveExternalImpulse: |J| > 1500 x skill+0x3c; within skill+0x10 s -> 'bump', traction control off"),
        0xfc8: ("fouroff_time", "float", "fire_fouroff (4 wheels off road < 31 m/s, aftershock, DoneLowering); target blends to the "
                                         "recovery point for 12 s after it"),
        0xfcc: ("race_start_time", "float", "begin_race; launch blend and yaw control (off for the first 10 s) count from it"),
        0xfd0: ("freeze_time", "float", "headon_panic / check_for_damage: no driving decisions until now > it + 2 x skill+0 + 2 ('freeze')"),
        0xfd4: ("wall_heat", "float", "+1 per wall force (ApplyExternalForce type 0x6e), x0.992 per tick; > 50 below 8.9 m/s -> teleport_to_track"),
        0xfdc: ("recover_side", "float", "+-1: sign of center_lat when fouroff fired / the race finished"),
        0xfe0: ("offground_cut", "float", "throttle subtracted while the rear wheels are airborne, +0.03 per tick ('offground_cut')"),
        0xfe4: ("line_update_time", "float", "now at the last update_line_info"),
        0xfe8: ("line", "ptr:IdealLine", "AIGetLine(car) = the car's AICarInfo +0x10 (a ConstIdealLine); NULL when its head is NULL"),
        0xfec: ("msgs", "arr:ptr:10", "_MSG reason strings this think (pointers into msg_tab at 0x4ed408); replayed as a bitmask"),
        0x1014: ("num_msgs", "int", "cleared on each think tick"),
        0x1018: ("min_brake", "float", "fast_drive: brake = max(brake, this); never written anywhere (always 0)"),
        0x101c: ("dice_cold_apex", "bool", "roll_dice: Random(255) > skill+0x39; lets fast_drive lift ('cold_apex') < 1 s from a braking point"),
        0x101d: ("dice_hot_entry", "bool", "roll_dice: Random(255) > skill+0x38; skips the early braking inside 2 s of a corner ('enter_hot')"),
        0x101e: ("dice_unused", "bool", "roll_dice: Random(255) > skill+0x3a; never read"),

        # --- pace control: the AI chases a target lap time ---
        0x1020: ("too_fast", "float", "0..1 ahead-of-schedule amount (check_for_too_fast): e-brake up to 0.6, throttle cap, slower "
                                      "target, softer steering; 'laptime slowdown' / 'VerySlow'"),
        0x1024: ("lap_target_time", "float", "s: base_lap_time + lap_time_offset (begin_lap)"),
        0x1028: ("base_lap_time", "float", "s: per-track skill +0x98 + track*0x18; negative = use line_lap_time"),
        0x102c: ("lap_time_spread", "float", "s: per-track skill +0xa4 + track*0x18; random +- per lap"),
        0x1030: ("pace", "float", "lap_target_time / line_lap_time: scales the line's cumulative time into a schedule"),
        0x1034: ("schedule_error", "float", "s: lap clock - line cum_time at the bead x pace (negative = ahead)"),
        0x1038: ("lap_clock", "ptr", "float* to this car's current-lap time in RaceDeity::global (+0x78 + car*0x74)"),
        0x103c: ("throttle_cap", "float", "1.0 .. 0.6 from too_fast; after the finish 1.0 .. 0.2; AISetThrottle clamps to it"),
        0x1040: ("lap_time_offset", "float", "s: AITrackInfo +0x10 on lap 0 (standing start) + random spread"),
        0x1044: ("line_lap_time", "float", "s: the racing line's own lap time (last segment's cum_time + step / mean speed)"),
        0x1048: ("proxer_info", "ptr:struct:AICar::ProxerInfo", "num_proxer_info entries; decay multiplies each .level by skill+0x60 per tick"),
        0x104c: ("num_proxer_info", "int", "AICarCount()"),

        # --- per-segment learned notes (drivers.res .dnt) ---
        0x1050: ("learn_hiatus", "int", "segments to wait before analyze may adjust notes: 13 at the start, >= 7 after contact ('hiatus')"),
        0x1054: ("seginfo", "ptr:struct:AICar::SegmentInfo", "num_segs x 24, filled by init_seginfo from GetDriverData (the .dnt notes)"),
        0x1058: ("num_segs", "int", "the line's segment count (IdealLine +0x34)"),
        0x105c: ("seg_index", "int", "ILSeg.index of the bead segment (update_segment_info)"),
        0x1060: ("next_seg_index", "int", "(seg_index + 1) % num_segs"),
        0x1064: ("seg_t", "float", "bead_t: 0..1 blend between seg_index and next_seg_index notes"),
        0x1068: ("seg_changed", "bool", "the bead entered a new segment this Update"),
        0x1069: ("seg_valid", "bool", "set once the bead has been seen non-NULL; gates in_path and the notes"),
        0x106c: ("wall_hit_lat", "float", "center_lat at the last wall impulse; analyze pushes the lateral notes away from that side"),
        0x1070: ("last_learn_lap", "int", "lap of the last note change; learn mode stops the race 3 laps after it ('done learning')"),
        0x1074: ("latslam_rel_speed", "float", "side_lat_rel_speed when latslam last acted; never read"),
        0x1078: ("launch_time", "float", "s: AITrackInfo +0 + AITrackInfo +4 x car index; the start blends from the grid lane to the line"),
        0x107c: ("teleport_armed_time", "float", "-100 = unarmed; teleport_to_track fires only on a call 2-5 s after an arming call"),
    },
    # what AICar reads from its skill record (AIGetSkill; drivers.res). Meanings from AICar's use only.
    "SkillInfo0": {
        0x00: ("recovery_time", "float", "? s: stranded after this + 6 s without progress; freeze = 2 x this + 2 s"),
        0x04: ("think_ticks", "float", "Update thinks when tick % n == n-1 (0 = every tick)"),
        0x08: ("brake_release_lag", "float", "AISetBrake: releasing blends from the old braking by max(this, 0.8, too_fast)"),
        0x10: ("bump_tc_off_time", "float", "s after a bump with traction control off"),
        0x14: ("aftershock_time", "float", "s of crash_aftershock after a crash-sized impulse"),
        0x1c: ("throttle_scale", "float", "AISetThrottle multiplies the command by it"),
        0x24: ("target_speed_scale", "float", "get_mpath_target multiplies the rabbit velocity by it (after the launch)"),
        0x28: ("steer_lock_time", "float", "s for lock-to-lock: steer slew = 2 / (this x 31.25) per call"),
        0x34: ("overspeed_throttle", "float", "obey_speed_limit ('MPH'): throttle = this - 0.15 x overspeed, brake = this + 0.15 x overspeed"),
        0x38: ("hot_entry_threshold", "byte", "dice: hot entry when Random(255) > this"),
        0x39: ("cold_apex_threshold", "byte", "dice: cold apex when Random(255) > this"),
        0x3a: ("dice3_threshold", "byte", "dice: third roll, result unused"),
        0x3c: ("bump_impulse", "float", "hard hit when |impulse| > 1500 x this"),
        0x40: ("crash_impulse", "float", "crash when |impulse| > 5000 x this"),
        0x60: ("proxer_decay", "float", "per-tick multiplier on ProxerInfo.level"),
    },
    "SkillInfo2": {
        0x70: ("car_name", "arr:char:32", "the car this driver is meant for; ctor logs 'driving inappropriate car' on mismatch"),
    },
    "Proxer": {   # g_proxer: the car-proximity table socialize/passer read
        0x00: ("num_objects", "int", "Proxer(n)"),
        0x0c: ("record_size", "int", "n x 12 + 16: pos, vel, then per-pair delta pointers"),
        0x10: ("records", "ptr", "record k at records + k x record_size: Point2D pos, Point2D vel, ...; "
                                 "socialize reads the sorted ProxerDelta* list at +0x10 + (2n + i) x 4"),
    },
}

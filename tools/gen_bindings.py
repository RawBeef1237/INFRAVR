#!/usr/bin/env python3
"""Generates the SteamVR action manifest + default bindings (dist: bin/infravr/actions)."""
import json, os, sys

out = sys.argv[1] if len(sys.argv) > 1 else 'actions'
os.makedirs(out, exist_ok=True)

B = [
    ("Jump", "Jump"), ("Crouch", "Crouch (tap) / recenter (hold 1 s)"), ("Sprint", "Sprint (toggle)"),
    ("UseRight", "Use / camera shutter (right hand)"), ("UseLeft", "Use / camera shutter (left hand)"),
    ("GripLeft", "Grab / holster (left hand)"), ("GripRight", "Grab / holster (right hand)"),
    ("Flashlight", "Flashlight"), ("Phone", "Phone"), ("Reload", "Reload (camera battery) / back in menus"),
    ("Pause", "Pause menu (tap) / screen mode (hold 1 s)"), ("Recenter", "Recenter (hold 1 s, optional extra button)"),
]
actions = [{"name": "/actions/main/in/Move", "type": "vector2"},
           {"name": "/actions/main/in/Turn", "type": "vector2"}]
actions += [{"name": "/actions/main/in/%s" % n, "type": "boolean"} for n, _ in B]
actions += [{"name": "/actions/main/in/PoseLeft", "type": "pose"},
            {"name": "/actions/main/in/PoseRight", "type": "pose"},
            {"name": "/actions/main/in/SkeletonLeft", "type": "skeleton", "skeleton": "/skeleton/hand/left"},
            {"name": "/actions/main/in/SkeletonRight", "type": "skeleton", "skeleton": "/skeleton/hand/right"},
            {"name": "/actions/main/out/HapticLeft", "type": "vibration"},
            {"name": "/actions/main/out/HapticRight", "type": "vibration"}]
loc = {"language_tag": "en_US", "/actions/main": "INFRA",
       "/actions/main/in/Move": "Move", "/actions/main/in/Turn": "Turn",
       "/actions/main/in/PoseLeft": "Left hand pose", "/actions/main/in/PoseRight": "Right hand pose",
       "/actions/main/in/SkeletonLeft": "Left hand fingers", "/actions/main/in/SkeletonRight": "Right hand fingers",
       "/actions/main/out/HapticLeft": "Left haptics", "/actions/main/out/HapticRight": "Right haptics"}
for n, d in B:
    loc["/actions/main/in/%s" % n] = d

# Crouch and sprint sit where INFRA's own gamepad support has them: crouch = right stick click
# (tap; hold 1 s = recenter), sprint = left stick click (toggle). B / the right menu button reloads.
controllers = {
    # type: (binding file, layout)
    "knuckles": dict(stick="thumbstick", jump=("right", "a"), reload=("right", "b"), flash=("left", "a"),
                     pause=("left", "b"), reload2=("right", "trackpad"), phone=("left", "trackpad")),
    "oculus_touch": dict(stick="joystick", jump=("right", "a"), reload=("right", "b"), flash=("left", "x"),
                         pause=("left", "y"), phone=None),
    "vive_cosmos_controller": dict(stick="joystick", jump=("right", "a"), reload=("right", "b"), flash=("left", "x"),
                                   pause=("left", "y"), phone=None),
    "hpmotioncontroller": dict(stick="joystick", jump=("right", "a"), reload=("right", "b"), flash=("left", "x"),
                               pause=("left", "y"), phone=None),
    "holographic_controller": dict(stick="joystick", jump=("right", "trackpad"), reload=("right", "application_menu"),
                                   flash=("left", "trackpad"), pause=("left", "application_menu"), phone=None),
    "vive_controller": dict(stick="trackpad", jump=("right", "trackpad"), crouch=("right", "application_menu"),
                            flash=None, pause=("left", "application_menu"), reload=None, phone=None),
}
aliases = {"oculus_touch": ["oculus_touch", "oculus_touch_plus", "oculus_touch_v3", "oculus_touch_pro",
                            "pico_controller", "vive_focus3_controller"],
           "knuckles": ["knuckles", "knuckles_ev1"]}

def button(hand, inp, action, mode="button"):
    return {"path": "/user/hand/%s/input/%s" % (hand, inp), "mode": mode,
            "inputs": {"click": {"output": "/actions/main/in/%s" % action}}}

manifest = {"default_bindings": [], "actions": actions,
            "action_sets": [{"name": "/actions/main", "usage": "single"}], "localization": [loc]}

for ctype, L in controllers.items():
    fname = "bindings_%s.json" % ctype
    for t in aliases.get(ctype, [ctype]):
        manifest["default_bindings"].append({"controller_type": t, "binding_url": fname})
    st = L["stick"]
    src = []
    if st == "trackpad":  # Vive wands: pads for move/turn
        src.append({"path": "/user/hand/left/input/trackpad", "mode": "trackpad",
                    "inputs": {"position": {"output": "/actions/main/in/Move"},
                               "click": {"output": "/actions/main/in/Sprint"}}})
        src.append({"path": "/user/hand/right/input/trackpad", "mode": "trackpad",
                    "inputs": {"position": {"output": "/actions/main/in/Turn"}}})
    else:
        src.append({"path": "/user/hand/left/input/%s" % st, "mode": "joystick",
                    "inputs": {"position": {"output": "/actions/main/in/Move"},
                               "click": {"output": "/actions/main/in/Sprint"}}})
        src.append({"path": "/user/hand/right/input/%s" % st, "mode": "joystick",
                    "inputs": {"position": {"output": "/actions/main/in/Turn"},
                               "click": {"output": "/actions/main/in/Crouch"}}})
    for key, action in (("jump", "Jump"), ("crouch", "Crouch"), ("flash", "Flashlight"), ("pause", "Pause"),
                        ("reload", "Reload"), ("reload2", "Reload"), ("phone", "Phone")):
        if L.get(key):
            hand, inp = L[key]
            src.append(button(hand, inp, action))
    for hand, act in (("left", "UseLeft"), ("right", "UseRight")):
        src.append(button(hand, "trigger", act))
    for hand, act in (("left", "GripLeft"), ("right", "GripRight")):
        g = button(hand, "grip", act)
        if ctype == "knuckles":
            g["parameters"] = {"click_activate_threshold": "0.55", "click_deactivate_threshold": "0.35"}
        src.append(g)
    b = {"action_manifest_version": 0, "category": "steamvr_input", "controller_type": ctype,
         "name": "INFRA VR default (%s)" % ctype, "description": "INFRA VR default bindings",
         "bindings": {"/actions/main": {
             "sources": src,
             "poses": [{"output": "/actions/main/in/PoseLeft", "path": "/user/hand/left/pose/tip"},
                       {"output": "/actions/main/in/PoseRight", "path": "/user/hand/right/pose/tip"}],
             "skeleton": [{"output": "/actions/main/in/SkeletonLeft", "path": "/user/hand/left/input/skeleton/left"},
                          {"output": "/actions/main/in/SkeletonRight",
                           "path": "/user/hand/right/input/skeleton/right"}],
             "haptics": [{"output": "/actions/main/out/HapticLeft", "path": "/user/hand/left/output/haptic"},
                         {"output": "/actions/main/out/HapticRight", "path": "/user/hand/right/output/haptic"}]}}}
    json.dump(b, open(os.path.join(out, fname), "w"), indent=2)
json.dump(manifest, open(os.path.join(out, "action_manifest.json"), "w"), indent=2)
print("wrote", out)

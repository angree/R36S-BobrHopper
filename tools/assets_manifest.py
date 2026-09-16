"""Logical asset names -> upstream files. Mirrors src/Models.ts and src/Audio.ts of expo-crossy-road.

Paths are relative to upstream/expo-crossy-road/assets. Names are lowercase (Linux is case-sensitive).
"""

M = "models/"

MESHES = {
    "grass": M + "environment/grass/model.obj",
    "road": M + "environment/road/model.obj",
    "river": M + "environment/river/0.obj",
    "railroad": M + "environment/railroad/0.obj",
    "lily_pad": M + "environment/lily_pad/0.obj",
    "train_light_inactive": M + "environment/train_light/inactive/0.obj",
    "train_light_active_0": M + "environment/train_light/active/0/0.obj",
    "train_light_active_1": M + "environment/train_light/active/1/0.obj",
    "train_front": M + "vehicles/train/front/0.obj",
    "train_middle": M + "vehicles/train/middle/0.obj",
    "train_back": M + "vehicles/train/back/0.obj",
    "police_car": M + "vehicles/police_car/0.obj",
    "blue_car": M + "vehicles/blue_car/0.obj",
    "blue_truck": M + "vehicles/blue_truck/0.obj",
    "green_car": M + "vehicles/green_car/0.obj",
    "orange_car": M + "vehicles/orange_car/0.obj",
    "purple_car": M + "vehicles/purple_car/0.obj",
    "red_truck": M + "vehicles/red_truck/0.obj",
    "taxi": M + "vehicles/taxi/0.obj",
    "chicken": M + "characters/chicken/0.obj",
    "brent": M + "characters/brent/0.obj",
    "avocoder": M + "characters/avocoder/avocoder.obj",
    "bacon": M + "characters/bacon/bacon.obj",
    "wheeler": M + "characters/wheeler/wheeler.obj",
    "palmer": M + "characters/palmer/palmer.obj",
    "juwan": M + "characters/juwan/juwan.obj",
}
for i in range(4):
    MESHES[f"log_{i}"] = M + f"environment/log/{i}/0.obj"
    MESHES[f"tree_{i}"] = M + f"environment/tree/{i}/0.obj"
for i in range(2):
    MESHES[f"boulder_{i}"] = M + f"environment/boulder/{i}/0.obj"

# texture name -> png; by default a texture has the same name as its mesh
TEXTURES = {name: path[:-len("0.obj")] + "0.png" if path.endswith("/0.obj") else None
            for name, path in MESHES.items()}
TEXTURES.update({
    "avocoder": M + "characters/avocoder/avocoder.png",
    "bacon": M + "characters/bacon/bacon.png",
    "wheeler": M + "characters/wheeler/wheeler.png",
    "palmer": M + "characters/palmer/palmer.png",
    "juwan": M + "characters/juwan/juwan.png",
})
del TEXTURES["grass"]
del TEXTURES["road"]
TEXTURES["grass_light"] = M + "environment/grass/light-grass.png"   # Models.ts grass "0"
TEXTURES["grass_dark"] = M + "environment/grass/dark-grass.png"     # Models.ts grass "1"
TEXTURES["road_stripes"] = M + "environment/road/stripes-texture.png"  # Models.ts road "0"
TEXTURES["road_blank"] = M + "environment/road/blank-texture.png"      # Models.ts road "1"

# O14: the port's own models, generated in the repo instead of taken from upstream (tools/make_beaver.py).
# The paths are relative to the upstream assets folder the bakers are given, so no baker needs to change.
MESHES["beaver"] = "../../../assets_extra/models/beaver/0.obj"
TEXTURES["beaver"] = "../../../assets_extra/models/beaver/0.png"

# model name -> (mesh, texture), the units the game code asks for
MODELS = {name: (name, name) for name in TEXTURES if name in MESHES}
MODELS.update({
    "grass_0": ("grass", "grass_light"),
    "grass_1": ("grass", "grass_dark"),
    "road_0": ("road", "road_stripes"),
    "road_1": ("road", "road_blank"),
})

SOUNDS = {
    "chicken_move_%d" % i: "audio/buck%d.wav" % (i + 1) for i in range(12)
}
SOUNDS.update({
    "chicken_die_0": "audio/chickendeath.wav",
    "chicken_die_1": "audio/chickendeath2.wav",
    # O18: "car_passive_0" (the upstream engine loop) is gone. Game::playPassiveCarSound only ever plays
    # car_passive_1, so it was baked into every package without being heard - and it was the last upstream
    # recording left in the game after O15 replaced the other 25 with our own.
    "car_passive_1": "audio/car-horn.wav",
    "car_die_0": "audio/carhit.mp3",
    "car_die_1": "audio/carsquish3.wav",
    "button_in": "audio/Pop_1.wav",
    "button_out": "audio/Pop_2.wav",
    "banner": "audio/bannerhit3-g.wav",
    "water": "audio/watersplashlow.mp3",
    "train_alarm": "audio/Train_Alarm.wav",
    "train_move_0": "audio/train_pass_no_horn.wav",
    "train_move_1": "audio/train_pass_shorter.wav",
    "train_die_0": "audio/trainsplat.wav",
})

# O11.7: the port's own sounds, from the repo (not from upstream assets). The user generated the fanfare that plays
# when a Progression level is finished; it is loud, so game/sound_volume.h halves it.
SOUNDS_EXTRA = {
    "fanfare": "assets_extra/fanfare.mp3",
}
# O14: the beaver's own voice, picked in tools/sound_studio.py (files land in assets_extra/sounds/).
# A sound that has not been chosen yet is simply skipped by the baker.
for _i in range(2):  # O15: two hops, recorded by the user (assets_extra/source_audio/Beaver2-3.wav)
    SOUNDS_EXTRA["beaver_move_%d" % _i] = "assets_extra/sounds/beaver_move_%d.wav" % _i
for _i in range(2):
    SOUNDS_EXTRA["beaver_die_%d" % _i] = "assets_extra/sounds/beaver_die_%d.mp3" % _i
SOUNDS_EXTRA["beaver_tail_slap"] = "assets_extra/sounds/beaver_tail_slap.mp3"
# O14: three fanfares, picked at random when a Progression level ends
for _i in range(3):
    SOUNDS_EXTRA["fanfare_%d" % _i] = "assets_extra/sounds/fanfare_%d.mp3" % _i

# Port's own music (the original has none), from Music/ in the repo root, not from upstream assets:
# the title song plays on the home and game over screens, the others one per game in this order.
MUSIC = {
    "title": "Arcade Energy.mp3",
    "track_1": "Chiptune 002.mp3",
    "track_2": "Chiptune 003.mp3",
    "track_3": "Chiptune 003 (1).mp3",
    "track_4": "Chiptune 004.mp3",
    "track_5": "Chiptune 004 (1).mp3",
    "track_6": "Chiptune 005.mp3",
    "track_7": "Chiptune 005 (1).mp3",
}

FONT = "fonts/retro.ttf"
IMAGES = {
    # O15: our own logo (tools/make_logo.py) instead of the original's wordmark, which is Hipster Whale's trademark
    "title": "../../../assets_extra/images/title.png",
    "hand_0": "images/hand/0.png",
    "hand_1": "images/hand/1.png",
}
# src/Images.ts buttons used by the port's screens (game over footer, settings, pause)
for _name in ("long_play", "settings", "share", "rank", "back", "menu", "mute", "shadows", "conserve_battery",
              "credits"):
    IMAGES["button_" + _name] = f"images/buttons/{_name}.png"

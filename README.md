# Bóbr Hopper — R36S / ArkOS

A hopping game for the **R36S** handheld and other ArkOS devices: a native C++17 build on SDL2 and OpenGL ES 2,
640×480, installed through the console's Ports menu.

Current build: **v019**.

![the game](docs/screenshot.png)

## What this is based on

The game logic is a port of **[EvanBacon/expo-crossy-road](https://github.com/EvanBacon/expo-crossy-road)**
(MIT, commit `6f2e84e5`) — an open-source TypeScript + three.js game. Its structure, timings and behaviour were
followed closely: rows, traffic, logs and the hero's hop reproduce what the original does, and the repository
keeps a trace harness that compares this port against a reference run of the original, step by step.

**This project is not affiliated with, endorsed by, or connected to Hipster Whale, Yodo1 or the "Crossy Road"
game or trademark.** It is a hobby port of an MIT-licensed open-source project, with its own name, its own
artwork and its own sounds. See [NOTICE.md](NOTICE.md).

## What was added on top of the original

- **Progression mode** beside the endless Classic mode: levels of 10·k rows with a chequered finish line, a
  career that remembers the level reached, a rank for every level, and a fanfare halfway through.
- **A difficulty curve that keeps going.** The first rows are gentle for small children; from 150 points the
  *slowest* traffic, logs and railroad spacing are cut away one layer at a time, and the minimum number of
  dangerous rows in a row keeps climbing with no ceiling (`src/game/difficulty.h`).
- **A guaranteed way forward:** every generated row is checked to be crossable (`build/path_check.sh`).
- **Our own hero and artwork:** a voxel beaver (`tools/make_beaver.py`) as the default character, the chicken
  kept as an alternative, an isometric wordmark (`tools/make_logo.py`), and **every sound effect replaced** with
  our own recordings and generated takes.
- **English and Polish** interface text, chosen in the settings.
- Fixes to behaviour the original shares with its own source: a chain of fast hops could carry the hero over a
  river or a railway. Each fix has a check script in `build/`.

Every deliberate deviation from the original sits behind `GameContext::originalBehaviour`, so the trace
comparison against the original still runs clean.

## Building

You need **Windows with Git Bash** and **Python 3** (the bakers use Pillow: `pip install pillow fonttools`), plus
**WSL** with Ubuntu 22.04 for the ELF checks. `build/setup_tools.sh` fetches the rest: zig 0.14.1 (which builds
both the PC executable and the aarch64 binary) and the SDL2 headers and library the device build links against.

```sh
sh build/setup_tools.sh
sh build/bake_all.sh                 # unpacks the upstream assets and bakes data/
sh build/build_r36s.sh bobrhopper    # -> out/r36s/bobrhopper.aarch64 (+ check_elf)
```

The binary is deliberately narrow: GLIBC 2.28 or older, and nothing in `NEEDED` beyond
`libSDL2-2.0.so.0`, `libc`, `libm`, `libdl` and `libpthread` — every GL entry point is resolved through
`SDL_GL_GetProcAddress`, because the Mali driver's soname on ArkOS is not predictable. `build/check_elf.sh`
fails the build if that ever stops being true.

Package it for a card:

```sh
powershell -File tools/package_r36s.ps1 -Version v019   # -> out/package/BobrHopper-R36S-v019.zip
```

## Installing on the console

Unpack the zip into the card's **EASYROMS** partition so its `ports` folder merges with the existing one:

```
EASYROMS\ports\BobrHopper.sh
EASYROMS\ports\bobrhopper\bobrhopper.aarch64
EASYROMS\ports\bobrhopper\data\...
```

Then **Ports → BobrHopper** in the console's menu. Settings and the best score live in
`ports\bobrhopper\conf\crossy.cfg`; if the game will not start, the launcher leaves
`ports\bobrhopper\bobrhopper-launcher.log` behind.

**Controls:** D-pad hops (on release, like the original), A hops forward / starts a new game, Start pauses,
Select opens the settings, Select + Start quits, Select + L shows the frame counter.

The launcher runs the game natively first (SDL2 KMSDRM + GLES2) and falls back to PortMaster's WestonPack
runtime only if the video setup fails; the working mode is remembered in `conf/video_mode`. There is no
gptokeyb: the game reads the pad itself, and gptokeyb on top doubled every press.

## Testing without the console

```sh
sh build/build_pc.sh bobrhopper   # the same game on the PC, SDL2 + desktop GL
sh build/smoke_test.sh            # a bot plays 20000 steps; the digest must not move
sh build/run_traces.sh            # this port against a reference run of the original
sh build/launcher_test.sh         # the launcher against a fake PortMaster (run it from WSL bash)
```

`build/*_check.sh` are regression checks written for specific bugs — each one's header explains the bug it guards.

## Layout

```
src/game      the game itself, shared by every platform
src/engine    maths, assets, audio, config, and the SDL/GL platform layer
src/ui        HUD, menus, ranks, translations
apps          bobrhopper.cpp is this build; the others are development tools
port          the PortMaster launcher installed as ports/BobrHopper.sh
tools         asset bakers and the card sync script
assets_extra  our own models, images and sounds - a file here overrides the baked upstream asset
release/      built packages, ignored by git (see release/README.md)
```

## Credits

- **[Evan Bacon](https://github.com/EvanBacon/expo-crossy-road)** — the original open-source game this port
  follows, MIT licensed. [NOTICE.md](NOTICE.md) says which part of this repository is whose.
- Port, artwork and sounds: G. Korycki, with Claude Code.

## Po polsku

**Bóbr Hopper** na konsolę **R36S** (ArkOS): natywna wersja w C++17 na SDL2 i OpenGL ES 2, 640×480, instalowana
przez menu Ports. Logika gry to port otwartoźródłowego projektu **expo-crossy-road** Evana Bacona (licencja MIT).
Projekt **nie jest** powiązany z firmą Hipster Whale ani z grą „Crossy Road" — ma własną nazwę, własną grafikę
i własne dźwięki.

Co doszło ponad pierwowzór: tryb progresji z poziomami, metą i rangami, kariera zapamiętywana między grami,
trudność rosnąca bez końca od 150 punktów, gwarancja przejścia każdego rzędu, bóbr jako domyślny bohater,
własne logo, wszystkie dźwięki wymienione na własne oraz polski i angielski interfejs.

Instalacja: rozpakuj paczkę na partycję EASYROMS karty, tak żeby folder `ports` połączył się z istniejącym,
i uruchom z menu **Ports → BobrHopper**. Ustawienia i rekord: `ports\bobrhopper\conf\crossy.cfg`.

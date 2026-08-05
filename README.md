# Terminal Roguelike

A classic turn-based, permadeath, ASCII fantasy dungeon crawler, written in
modern C++17 using [libtcod](https://github.com/libtcod/libtcod).

## About this project

This game is built with [Claude Code](https://claude.com/claude-code), an AI
coding agent, as a test of its capabilities on a non-trivial C++ project. The
README and code comments are also written by Claude Code.

## One-time setup (Linux)

You need `git`, `cmake`, a C++ compiler, and `ninja` (recommended, faster than
default make). On Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y git cmake g++ ninja-build build-essential \
    pkg-config autoconf libtool
```

(`autoconf`/`libtool` are needed because vcpkg builds some of SDL3's
dependencies from source on Linux.)

On Fedora:

```bash
sudo dnf install -y git cmake gcc-c++ ninja-build pkgconf autoconf libtool
```

On Arch:

```bash
sudo pacman -S git cmake gcc ninja pkgconf autoconf libtool
```

## Build

```bash
# 1. Clone this repo, including the vcpkg submodule (only needed once)
git clone --recurse-submodules <this-repo-url>
cd <repo-directory>

# 2. Configure (this step will download & compile SDL3 + libtcod via vcpkg
#    the first time — expect this to take several minutes)
cmake -B build -S . -G Ninja

# 3. Build
cmake --build build

# 4. Run
./build/bin/roguelike
```

After the first configure, steps 3–4 are all you need when you change code.
If you add new .cpp files, re-run step 2 so CMake picks them up.

If you already have the folder locally without having cloned it (e.g. you
downloaded it some other way), run `git submodule update --init` instead of
step 1.

## Controls

- Arrow keys, `h j k l`, or `y u b n` (vim-style, including diagonals) to move
- Walk into an enemy to attack it
- `>` / `<` to take stairs down/up (must be standing on them)
- `g` to pick up whatever's on your tile (nothing is picked up automatically)
- `w` to open the weapon menu and equip a carried weapon (or bare fists)
- `a` to open the armor menu and equip a carried piece (or bare skin)
- `d` to drop your equipped weapon/armor or something from your pack
- `z` to open your known spells and cast one (unlocked by Intelligence — see
  below); movement then aims a targeting cursor instead of moving you, with
  a preview line showing what the shot would hit, `Enter` fires, `Esc` cancels
- On leveling up, `Shift+S` / `Shift+D` / `Shift+I` to put your point into
  Strength / Dexterity / Intelligence
- `Esc` to quit

## Project layout

```
roguelike/
├── CMakeLists.txt      # build configuration
├── vcpkg.json          # dependency manifest (libtcod, sdl3)
├── src/
│   ├── main.cpp         # entry point, game loop, rendering, input
│   ├── map.hpp/.cpp     # dungeon generation, FOV, fog of war
│   ├── entity.hpp/.cpp  # Weapon/Armor/Actor types, damage rolls
│   └── rng.hpp/.cpp     # shared random-number and dice-rolling utility
└── vcpkg/               # (created by you) vendored package manager
```

## Status

- [x] Window opens, `@` renders, moves with arrow keys / hjkl / diagonals
- [x] Map data structure (tiles: wall/floor)
- [x] Procedural dungeon generation (rooms + corridors)
- [x] Field of view / fog of war (including remembered monster sightings)
- [x] Monsters + turn-based melee combat (bump-to-attack, weapon dice)
- [x] Items & inventory (pickup, equip, drop)
- [x] Permadeath + restart flow
- [x] Multi-level dungeon (stairs up/down, floors persist as you leave/return)
- [x] Player attributes & leveling (Strength/Dexterity/Intelligence, XP)
- [x] Armor & evasion (equippable armor, Dexterity-based dodge chance)
- [x] Spells (Intelligence unlocks them; ranged, travel a fixed distance per
      turn rather than resolving instantly, with an aim-preview line) — only
      one spell (Magic Dart) so far, more planned
- [ ] Monster AI (currently stationary; only fights back when attacked)
- [ ] Multi-line message log (currently a single rolling line)

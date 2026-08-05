# Terminal Roguelike

A classic turn-based, permadeath, ASCII fantasy dungeon crawler, written in
modern C++17 using [libtcod](https://github.com/libtcod/libtcod).

## About this project

This game is being built with [Claude Code](https://claude.com/claude-code) as
an ongoing experiment in exploring what modern AI coding assistants are
capable of — from initial project setup and dependency wrangling, through
incrementally designing and implementing actual game systems (dungeon
generation, FOV, combat, items, leveling, and more) via conversation. The
`README.md` and code comments are also AI-written, kept up to date turn by
turn as the game grows.

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
- `i` to open your inventory and equip a carried weapon (or bare fists)
- `d` to drop your equipped weapon or something from your pack
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
│   ├── entity.hpp/.cpp  # Weapon/Actor types, damage rolls
│   └── rng.hpp/.cpp     # shared random-number utility
└── vcpkg/               # (created by you) vendored package manager
```

## Where we are / what's next

- [x] Window opens, `@` renders, moves with arrow keys / hjkl / diagonals
- [x] Map data structure (tiles: wall/floor)
- [x] Procedural dungeon generation (rooms + corridors)
- [x] Field of view / fog of war (including remembered monster sightings)
- [x] Monsters + turn-based melee combat (bump-to-attack, weapon dice)
- [x] Items & inventory (pickup, equip, drop)
- [x] Permadeath + restart flow
- [x] Multi-level dungeon (stairs up/down, floors persist as you leave/return)
- [x] Player attributes & leveling (Strength/Dexterity/Intelligence, XP)
- [ ] Armor & evasion
- [ ] Monster AI (currently stationary; only fights back when attacked)
- [ ] Multi-line message log (currently a single rolling line)

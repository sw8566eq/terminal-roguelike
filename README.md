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
- `.` to wait a turn
- `>` / `<` to take stairs down/up (must be standing on them)
- `g` to pick up whatever's on your tile (nothing is picked up automatically)
- `w` to open the weapon menu and equip a carried weapon (or bare fists)
- `a` to open the armor menu and equip a carried piece (or bare skin)
- `q` to open the potion menu and drink one (heal, a temporary stat boost, or
  teleport to a random spot on the floor — see below)
- `d` to drop your equipped weapon/armor or something from your pack
- `z` to open your known spells and cast one (unlocked by Intelligence — see
  below); for an aimed spell, movement then steers a targeting cursor instead
  of moving you, with a preview line (and a highlighted blast radius for an
  AoE spell) showing what the shot would hit, `Enter` fires, `Esc` cancels; a
  toggled spell (like Sandstorm) instead turns on/off immediately, no aiming;
  a summon spell raises a minion next to you immediately, no aiming either
- `m` to command your minions, if you have any: **Follow** (return to your
  side, defending themselves if attacked) or **Attack...** (pick a monster
  with a cursor — every minion switches to hunting it, reverting to Follow
  once it dies); walking into your own minion swaps places with it instead
  of attacking
- On leveling up, `Shift+S` / `Shift+D` / `Shift+I` to put your point into
  Strength / Dexterity / Intelligence
- `]` to open the full message log (scroll with `j`/`k` or arrows, `Esc` or
  `]` to close); the HUD always shows the last few messages anyway
- `?` to open a controls reference screen (`?` or `Esc` to close)
- `Esc` to quit

### Debug flags

These are for development/testing, not normal play:

- `--floor=N` — jump straight to floor N at startup instead of walking down
  from floor 1
- `--level=N` — spawn already at player level N, prompting for all N-1
  attribute points at startup so you allocate them yourself (same LevelUp
  prompt as leveling up for real); combinable with `--floor=N`
- `--reveal` — show the entire current floor's terrain/monsters/items
  regardless of exploration or line of sight (dimmed where not actually in
  view), for eyeballing spawns without exploring first
- `--dump-loot` — print every weapon/armor/potion/monster on the floor
  reached via `--floor=N` to the console and exit immediately, without
  opening a window
- `--give=<name>[,<name>...]` — add items straight to your carried
  inventory at startup (not equipped), by exact name across weapons,
  armor, and potions, e.g. `--give="Dagger,Potion of Teleportation"`;
  combinable with the other flags above

## Project layout

```
roguelike/
├── CMakeLists.txt      # build configuration
├── vcpkg.json          # dependency manifest (libtcod, sdl3)
├── src/
│   ├── main.cpp         # entry point, game loop, rendering, input, content tables
│   ├── map.hpp/.cpp     # dungeon generation, FOV, fog of war
│   ├── entity.hpp/.cpp  # Weapon/Armor/Potion/Actor types, damage rolls
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
- [x] Armor & evasion: monster attacks against the player are a live Dexterity
      contest (attacker vs. defender); the player's own weapons and spells
      against monsters instead roll their own accuracy ("hit-dice") against
      the target's flat evasion, so a fast dagger or a wide-area Fireball are
      harder to dodge than a heavy axe or a precise Magic Dart
- [x] Spells (Intelligence unlocks them): Magic Dart (single-target),
      Fireball (slow-moving orb, explodes into a 3x3 blast on impact —
      including on the caster, if cast too close), and Sandstorm (a
      toggled aura that follows the player around instead of being aimed,
      damaging everything in a 7x7 zone every turn while it drains mana)
- [x] Mana: spells cost mana to cast (or, for Sandstorm, to keep running),
      regenerating passively the same way HP does
- [x] Monster AI: chases when it can see you, keeps heading for the last
      place it saw you after you break line of sight (until it gets there),
      otherwise wanders idly — real A* pathfinding (libtcod's `TCODPath`), so
      it routes around walls instead of getting stuck on corners
- [x] Monster variety scales with depth (tougher monster types replace
      weaker ones as you descend, plus more of them per floor)
- [x] Potions: healing, temporary (+5, 15-turn) Strength/Dexterity/
      Intelligence boosts, and teleportation (random spot on the current floor)
- [x] Weapon/armor/potion variety also scales with depth, mirroring monsters
- [x] Passive HP regeneration (slow, scales with max HP)
- [x] Multi-line message log with scrollback (`]`), repeat-message coalescing
- [x] First distinct monster AI: Goblin Slinger (floors 1-4) and its tougher
      floor-5+ counterpart Orc Archer snipe from range without approaching,
      but permanently switch to a more accurate melee weapon and start
      chasing like an ordinary monster the moment they're touched once —
      every other monster still shares the plain wander/chase/remember
      behavior
- [x] Summoner playstyle, phase 1: a spell raises a temporary minion that
      fights at your side; command the whole pack with `m` (Follow or
      Attack a chosen monster); hostile monsters can target minions
      instead of always going after you, so they're real meat-shields;
      minions are immune to your own spells, swap places instead of being
      attacked if you walk into them, and follow you through stairs
- [ ] Summoner playstyle, phase 2: direct per-minion control (an "X-COM
      mode" cycling focus between individual minions) as an alternative to
      pack-wide orders, and raising the minion cap toward a 1-7 range
- [ ] Summoner playstyle, phase 3: true necromancy — reanimating the
      specific monster you just killed (not just conjuring a generic
      minion), via a corpse left on the ground and a spell that targets one

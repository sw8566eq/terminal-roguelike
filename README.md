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
- `>` / `<` to take stairs down/up (must be standing on them); this costs a
  turn, and anything next to you gets a parting shot as you go
- `g` to pick up everything on your tile, in one turn (nothing is picked up
  automatically)
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
- Commanding minions, if you have any: `o` / `p` cycle command focus to the
  next/previous minion, or `m` opens a roster menu to pick one (or "All")
  by letter — either way drops you into one cursor with no range limit.
  Move it onto a monster and press `Enter` to send the minion(s) to attack
  it, or onto empty ground to send them to hold that position (still
  defending themselves if something comes into range); `f` instead tells
  them to follow you, no aiming needed; `Esc` cancels. `Shift+P` snaps
  command focus back to yourself. The minion(s) you're currently
  commanding are highlighted so you don't lose track while aiming.
  Walking into your own minion swaps places with it instead of attacking
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
- [x] Armor & evasion, on one formula shared by everything that fights: an
      attack's accuracy (the attacker's Dexterity, plus a roll of the weapon
      or spell's own "hit-dice") is subtracted from the defender's evasion
      rating, and armor soaks a flat amount off whatever lands. A fast dagger
      or a wide-area Fireball are harder to dodge than a heavy axe or a
      precise Magic Dart, whoever is swinging them
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
- [x] Passive HP regeneration (slow, scales with max HP) — the player only;
      monsters don't heal, so wounds you inflict stick and chip-and-retreat
      tactics work. It's a per-monster toggle, ready for boss-type enemies
- [x] Multi-line message log with scrollback (`]`), repeat-message coalescing
- [x] First distinct monster AI: Goblin Slinger (floors 1-4) and its tougher
      floor-5+ counterpart Orc Archer snipe from range without approaching,
      but permanently switch to a more accurate melee weapon and start
      chasing like an ordinary monster the moment they're touched once —
      every other monster still shares the plain wander/chase/remember
      behavior
- [x] Monsters and the player run on one shared foundation: the same Actor,
      the same attack/dodge/damage math, the same per-turn upkeep and stat
      timers. Monsters carry an inventory — they wear armor that soaks your
      hits (an Orc starts in Leather Armor) and swap between carried weapons
      depending on how far away you are. Every non-natural item they were
      carrying drops on the floor when they die, and it's the same item you'd
      have found lying there. `x` (look) shows what a monster is wearing and
      carrying before you commit to the fight. Monsters can drink potions
      through the same code you do, but no monster is given any right now —
      it turned the floor into a consumables buffet
- [x] Summoner playstyle, phase 1: a spell raises a temporary minion that
      fights at your side; hostile monsters can target minions instead of
      always going after you, so they're real meat-shields; minions are
      immune to your own spells, swap places instead of being attacked if
      you walk into them, and follow you through stairs
- [x] Summoner playstyle, phase 2: per-minion control instead of pack-only
      orders — `o`/`p` cycle command focus between individual minions (or
      pick one from a roster with `m`), aim a cursor with no range limit,
      and send that minion (or the whole pack) to attack a chosen monster
      or hold a chosen tile, still defending itself either way; the
      minion(s) you're commanding are highlighted so you don't lose track
- [ ] Summoner playstyle, phase 3: true necromancy — reanimating the
      specific monster you just killed (not just conjuring a generic
      minion), via a corpse left on the ground and a spell that targets one;
      also raising the minion cap toward a 1-7 range now that per-minion
      control exists

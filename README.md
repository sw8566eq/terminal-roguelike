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
- Walk into an enemy to attack it (melee only, whatever you have equipped)
- `f` to fire an equipped ranged weapon (a Bow) — movement steers a targeting
  cursor clamped to the weapon's range, with a preview line showing what the
  shot would hit, `Enter` looses it, `Esc` cancels for free
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
  a summon spell raises a minion next to you immediately, no aiming either;
  so does a self-buff spell (Battle Fury, Iron Skin, Haste), which shows up
  in the sidebar's Buffs list with its remaining turns
  — freshly summoned minions default to Aggressive (below), not passive Follow
- Commanding minions, if you have any: `o` / `p` cycle command focus to the
  next/previous minion, or `m` opens a roster menu to pick one by letter —
  or `Shift+A` there to command the whole pack at once, however many you
  have. Either way drops you into one cursor with no range limit.
  Move it onto a monster and press `Enter` to send the minion(s) to attack
  it, onto empty ground to send them to hold that position (still
  defending themselves if something comes into range), or back onto
  yourself to call them off and have them follow you again; `f` gives that
  same follow order from wherever the cursor is, no aiming needed. `g` sets
  Aggressive instead: follows you the same as `f`'s order, but breaks off on
  its own to chase down and fight anything hostile that comes into view,
  rather than waiting for it to wander into reach. `Esc`
  cancels. `Shift+P` snaps
  command focus back to yourself. The minion(s) you're currently
  commanding are highlighted so you don't lose track while aiming.
  Walking into your own minion swaps places with it instead of attacking
- On leveling up, `Shift+S` / `Shift+D` / `Shift+I` to put your point into
  Strength / Dexterity / Intelligence. The first time Intelligence reaches 4,
  a one-time, permanent choice interrupts: `Shift+C` for Caster (Fireball,
  Sandstorm, Lightning Bolt), `Shift+U` for Summoner (Raise Skeleton, Place
  Swap, Summon Demon), or `Shift+M` for Combat Mage (Battle Fury, Iron Skin,
  Haste) — Magic Dart and its upgraded cousin Energy Lance stay available
  whichever path you pick
- `x` to look around: move a cursor over the map without spending a turn or
  moving, and the side panel describes what's under it — remembered terrain
  anywhere you've explored, and for anything actually in view, the item or
  the monster's weapon, armor, evasion, pack, and (for a caster like the
  Goblin Shaman) which spell it knows and how much mana it has left
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
- `--dump-loot` — print every weapon/armor/potion on the floor reached via
  `--floor=N`, plus every monster with its stats and everything it's
  carrying (which is real loot, since it drops), then exit immediately
  without opening a window
- `--fast-monsters` — give every hostile monster one extra action per turn,
  so they move and attack twice for each turn you take. No monster in the
  game does this normally; the flag exists to exercise the boss/elite
  behavior before an actual boss row is written
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
      Energy Lance (a straightforward upgrade to Magic Dart — better dice,
      steeper mana cost), Fireball (slow-moving orb, explodes into a 3x3
      blast on impact — including on the caster, if cast too close),
      Sandstorm (a toggled aura that follows the player around instead of
      being aimed, damaging everything in a 7x7 zone every turn while it
      drains mana), and Lightning Bolt (a piercing beam that hits every
      monster along its line of travel instead of stopping at the first one)
- [x] Spell schools: at Intelligence 4 you permanently pick Caster (Fireball,
      Sandstorm, Lightning Bolt), Summoner (Raise Skeleton, Place Swap,
      Summon Demon — a permanent, much stronger minion), or Combat Mage
      (Battle Fury, a melee-damage buff; Iron Skin, a flat-armor buff; Haste,
      below) — the other two paths' spells are locked out for the rest of
      that run. Magic Dart and Energy Lance are shared, available regardless
      of path
- [x] Extra actions per turn: Haste (the Combat Mage's top spell) lets you act
      twice for every turn the dungeon takes — you move, fight and cast at
      double rate while monsters, minions, buff timers and mana regen all run
      on the normal clock. It's a plain Actor property rather than a player
      perk, so a boss or elite monster can be given the same thing by setting
      one field on its table row (try `--fast-monsters` below to see it from
      the other side)
- [x] Mana: spells cost mana to cast (or, for Sandstorm, to keep running).
      Regeneration is per-Actor like HP regen — you regenerate, the Goblin
      Shaman deliberately doesn't, so its pool is a one-time budget
- [x] Monster AI: chases when it can see you, keeps heading for the last
      place it saw you after you break line of sight (until it gets there),
      otherwise wanders idly — real A* pathfinding (libtcod's `TCODPath`), so
      it routes around walls instead of getting stuck on corners
- [x] Monster variety scales with depth (tougher monster types replace
      weaker ones as you descend, plus more of them per floor)
- [x] Bosses: exactly one spawns on each floor its table row covers, on top of
      that floor's normal monsters rather than drawn from them. The first is
      the Orc Warlord on floor 3 — an outsized Orc arriving two floors before
      ordinary Orcs do, on a floor otherwise full of Rats and Goblins. It's
      the only thing in the game that regenerates besides you, so wounds you
      don't finish it with heal back off while you keep your distance, and
      it's the only monster carrying a potion — expect it to open by drinking
      a Potion of Strength. Its Chainmail and Warlord's Cleaver drop when it
      dies
- [x] Potions: healing, temporary (+5, 15-turn) Strength/Dexterity/
      Intelligence boosts, and teleportation (random spot on the current floor)
- [x] Weapon/armor/potion variety also scales with depth, mirroring monsters
- [x] Passive HP regeneration (slow, scales with max HP). A per-Actor toggle:
      the player and the Orc Warlord have it, every ordinary monster doesn't,
      so wounds you inflict on a normal monster stick and chip-and-retreat
      tactics work on everything except the boss
- [x] Multi-line message log with scrollback (`]`), repeat-message coalescing
- [x] Sectioned HUD: three bordered ASCII panels — the dungeon, a message log,
      and a status sidebar listing your stats, every temporary buff you have
      running with its remaining turns, plus the enemies and minions
      currently in view. The dungeon is bigger than the panel showing it, so
      the view scrolls to follow you (or your cursor while aiming)
- [x] Look around (`x`): inspect any explored tile without spending a turn
- [x] Ranged weapons for the player: a Bow fired with `f` instead of bumping,
      reusing the same travelling-projectile machinery spells use. Damage and
      hit chance both scale with Dexterity; ammo is unlimited for now
- [x] Monsters that cast: the Goblin Shaman (floors 1-4) throws the same Magic
      Dart you can learn, out of a real mana pool, through the same
      projectile code your own spells use — so it hits instantly at range,
      exactly as your Magic Dart does, and you can't sidestep it. Its whole
      design is a budget: 10 mana, 1 per dart, and no regeneration, so it
      gets exactly 10 darts and is then a 5 HP nuisance with claws.
      Outlasting it, breaking line of sight, or closing to melee are the
      counterplay, and `x` on it shows how much mana it has left. Projectiles
      now carry an owner, so anything that flies works the same in either
      direction — it never hits its own side, and XP flows to whoever fired it
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
      through the same code you do — the Orc Warlord boss is the only one
      given any, since handing them out widely turned the floor into a
      consumables buffet
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
- [x] Aggressive minion stance: `g` (or the default for a freshly summoned
      minion) sets a minion to chase down and fight anything hostile it can
      see instead of passively waiting at your side for something to wander
      into range
- [x] Monster attacks run through the player's own projectile code: a Shaman's
      dart is a real `Projectile` with an owner, resolved by the same function,
      dodge roll and damage math your spells use. Nothing in the pipeline
      assumes the player fired it any more, so a slow-moving monster spell
      would visibly cross the map over several turns the way your Fireball
      does — no monster has one yet
- [ ] Summoner playstyle, phase 3: true necromancy — reanimating the
      specific monster you just killed (not just conjuring a generic
      minion), via a corpse left on the ground and a spell that targets one;
      also raising the minion cap toward a 1-7 range now that per-minion
      control exists

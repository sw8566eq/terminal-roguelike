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
- `g` to pick up (nothing is picked up automatically). A lone item is taken
  straight away; with several on the tile a menu opens where letters toggle
  what you want, `Shift+A` selects all or none, `Enter` takes everything
  checked for one turn total, and `Esc` cancels for free
- `w` to open the weapon menu and equip a carried weapon (or bare fists)
- `a` to open the armor menu and equip a carried piece (or bare skin)
- `q` to open the potion menu and drink one: healing, a temporary +5 boost to
  Strength/Dexterity/Intelligence for 15 turns, or a teleport to a random spot
  on the current floor
- `d` to drop your equipped weapon/armor or something from your pack
- `z` to open your known spells and cast one (unlocked by Intelligence, and by
  your school — see Status); for an aimed spell, movement then steers a
  targeting cursor instead
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
  command focus back to yourself. With a single minion focused, `z` opens that
  minion's own abilities (if it has any) and works like your own spell menu —
  pick one, aim from *the minion's* position, `Enter` to use it. A Demon knows
  Wither Curse: 4 of its 10 mana, range 5, and the target hits 2 weaker in
  melee for 10 turns — two curses up front, then roughly one more per 60 turns
  as its mana comes back. Its MP is shown in the sidebar's Minions list, so you
  don't have to open the menu to check. The minion(s) you're currently
  commanding are highlighted so you don't lose track while aiming.
  Walking into your own minion swaps places with it instead of attacking
- On leveling up, `Shift+S` / `Shift+D` / `Shift+I` to put your point into
  Strength / Dexterity / Intelligence. The first time Intelligence reaches 4,
  a one-time, permanent choice interrupts: `Shift+C` for Caster (Fireball,
  Sandstorm, Lightning Bolt), `Shift+U` for Summoner (Summon Imp, Place
  Swap, Raise Dead, Summon Demon), or `Shift+M` for Combat Mage (Battle Fury,
  Iron Skin, Haste) — Magic Dart and its upgraded cousin Energy Lance stay available
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
- `--seed=N` — pin the random number generator, so the same seed builds the
  same floors with the same monsters, gear and rolls every run. Makes a bug
  reproducible instead of a one-off, and pairs with `--dump-loot` to diff
  what actually changed between two builds

## Project layout

```
roguelike/
├── CMakeLists.txt      # build configuration
├── vcpkg.json          # dependency manifest (libtcod, sdl3)
├── src/
│   ├── main.cpp             # entry point: argv, window, main loop
│   │
│   │                        # primitives
│   ├── entity.hpp/.cpp      # Weapon/Armor/Potion/Actor types, damage rolls
│   ├── map.hpp/.cpp         # dungeon generation, FOV, fog of war, pathfinding
│   ├── rng.hpp/.cpp         # shared random-number and dice-rolling utility
│   │
│   │                        # content and numbers
│   ├── content.hpp/.cpp     # the weapon/armor/potion/monster/minion tables
│   ├── spells.hpp/.cpp      # the spell table and what the player knows
│   ├── rules.hpp/.cpp       # tuning constants, derived stats, the combat formula
│   │
│   │                        # the world
│   ├── actors.hpp/.cpp      # queries and phrasing over a floor's Actor list
│   ├── projectile.hpp/.cpp  # spells and shots in flight, and their geometry
│   ├── level.hpp/.cpp       # one floor: its map, monsters, items, generation
│   │
│   │                        # the game
│   ├── game.hpp/.cpp        # GameState and the operations on it
│   ├── turn.hpp/.cpp        # what the world does between two player actions
│   ├── render.hpp/.cpp      # screen layout and every function that draws
│   └── input.hpp/.cpp       # one keyboard handler per mode
└── vcpkg/               # (created by you) vendored package manager
```

Each layer only depends on the ones above it. A new weapon or monster is a row
in `content.cpp`; a new spell is a row in `spells.cpp` plus however it resolves;
a new full-screen menu is a `Mode` in `game.hpp`, a render function, an input
handler, and a key to open it.

## Status

Everything here is built and playable except the single unchecked item, which is
what's next.

**World**

- [x] Procedurally generated multi-level dungeon — rooms, corridors, pits you
      can shoot over but not walk across, and stairs both ways. Floors persist
      as you leave and return
- [x] Field of view and fog of war, including remembered monster sightings
- [x] Depth scaling: monsters, weapons, armor and potions are all gated by floor,
      and monsters get both tougher and more numerous as you descend

**Combat**

- [x] Turn-based melee (walk into something to hit it), ranged weapons (`f`),
      and permadeath
- [x] One accuracy/dodge formula shared by everything that fights: the attacker's
      Dexterity plus a roll of the weapon or spell's own "hit-dice", against the
      defender's evasion, with armor soaking a flat amount off whatever lands. A
      fast dagger or a wide Fireball are harder to dodge than a heavy axe or a
      precise Magic Dart, whoever is swinging them

**Character**

- [x] Attributes and leveling — Strength, Dexterity, Intelligence, earned with XP
- [x] Items: weapons, armor and potions, with pickup, equip and drop. Nothing
      is picked up automatically, and a tile holding several things opens a menu
      so you take only what you want, still in a single turn
- [x] Passive HP and mana regeneration, both opt-in per creature rather than
      universal — see Monsters

**Magic**

- [x] Spell schools: at Intelligence 4 you permanently pick one of three paths,
      locking the other two out for the rest of the run. Magic Dart and its
      upgrade Energy Lance stay available whichever you choose
  - Caster — Fireball (slow orb, 3x3 blast that can catch you too), Sandstorm
    (toggled aura that follows you and drains mana), Lightning Bolt (pierces
    every monster along its line)
  - Summoner — Summon Imp, Place Swap (trade places with a minion you can
    see), Raise Dead (turn a corpse into a minion of that species), Summon
    Demon (permanent and much stronger)
  - Combat Mage — Battle Fury (melee damage), Iron Skin (armor), Haste
- [x] Mana, spent per cast and regenerated slowly
- [x] Extra actions per turn: Haste lets you act twice for every turn the dungeon
      takes, while monsters, buff timers and regen all stay on the normal clock.
      It's a plain creature property, not a player perk, so a boss can be given
      the same thing in one table field (`--fast-monsters` shows it from the
      other side)

**Monsters**

- [x] One shared foundation with the player: the same underlying creature, the
      same attack/dodge/damage math, the same per-turn upkeep. Monsters wear
      armor that soaks your hits, swap between carried weapons by range, drink
      potions through the code you do, and drop everything non-natural when they
      die. `x` shows what a fight will cost you before you commit
- [x] AI chases on sight, keeps heading for the last place it saw you after you
      break line of sight, and otherwise wanders — with real A* pathfinding, so
      it routes around walls instead of grinding into corners
- [x] Ranged specialists: Goblin Slinger, and its floor-5+ counterpart Orc
      Archer, snipe without approaching but permanently commit to melee the
      moment you reach them
- [x] Casters: the Goblin Shaman throws the same Magic Dart you can learn, from a
      small mana pool that comes back only slowly — a handful of darts up front,
      then a 5 HP nuisance with claws until it has recovered. Projectiles carry an owner, so one code path resolves yours and
      theirs alike, and neither ever hits its own side
- [x] Bosses: one guaranteed spawn on each floor its row covers, on top of that
      floor's normal monsters. The Orc Warlord (floor 3) is the only thing
      besides you that regenerates, so wounds you don't finish it with heal back
      off — and the only monster carrying a potion

**Minions**

- [x] Summon temporary or permanent allies that draw enemy attention off you,
      follow you through stairs, and are immune to your own spells
- [x] You always know where your own minions are — yours stay on the map even
      out of sight, dimmed. Seeing one is not the same as reaching it, though:
      Place Swap still needs a clear line, so a minion parked behind a wall is
      no escape hatch
- [x] Per-minion command: cycle focus with `o`/`p` or pick one from a roster
      (`m`), then send that minion or the whole pack to attack a target, hold a
      tile, follow you, or hunt on its own
- [x] Minion abilities: a summon can carry spells of its own, spent from its own
      mana and aimed from where it stands, via `z` while it's focused. The Demon's
      Wither Curse is the first — it has a fixed pool and no regeneration, so an
      ability is something you spend a minion on, not a per-turn tool
- [x] Corpses: a slain monster has a chance to leave its body behind, shown as
      `%` on the map and named by `x`. Bodies persist with the floor like any
      other loot. Bosses never leave one — their gear is the reward, and their
      stat line isn't meant to end up on your side — and neither do Skeletons,
      which are animated bones with nothing left to raise
- [x] True necromancy: Raise Dead turns a corpse into a minion of that exact
      species — gear and all, so a raised Orc arrives in Leather Armor swinging
      an Orc Axe. It comes back at 60% of the living creature's HP and rots
      away after 50 turns, so raising is opportunistic borrowed time rather
      than a permanent gain — Summon Demon stays the school's dependable
      standing minion
- [ ] A higher minion cap, now that per-minion control exists

**Interface**

- [x] Sectioned HUD: the dungeon, a message log, and a status sidebar showing
      your stats, every active buff with its turns remaining, and the enemies
      and minions currently in view — each minion with its health, any mana it
      has for abilities, and how many turns before a timed one expires. The map is bigger than its panel, so the
      view scrolls to follow you
- [x] Message log with scrollback (`]`) and repeat coalescing
- [x] Look around (`x`) to inspect any explored tile without spending a turn

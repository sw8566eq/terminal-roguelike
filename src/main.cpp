// Startup and the main loop, and nothing else. Everything this file does is: parse
// argv, open a window, then alternate render_frame() with handle_event() until the
// player quits.
//
// The layers underneath it, innermost first:
//   entity/map/rng      the primitives
//   content/spells/rules  the tables and the numbers
//   actors/projectile/level  the world's data structures
//   game                GameState and the operations on it
//   turn                what happens between one player action and the next
//   render/input        drawing and keys

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <libtcod.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

#include "content.hpp"
#include "game.hpp"
#include "input.hpp"
#include "level.hpp"
#include "render.hpp"
#include "rng.hpp"

int main(int argc, char* argv[]) {
  // Pre-scan for the debug flags that have to be known before any level is generated
  // — start_new_game() below builds floor 1, and --floor=N replays descend(), both well
  // before the ordinary argv loop further down. See g_debug_fast_monsters, and seed_rng()
  // in rng.hpp: seeding after floor 1 already exists would defeat the whole point.
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--fast-monsters") g_debug_fast_monsters = true;
    const std::string seed_prefix = "--seed=";
    if (arg.rfind(seed_prefix, 0) == 0) {
      // Parsed by hand rather than with std::stoul, which *throws* on a non-numeric
      // value and would abort the process — every other debug flag silently ignores a
      // malformed value (--floor=/--level= use atoi), and the README promises that.
      const std::string value = arg.substr(seed_prefix.size());
      bool numeric = !value.empty();
      for (char c : value) {
        if (c < '0' || c > '9') numeric = false;
      }
      if (numeric) seed_rng(static_cast<unsigned int>(std::strtoul(value.c_str(), nullptr, 10)));
    }
  }

  auto console = tcod::Console{SCREEN_WIDTH, SCREEN_HEIGHT};

  // Explicitly pick a font instead of leaving tileset null: with none set, libtcod
  // tries to load a "terminal.png" from disk (which this project doesn't ship, hence
  // the "Error loading font image" warning at startup), then silently falls back to
  // its built-in font anyway, but at whatever tiny default size it picks. Doing it
  // ourselves skips the failed disk lookup, lets us pick a real size, and tries to
  // match the font your terminal would normally show text in.
  auto tileset = load_best_tileset(TILE_SIZE);

  // Configure the context.
  auto params = TCOD_ContextParams{};
  params.console = console.get();  // Derive the window size from the console size.
  params.tileset = tileset.get();
  params.window_title = "Terminal Roguelike";
  params.sdl_window_flags = SDL_WINDOW_RESIZABLE;
  params.vsync = true;
  params.argc = argc;
  params.argv = argv;

  auto context = tcod::Context(params);

  // The entire mutable state of the run. Everything below operates on this rather than
  // on a pile of locals, which is what lets the turn loop, the renderer and the input
  // handlers live in their own translation units (see game.hpp).
  GameState gs;
  gs.player.glyph = '@';
  gs.player.color = tcod::ColorRGB{255, 255, 0};
  gs.player.name = "Player";
  gs.player.is_player = true;
  gs.player.id = allocate_actor_id();

  start_new_game(gs);

  // Debug convenience: `--floor=N` jumps straight to floor N at startup, so testing
  // deep floors doesn't require a long walk down through every floor above it. Not
  // meant for normal play; a missing/malformed value is just silently ignored.
  //
  // `--level=N` spawns the player already at level N, forcing the same Mode::LevelUp
  // prompt (N-1) times in a row so you allocate every point yourself, same as leveling
  // up for real — it calls level_up_once() directly rather than faking XP, so it's the
  // exact core path a big XP reward would also drive if it crossed several thresholds
  // at once (see level_up_once's comment). Combinable with --floor=N.
  //
  // `--reveal` shows every tile/monster/item on the current floor regardless of
  // exploration or FOV (still dimmed if not actually in current sight, matching the
  // remembered-terrain/monster look) — for eyeballing spawns and loot without having
  // to walk the whole floor first. Also debug-only, off by default.
  //
  // `--dump-loot` prints every weapon/armor/potion/monster on the floor reached via
  // --floor=N (floor 1 if that flag's absent) to stdout, then exits before the window
  // ever opens — a scriptable alternative to eyeballing --reveal in the live window,
  // for checking depth-gating (kWeaponTable etc.) actually filters as intended.
  //
  // `--give=<name>[,<name>...]` adds items straight to the carried inventory at
  // startup (not auto-equipped — same "found it, now equip it" flow as picking one up,
  // just skipping the walk), for testing a specific item without grinding to find one.
  // Comma-separated, e.g. --give="Dagger,Potion of Teleportation"; see
  // give_starting_item() above.
  //
  // `--seed=N` pins the shared RNG (see seed_rng() in rng.hpp) so a run is reproducible:
  // the same seed builds the same floors with the same monsters, gear and rolls. Handled
  // in the pre-scan at the top of main(), not here, since floor 1 is generated long
  // before this loop runs. Pairs with --dump-loot to make floor contents diffable.
  bool dump_loot = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--reveal") {
      gs.reveal_all = true;
      continue;
    }
    if (arg == "--dump-loot") {
      dump_loot = true;
      continue;
    }
    if (arg == "--fast-monsters") {
      continue;  // already handled by the pre-scan at the top of main()
    }
    if (arg.rfind("--seed=", 0) == 0) {
      continue;  // likewise — seeding has to happen before floor 1 exists
    }
    const std::string floor_prefix = "--floor=";
    if (arg.rfind(floor_prefix, 0) == 0) {
      int target_floor = std::atoi(arg.c_str() + floor_prefix.size());
      for (int f = 1; f < target_floor; ++f) descend(gs);
      continue;
    }
    const std::string level_prefix = "--level=";
    if (arg.rfind(level_prefix, 0) == 0) {
      int target_level = std::atoi(arg.c_str() + level_prefix.size());
      for (int lv = gs.player.level; lv < target_level; ++lv) level_up_once(gs);
      continue;
    }
    const std::string give_prefix = "--give=";
    if (arg.rfind(give_prefix, 0) == 0) {
      std::string names = arg.substr(give_prefix.size());
      size_t pos = 0;
      while (pos <= names.size()) {
        size_t comma = names.find(',', pos);
        std::string name = names.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!name.empty()) {
          if (give_starting_item(name, gs.player)) {
            add_message(gs, "Debug: added " + name + " to inventory.");
          } else {
            add_message(gs, "Debug: no item named \"" + name + "\" found.");
          }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
      continue;
    }
  }
  // Surfaces the LevelUp prompt if --level= queued any points, same as grant_xp does.
  if (gs.pending_attribute_points > 0) gs.mode = Mode::LevelUp;

  if (dump_loot) {
    Level& level = gs.level();
    std::cout << "Floor " << (gs.current_level + 1) << " loot:\n";
    for (const auto& item : level.items) {
      std::cout << "  weapon: " << item.weapon.name << " (" << describe_weapon(item.weapon) << ")\n";
    }
    for (const auto& armor_item : level.armor_items) {
      std::cout << "  armor: " << armor_item.armor.name << " (" << describe_armor(armor_item.armor) << ")\n";
    }
    for (const auto& ground_potion : level.potions) {
      std::cout << "  potion: " << ground_potion.potion.name << " (" << describe_potion(ground_potion.potion)
                 << ")\n";
    }
    // Monsters carry gear now, and everything non-intrinsic here is what they'll drop —
    // worth printing, since that's a real part of a floor's loot.
    std::cout << "Floor " << (gs.current_level + 1) << " monsters:\n";
    for (const auto& monster : level.monsters) {
      std::cout << "  " << monster.name << " (" << monster.hp << " HP, " << monster.weapon.name << " "
                << describe_weapon(monster.weapon) << ", STR " << monster.strength << ", DEX " << monster.dexterity
                << ", evasion " << monster.evasion;
      if (!monster.armor.is_intrinsic) std::cout << ", " << monster.armor.name;
      // A caster's spells and mana pool are as much a part of "what will this fight cost
      // you" as its weapon is — see the same line in the 'x' look panel. Joined with
      // "/" when there's more than one (the Orc Wizard's Magic Dart/Fireball).
      if (!monster.spell_indices.empty()) {
        std::string casts;
        for (int spell_idx : monster.spell_indices) {
          if (!casts.empty()) casts += "/";
          casts += kSpellTable[static_cast<size_t>(spell_idx)].name;
        }
        std::cout << ", " << casts << " " << monster.mana << "/" << monster.max_mana << " MP";
      }
      std::cout << ")\n";
      for (const auto& w : monster.weapons) std::cout << "      carries weapon: " << w.name << "\n";
      for (const auto& a : monster.armors) std::cout << "      carries armor: " << a.name << "\n";
      for (const auto& p : monster.potions) std::cout << "      carries potion: " << p.name << "\n";
    }
    return 0;
  }

  while (gs.running) {

    render_frame(gs, console);
    context.present(console);

    // --- Input / events ---
    SDL_Event event;
    SDL_WaitEvent(nullptr);  // Sleep until an event arrives (this is a turn-based game; no need to busy-loop).
    while (SDL_PollEvent(&event)) {
      context.convert_event_coordinates(event);

      if (event.type == SDL_EVENT_QUIT) {
        gs.running = false;
        continue;
      }
      if (event.type != SDL_EVENT_KEY_DOWN) continue;

      // Each handler re-fetches its own Level& through gs.level(): descend() can
      // push_back onto `levels`, which may reallocate and would dangle a reference held
      // across more than one queued event in the same batch.
      handle_event(gs, event);
    }
  }

  return 0;
}

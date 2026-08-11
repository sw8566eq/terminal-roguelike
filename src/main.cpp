#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <libtcod.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "actors.hpp"
#include "content.hpp"
#include "game.hpp"
#include "entity.hpp"
#include "level.hpp"
#include "map.hpp"
#include "projectile.hpp"
#include "render.hpp"
#include "rng.hpp"
#include "rules.hpp"
#include "spells.hpp"
#include "turn.hpp"

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
      seed_rng(static_cast<unsigned int>(std::stoul(arg.substr(seed_prefix.size()))));
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

  // Runs after the player's turn: every living monster still adjacent to the player
  // gets to attack. (Movement/chasing AI will plug into this same turn boundary later.)

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
      // A caster's spell and mana pool are as much a part of "what will this fight cost
      // you" as its weapon is — see the same line in the 'x' look panel.
      if (monster.spell_index >= 0) {
        std::cout << ", " << kSpellTable[static_cast<size_t>(monster.spell_index)].name << " " << monster.mana << "/"
                  << monster.max_mana << " MP";
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

      // Re-fetched fresh for every event (not reused from the outer render-time `level`
      // above): descend() can push_back onto `levels`, which may reallocate and would
      // dangle a reference held across more than one queued event in the same batch.
      Level& level = gs.level();

      // Moves command focus to the next (direction=+1) or previous (direction=-1)
      // living minion, in level.monsters order, wrapping around; if focused_minion_id
      // doesn't currently name a living minion (nothing focused yet, or it died),
      // starts from the first (next) or last (prev) instead of wrapping relative to a
      // missing position. Always lands on one specific minion — never "all" — and
      // points the cursor at its current position. Returns false (no-op) if there are
      // no minions at all. Shared by the 'o'/'p' trigger keys from Mode::Playing and
      // by the same keys working *inside* Mode::MinionFocus too, so you can tab
      // straight from planning one minion's order to the next without dropping back
      // to normal play in between — the "turn planner" feel this whole system is for.
      auto cycle_minion_focus = [&](int direction) -> bool {
        std::vector<int> minion_ids;
        for (const auto& m : level.monsters) {
          if (m.allegiance == Allegiance::Player && m.is_alive()) minion_ids.push_back(m.id);
        }
        if (minion_ids.empty()) return false;
        int current = -1;
        for (size_t i = 0; i < minion_ids.size(); ++i) {
          if (minion_ids[i] == gs.focused_minion_id) {
            current = static_cast<int>(i);
            break;
          }
        }
        int next_index;
        if (current < 0) {
          next_index = direction >= 0 ? 0 : static_cast<int>(minion_ids.size()) - 1;
        } else {
          next_index = (current + direction + static_cast<int>(minion_ids.size())) %
                       static_cast<int>(minion_ids.size());
        }
        gs.focused_minion_id = minion_ids[static_cast<size_t>(next_index)];
        gs.commanding_all_minions = false;
        int fi = actor_index_by_id(level.monsters, gs.focused_minion_id);
        gs.target_x = level.monsters[static_cast<size_t>(fi)].x;
        gs.target_y = level.monsters[static_cast<size_t>(fi)].y;
        return true;
      };

      if (gs.mode == Mode::Dead) {
        if (event.key.key == SDLK_ESCAPE) {
          gs.running = false;
        } else {
          start_new_game(gs);
        }
        continue;
      }

      if (gs.mode == Mode::MessageLog) {
        if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_RIGHTBRACKET) {
          gs.mode = Mode::Playing;
        } else if (event.key.key == SDLK_K || event.key.key == SDLK_UP) {
          int visible_rows = SCREEN_HEIGHT - 1;
          int max_scroll = std::max(0, static_cast<int>(gs.message_log.size()) - visible_rows);
          gs.log_scroll = std::min(gs.log_scroll + 1, max_scroll);
        } else if (event.key.key == SDLK_J || event.key.key == SDLK_DOWN) {
          gs.log_scroll = std::max(gs.log_scroll - 1, 0);
        }
        continue;
      }

      if (gs.mode == Mode::Help) {
        // Same unshifted-keycode-plus-modifier check the stairs keys use below, since
        // '?' is Shift+/ on a US layout.
        bool pressed_question =
            event.key.key == SDLK_QUESTION || (event.key.key == SDLK_SLASH && (event.key.mod & SDL_KMOD_SHIFT));
        if (event.key.key == SDLK_ESCAPE || pressed_question) gs.mode = Mode::Playing;
        continue;
      }

      if (gs.mode == Mode::LevelUp) {
        // No menu for this on purpose: just force S/D/I directly, one point at a time.
        // Requires actual Shift+S/D/I (not the bare lowercase letter) since 'd' and 'i'
        // already mean something in normal play — a permanent stat point shouldn't be
        // one stray unshifted keypress away from being spent on the wrong thing.
        bool shift_held = (event.key.mod & SDL_KMOD_SHIFT) != 0;
        if (event.key.key == SDLK_ESCAPE) {
          gs.running = false;
        // Each of these raises the attribute and then applies that point's knock-on
        // ceiling as a delta, rather than recomputing the ceiling from the attribute —
        // the same rule apply_potion() follows, so spending a level-up point while a
        // stat potion is running doesn't quietly cancel the potion. Current HP/mana rise
        // with the ceiling here (unlike a temporary buff, which only lifts the ceiling).
        } else if (shift_held && event.key.key == SDLK_S) {
          gs.player.strength += 1;
          gs.player.max_hp += kHpPerStrength;
          gs.player.hp += kHpPerStrength;
          add_message(gs, "Strength increased to " + std::to_string(gs.player.strength) + "!");
          gs.pending_attribute_points -= 1;
        } else if (shift_held && event.key.key == SDLK_D) {
          // Dexterity is now worth accuracy on every attack as well as evasion — see
          // the combat-formula block at the top of this file.
          gs.player.dexterity += 1;
          gs.player.evasion += kDodgePerDexPoint;
          add_message(gs, "Dexterity increased to " + std::to_string(gs.player.dexterity) + "!");
          gs.pending_attribute_points -= 1;
        } else if (shift_held && event.key.key == SDLK_I) {
          auto known_before = known_spell_indices(gs.player.intelligence, gs.player.chosen_school);
          int mana_delta =
              max_mana_for_intelligence(gs.player.intelligence + 1) - max_mana_for_intelligence(gs.player.intelligence);
          gs.player.intelligence += 1;
          auto known_after = known_spell_indices(gs.player.intelligence, gs.player.chosen_school);
          gs.player.max_mana += mana_delta;
          gs.player.mana += mana_delta;
          add_message(gs, "Intelligence increased to " + std::to_string(gs.player.intelligence) + "!");
          for (int spell_idx : known_after) {
            bool already_known = std::find(known_before.begin(), known_before.end(), spell_idx) != known_before.end();
            if (!already_known) add_message(gs, "You can now cast " + kSpellTable[static_cast<size_t>(spell_idx)].name + "!");
          }
          gs.pending_attribute_points -= 1;
          // The first time Intelligence reaches 4, interrupt with the forced
          // Caster/Summoner pick (Mode::SchoolChoice) instead of falling straight back
          // into Mode::Playing/another LevelUp prompt — known_spell_indices() above
          // deliberately can't have surfaced any class-gated spell yet, since
          // chosen_school is still None at this point (only the shared Magic Dart-style
          // spells show up from this diff; the school's own entry spell is announced
          // from Mode::SchoolChoice's handler once a path is actually picked).
          if (gs.player.chosen_school == SpellSchool::None && gs.player.intelligence >= 4) {
            gs.mode = Mode::SchoolChoice;
          }
        }
        // Guarded so the same keypress that just triggered Mode::SchoolChoice above
        // (the common case — a level-up grants exactly one point, so
        // pending_attribute_points usually also hits 0 right when INT crosses 4)
        // doesn't immediately stomp it back to Mode::Playing before it's ever seen.
        if (gs.mode != Mode::SchoolChoice && gs.pending_attribute_points <= 0) gs.mode = Mode::Playing;
        continue;
      }

      if (gs.mode == Mode::SchoolChoice) {
        // Mandatory, same as LevelUp's own Esc — quitting rather than silently leaving
        // chosen_school stuck at None forever, which would permanently lock out every
        // school's spells.
        bool shift_held = (event.key.mod & SDL_KMOD_SHIFT) != 0;
        if (event.key.key == SDLK_ESCAPE) {
          gs.running = false;
        } else if (shift_held && (event.key.key == SDLK_C || event.key.key == SDLK_U || event.key.key == SDLK_M)) {
          auto known_before = known_spell_indices(gs.player.intelligence, gs.player.chosen_school);
          if (event.key.key == SDLK_C) {
            gs.player.chosen_school = SpellSchool::Caster;
          } else if (event.key.key == SDLK_U) {
            gs.player.chosen_school = SpellSchool::Summoner;
          } else {
            gs.player.chosen_school = SpellSchool::CombatMage;
          }
          auto known_after = known_spell_indices(gs.player.intelligence, gs.player.chosen_school);
          if (gs.player.chosen_school == SpellSchool::Caster) {
            add_message(gs, "You have specialized in Caster magic!");
          } else if (gs.player.chosen_school == SpellSchool::Summoner) {
            add_message(gs, "You have specialized in Summoner magic!");
          } else {
            add_message(gs, "You have specialized in Combat Mage magic!");
          }
          // Same before/after diff idiom as the Shift+I handler above, reused rather
          // than duplicated, so "what's newly known" can't drift between the two call
          // sites — announces the school's own entry spell (Fireball/Raise Skeleton/
          // Battle Fury), the only thing this diff can ever surface given the choice
          // fires the instant Intelligence crosses 4.
          for (int spell_idx : known_after) {
            bool already_known = std::find(known_before.begin(), known_before.end(), spell_idx) != known_before.end();
            if (!already_known) add_message(gs, "You can now cast " + kSpellTable[static_cast<size_t>(spell_idx)].name + "!");
          }
          // Resume any level-up points still queued (e.g. mid-drain from --level=N)
          // instead of always dropping straight back to Playing.
          gs.mode = gs.pending_attribute_points > 0 ? Mode::LevelUp : Mode::Playing;
        }
        continue;
      }

      if (gs.mode == Mode::WeaponMenu) {
        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          // Slot 'a' is always fists; carried weapons fill 'b' onward.
          Weapon chosen;
          bool valid = false;
          if (idx == 0) {
            chosen = kFists;
            valid = true;
          } else if (idx - 1 < gs.player.weapons.size()) {
            chosen = gs.player.weapons[idx - 1];
            gs.player.weapons.erase(gs.player.weapons.begin() + static_cast<long>(idx - 1));
            valid = true;
          }
          if (valid) {
            // Swap the old weapon back into the pack, unless it's an intrinsic one
            // like bare fists, which isn't a real item.
            if (!gs.player.weapon.is_intrinsic) gs.player.weapons.push_back(gs.player.weapon);
            gs.player.weapon = chosen;
            add_message(gs, "You equip the " + chosen.name + ".");
            gs.mode = Mode::Playing;
            end_turn(gs);  // fiddling with gear takes time; adjacent monsters get a free hit
          }
        }
        continue;
      }

      if (gs.mode == Mode::ArmorMenu) {
        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          // Slot 'a' is always "Nothing"; carried armor fills 'b' onward.
          Armor chosen;
          bool valid = false;
          if (idx == 0) {
            chosen = kNoArmor;
            valid = true;
          } else if (idx - 1 < gs.player.armors.size()) {
            chosen = gs.player.armors[idx - 1];
            gs.player.armors.erase(gs.player.armors.begin() + static_cast<long>(idx - 1));
            valid = true;
          }
          if (valid) {
            if (!gs.player.armor.is_intrinsic) gs.player.armors.push_back(gs.player.armor);
            gs.player.armor = chosen;
            add_message(gs, "You equip the " + chosen.name + ".");
            gs.mode = Mode::Playing;
            end_turn(gs);  // fiddling with gear takes time; adjacent monsters get a free hit
          }
        }
        continue;
      }

      if (gs.mode == Mode::PotionMenu) {
        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          if (idx < gs.player.potions.size()) {
            // Same call an Orc Archer makes when it decides to quaff its own Heal
            // Potion — see apply_potion(), where every potion effect is defined once.
            Potion chosen = gs.player.potions[idx];
            gs.player.potions.erase(gs.player.potions.begin() + static_cast<long>(idx));
            apply_potion(gs, gs.player, chosen);
            gs.mode = Mode::Playing;
            end_turn(gs);  // drinking takes a moment; adjacent monsters get a free hit
          }
        }
        continue;
      }

      if (gs.mode == Mode::SpellMenu) {
        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          auto known = known_spell_indices(gs.player.intelligence, gs.player.chosen_school);
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          if (idx < known.size()) {
            int spell_idx = known[idx];
            const Spell& spell = kSpellTable[static_cast<size_t>(spell_idx)];
            if (spell.is_toggle) {
              if (gs.active_toggle_spell == spell_idx) {
                // Turning off is always free — no mana cost, but still takes the turn,
                // same as every other spell-menu action.
                gs.active_toggle_spell = -1;
                add_message(gs, "Your " + spell.name + " dissipates.");
                gs.mode = Mode::Playing;
                end_turn(gs);
              } else if (gs.player.mana < spell.mana_cost) {
                add_message(gs, "Not enough mana to cast " + spell.name + ".");
                gs.mode = Mode::Playing;  // free cancel, no turn spent
              } else {
                gs.player.mana -= spell.mana_cost;
                add_message(gs, "You summon a " + spell.name + " around yourself!");
                gs.mode = Mode::Playing;
                end_turn(gs);  // this turn only pays the flat activation cost above
                gs.active_toggle_spell = spell_idx;  // set after end_turn(gs), so the
                                                   // per-turn tick starts next turn
              }
            } else if (spell.is_summon) {
              int spawn_x, spawn_y;
              if (count_minions(level.monsters) >= kMaxMinions) {
                add_message(gs, "You can't command any more minions right now.");
                gs.mode = Mode::Playing;  // free cancel, no turn spent
              } else if (gs.player.mana < spell.mana_cost) {
                add_message(gs, "Not enough mana to cast " + spell.name + ".");
                gs.mode = Mode::Playing;  // free cancel, no turn spent
              } else if (!free_adjacent_tile(level.map, level.monsters, gs.player.x, gs.player.y, spawn_x, spawn_y)) {
                add_message(gs, "There's no room to summon here!");
                gs.mode = Mode::Playing;  // free cancel, no turn spent
              } else {
                gs.player.mana -= spell.mana_cost;
                const MinionTemplate& tmpl = kMinionTable[static_cast<size_t>(spell.summon_template_index)];
                Actor minion = spawn_minion(tmpl, spawn_x, spawn_y);
                // Defaults to Aggressive — a fresh recruit engages anything hostile it
                // can see rather than passively waiting for something to wander into
                // its own reach — but joins the pack's current stance instead if an
                // existing minion is already off attacking something specific, so the
                // new recruit piles onto the same fight rather than going its own way
                // (orders are pack-wide in Phase 1, see the `m` menu).
                minion.order = MinionOrder::Aggressive;
                for (const auto& existing : level.monsters) {
                  if (existing.allegiance == Allegiance::Player && existing.is_alive() &&
                      existing.order == MinionOrder::AttackTarget) {
                    minion.order = MinionOrder::AttackTarget;
                    minion.attack_target_id = existing.attack_target_id;
                    break;
                  }
                }
                level.monsters.push_back(minion);
                add_message(gs, "You raise a " + tmpl.name + " to fight for you!");
                gs.mode = Mode::Playing;
                end_turn(gs);
              }
            } else if (spell.is_melee_buff) {
              if (gs.player.mana < spell.mana_cost) {
                add_message(gs, "Not enough mana to cast " + spell.name + ".");
                gs.mode = Mode::Playing;  // free cancel, no turn spent
              } else {
                gs.player.mana -= spell.mana_cost;
                // Refresh-not-stack, same idiom apply_potion() uses for STR/DEX/INT.
                if (gs.player.temp_melee_damage_turns <= 0) gs.player.temp_melee_damage_bonus = spell.buff_amount;
                gs.player.temp_melee_damage_turns = spell.buff_turns;
                add_message(gs, "Your strikes grow fiercer! Melee damage +" + std::to_string(spell.buff_amount) +
                            " for " + std::to_string(spell.buff_turns) + " turns.");
                gs.mode = Mode::Playing;
                end_turn(gs);
              }
            } else if (spell.is_armor_buff) {
              if (gs.player.mana < spell.mana_cost) {
                add_message(gs, "Not enough mana to cast " + spell.name + ".");
                gs.mode = Mode::Playing;  // free cancel, no turn spent
              } else {
                gs.player.mana -= spell.mana_cost;
                if (gs.player.temp_armor_turns <= 0) gs.player.temp_armor_bonus = spell.buff_amount;
                gs.player.temp_armor_turns = spell.buff_turns;
                add_message(gs, "Your skin hardens! Armor +" + std::to_string(spell.buff_amount) + " for " +
                            std::to_string(spell.buff_turns) + " turns.");
                gs.mode = Mode::Playing;
                end_turn(gs);
              }
            } else if (spell.is_haste_buff) {
              if (gs.player.mana < spell.mana_cost) {
                add_message(gs, "Not enough mana to cast " + spell.name + ".");
                gs.mode = Mode::Playing;  // free cancel, no turn spent
              } else {
                gs.player.mana -= spell.mana_cost;
                add_message(gs, "You blur into motion! +" + std::to_string(spell.buff_amount) +
                            " action per turn for " + std::to_string(spell.buff_turns) + " turns.");
                gs.mode = Mode::Playing;
                // The buff is applied *after* end_turn(gs), so casting Haste costs a whole
                // turn like any other spell instead of immediately refunding itself as a
                // free action. Exactly the reason active_toggle_spell is set after
                // end_turn(gs) when a toggle spell is switched on — otherwise the cheapest
                // way to use the spell would be to keep re-casting it.
                end_turn(gs);
                if (gs.player.temp_extra_actions_turns <= 0) gs.player.temp_extra_actions_bonus = spell.buff_amount;
                gs.player.temp_extra_actions_turns = spell.buff_turns;  // refresh-not-stack, as above
              }
            } else if (spell.is_swap) {
              gs.casting_spell_index = spell_idx;
              // Auto-aim at the closest minion in range (no FOV requirement — see
              // closest_own_minion()), else fall back to the player's own tile; Enter
              // will just reject the cast with a message if nothing's actually there.
              int auto_id = closest_own_minion(level.monsters, gs.player, spell.range);
              int auto_idx = actor_index_by_id(level.monsters, auto_id);
              if (auto_idx >= 0) {
                gs.target_x = level.monsters[static_cast<size_t>(auto_idx)].x;
                gs.target_y = level.monsters[static_cast<size_t>(auto_idx)].y;
              } else {
                gs.target_x = gs.player.x;
                gs.target_y = gs.player.y;
              }
              gs.mode = Mode::Targeting;
            } else {
              gs.casting_spell_index = spell_idx;
              // Auto-aim at the most recently targeted hostile if it still qualifies,
              // else the closest qualifying one, else fall back to the player's own
              // tile (the old default) — see auto_target_hostile().
              int auto_id = auto_target_hostile(level.monsters, gs.player, level.map, gs.last_target_id, spell.range);
              int auto_idx = actor_index_by_id(level.monsters, auto_id);
              if (auto_idx >= 0) {
                gs.target_x = level.monsters[static_cast<size_t>(auto_idx)].x;
                gs.target_y = level.monsters[static_cast<size_t>(auto_idx)].y;
              } else {
                gs.target_x = gs.player.x;
                gs.target_y = gs.player.y;
              }
              gs.mode = Mode::Targeting;
            }
          }
        }
        continue;
      }

      if (gs.mode == Mode::MinionRoster) {
        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
          continue;
        }
        if (event.key.key == SDLK_A && (event.key.mod & SDL_KMOD_SHIFT) != 0) {
          // Shift+A always means "All", regardless of which letter it actually landed
          // on this frame (that shifts with the pack's current size) — a fast path so
          // you don't have to read the list to find the right letter every time.
          gs.commanding_all_minions = true;
          gs.target_x = gs.player.x;
          gs.target_y = gs.player.y;
          gs.mode = Mode::MinionFocus;
          continue;
        }
        if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          // Same ordering as the roster's render: a letter per living minion, in
          // level.monsters order ("All" is the fixed Shift+A hotkey above, not a
          // letter in this range — see the check above this one).
          std::vector<int> minion_ids;
          for (const auto& m : level.monsters) {
            if (m.allegiance == Allegiance::Player && m.is_alive()) minion_ids.push_back(m.id);
          }
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          if (idx < minion_ids.size()) {
            gs.focused_minion_id = minion_ids[idx];
            gs.commanding_all_minions = false;
            int fi = actor_index_by_id(level.monsters, gs.focused_minion_id);
            gs.target_x = level.monsters[static_cast<size_t>(fi)].x;
            gs.target_y = level.monsters[static_cast<size_t>(fi)].y;
            gs.mode = Mode::MinionFocus;
          }
        }
        continue;
      }

      if (gs.mode == Mode::MinionFocus) {
        // Applies `fn` to every currently-commanded minion — all of them if this
        // session came from the roster's "All", otherwise just the one named by
        // focused_minion_id. Shared by F (Follow) and Enter (Attack/Hold) below so
        // the "who does this apply to" logic can't drift between the two.
        auto for_each_commanded_minion = [&](auto&& fn) {
          int count = 0;
          if (gs.commanding_all_minions) {
            for (auto& m : level.monsters) {
              if (m.allegiance == Allegiance::Player && m.is_alive()) {
                fn(m);
                ++count;
              }
            }
          } else {
            int fi = actor_index_by_id(level.monsters, gs.focused_minion_id);
            if (fi >= 0) {
              fn(level.monsters[static_cast<size_t>(fi)]);
              count = 1;
            }
          }
          return count;
        };

        bool shift_held = (event.key.mod & SDL_KMOD_SHIFT) != 0;
        if (event.key.key == SDLK_ESCAPE || (event.key.key == SDLK_P && shift_held)) {
          // Esc just backs out of this one planning action; Shift+P additionally
          // resets cycle position, so the next 'o'/'p' starts over from the top —
          // "focusing back on the player instantly."
          if (event.key.key == SDLK_P) gs.focused_minion_id = -1;
          gs.commanding_all_minions = false;
          gs.mode = Mode::Playing;
          continue;
        }
        if (event.key.key == SDLK_O || (event.key.key == SDLK_P && !shift_held)) {
          // Tab straight to the next/previous minion without dropping back to normal
          // play in between — plan one, tab, plan the next, same as 'o'/'p' do from
          // Mode::Playing (see cycle_minion_focus above), just without leaving this mode.
          if (!cycle_minion_focus(event.key.key == SDLK_O ? 1 : -1)) {
            add_message(gs, "You have no minions to command.");
            gs.mode = Mode::Playing;
          }
          continue;
        }
        if (event.key.key == SDLK_F) {
          int ordered = for_each_commanded_minion([](Actor& m) { m.order = MinionOrder::Follow; });
          add_message(gs, ordered == 1 ? "Your minion returns to your side." : "Your minions return to your side.");
          gs.commanding_all_minions = false;
          gs.mode = Mode::Playing;
          continue;
        }
        if (event.key.key == SDLK_G) {
          // Aggressive: same as Follow, but engages anything hostile it can see
          // instead of waiting for something to wander into its own reach — see the
          // MinionOrder doc comment in entity.hpp.
          int ordered = for_each_commanded_minion([](Actor& m) { m.order = MinionOrder::Aggressive; });
          add_message(gs, ordered == 1 ? "Your minion goes on the offensive." : "Your minions go on the offensive.");
          gs.commanding_all_minions = false;
          gs.mode = Mode::Playing;
          continue;
        }
        if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
          int hostile_hit = hostile_monster_at(level.monsters, gs.target_x, gs.target_y);
          if (hostile_hit >= 0) {
            int target_id = level.monsters[static_cast<size_t>(hostile_hit)].id;
            std::string target_name = level.monsters[static_cast<size_t>(hostile_hit)].name;
            int ordered = for_each_commanded_minion([&](Actor& m) {
              m.order = MinionOrder::AttackTarget;
              m.attack_target_id = target_id;
            });
            add_message(gs, (ordered == 1 ? "Your minion attacks the " : "Your minions attack the ") + target_name +
                        "!");
            gs.commanding_all_minions = false;
            gs.mode = Mode::Playing;
            continue;
          }
          if (gs.target_x == gs.player.x && gs.target_y == gs.player.y) {
            // Targeting yourself reads as "come back to me" — the same Follow order
            // 'F' gives directly, just reachable without moving the cursor off your
            // own tile first. (Previously this fell through to the tile_free/Hold
            // check below, which — since the player isn't in level.monsters and thus
            // invisible to monster_at() — would silently issue a Hold planted on the
            // player's exact tile instead of anything resembling a rejection.)
            int ordered = for_each_commanded_minion([](Actor& m) { m.order = MinionOrder::Follow; });
            add_message(gs, ordered == 1 ? "Your minion returns to your side." : "Your minions return to your side.");
            gs.commanding_all_minions = false;
            gs.mode = Mode::Playing;
            continue;
          }
          bool tile_free = level.map.is_walkable(gs.target_x, gs.target_y) &&
                            monster_at(level.monsters, gs.target_x, gs.target_y) < 0;
          if (tile_free) {
            int hx = gs.target_x;
            int hy = gs.target_y;
            int ordered = for_each_commanded_minion([&](Actor& m) {
              m.order = MinionOrder::Hold;
              m.hold_x = hx;
              m.hold_y = hy;
            });
            add_message(gs, ordered == 1 ? "Your minion holds position." : "Your minions hold position.");
            gs.commanding_all_minions = false;
            gs.mode = Mode::Playing;
            continue;
          }
          add_message(gs, "You can't send them there.");
          continue;  // stay in this mode, no turn spent — try again
        }

        // Movement keys move the cursor — unlike a spell's Targeting, there's no
        // range limit here (a minion will path however far it needs to), just the
        // map bounds.
        int tdx = 0;
        int tdy = 0;
        switch (event.key.key) {
          case SDLK_UP:
          case SDLK_K:
            tdy = -1;
            break;
          case SDLK_DOWN:
          case SDLK_J:
            tdy = 1;
            break;
          case SDLK_LEFT:
          case SDLK_H:
            tdx = -1;
            break;
          case SDLK_RIGHT:
          case SDLK_L:
            tdx = 1;
            break;
          case SDLK_Y:
            tdx = -1;
            tdy = -1;
            break;
          case SDLK_U:
            tdx = 1;
            tdy = -1;
            break;
          case SDLK_B:
            tdx = -1;
            tdy = 1;
            break;
          case SDLK_N:
            tdx = 1;
            tdy = 1;
            break;
          default:
            break;
        }
        if (tdx != 0 || tdy != 0) {
          int nx = gs.target_x + tdx;
          int ny = gs.target_y + tdy;
          if (level.map.in_bounds(nx, ny)) {
            gs.target_x = nx;
            gs.target_y = ny;
          }
        }
        continue;
      }

      if (gs.mode == Mode::Targeting) {
        const Spell& spell = kSpellTable[static_cast<size_t>(gs.casting_spell_index)];

        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
          continue;
        }
        if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
          if (gs.player.mana < spell.mana_cost) {
            add_message(gs, "Not enough mana to cast " + spell.name + ".");
            gs.mode = Mode::Playing;  // free cancel, same as Esc — no turn spent
            continue;
          }

          if (spell.is_swap) {
            int minion_index = own_minion_at(level.monsters, gs.target_x, gs.target_y);
            if (minion_index < 0) {
              add_message(gs, "There's no minion there to swap places with.");
              gs.mode = Mode::Playing;  // free cancel, same as Esc — no turn spent
              continue;
            }
            Actor& minion = level.monsters[static_cast<size_t>(minion_index)];
            std::swap(gs.player.x, minion.x);
            std::swap(gs.player.y, minion.y);
            // Not an incremental step, so (unlike normal movement) FOV needs an
            // explicit recompute — same as the Potion of Teleportation's effect.
            level.map.update_fov(gs.player.x, gs.player.y, FOV_RADIUS);
            gs.player.mana -= spell.mana_cost;
            add_message(gs, "You swap places with your " + minion.name + ".");
            gs.mode = Mode::Playing;
            end_turn(gs);
            continue;
          }

          // Remember what's under the cursor now, before firing, so the next time
          // Targeting/RangedAttack opens it re-aims at the same monster (see
          // auto_target_hostile()). Left unchanged if the shot is aimed at empty
          // ground (e.g. an AoE spell dropped on open floor).
          int hit_index = hostile_monster_at(level.monsters, gs.target_x, gs.target_y);
          if (hit_index >= 0) gs.last_target_id = level.monsters[static_cast<size_t>(hit_index)].id;

          // Any tile is a legal target now: the spell travels and resolves against
          // whatever (if anything) it actually reaches, not necessarily the cursor tile.
          Projectile proj;
          proj.path = trace_path(gs.player.x, gs.player.y, gs.target_x, gs.target_y);
          proj.speed = spell.speed;
          proj.dice_count = spell.dice_count;
          proj.dice_sides = spell.dice_sides;
          proj.hit_dice_count = spell.hit_dice_count;
          proj.hit_dice_sides = spell.hit_dice_sides;
          proj.aoe_radius = spell.aoe_radius;
          proj.pierces = spell.pierces;
          proj.prev_x = gs.player.x;  // seeds the "last open tile" for an immediate wall hit
          proj.prev_y = gs.player.y;
          // Locked in now, not re-read when it lands. Temporary INT (from a Potion of
          // Intelligence) boosts this the same as permanent INT would — only spell
          // *unlocking* (known_spell_indices, above) ignores the temporary bonus.
          proj.bonus = (gs.player.intelligence + gs.player.temp_int_bonus) / 3;
          // A spell's accuracy is built the same way a weapon swing's is: the caster's
          // Dexterity term plus the spell's own hit-dice, rolled on impact. Locking the
          // Dexterity half in here (rather than reading it when the projectile lands
          // several turns later) is what makes a slow Fireball as accurate as the moment
          // it was thrown.
          proj.accuracy_bonus = (gs.player.dexterity + gs.player.temp_dex_bonus) * kAccuracyPerDexPoint;
          proj.name = spell.name;
          proj.glyph = spell.glyph;
          proj.color = spell.color;
          // Matches Projectile's defaults, but set explicitly: now that a monster can
          // fire one too, "who owns this" shouldn't be something a reader has to infer
          // from a default.
          proj.owner_allegiance = Allegiance::Player;
          proj.owner_is_player = true;
          level.projectiles.push_back(proj);
          gs.player.mana -= spell.mana_cost;

          add_message(gs, "You cast " + spell.name + ".");
          gs.mode = Mode::Playing;
          end_turn(gs);  // advance_projectiles() may resolve this immediately for fast spells
          continue;
        }

        // Movement keys move the targeting cursor instead of the player.
        int tdx = 0;
        int tdy = 0;
        switch (event.key.key) {
          case SDLK_UP:
          case SDLK_K:
            tdy = -1;
            break;
          case SDLK_DOWN:
          case SDLK_J:
            tdy = 1;
            break;
          case SDLK_LEFT:
          case SDLK_H:
            tdx = -1;
            break;
          case SDLK_RIGHT:
          case SDLK_L:
            tdx = 1;
            break;
          case SDLK_Y:
            tdx = -1;
            tdy = -1;
            break;
          case SDLK_U:
            tdx = 1;
            tdy = -1;
            break;
          case SDLK_B:
            tdx = -1;
            tdy = 1;
            break;
          case SDLK_N:
            tdx = 1;
            tdy = 1;
            break;
          default:
            break;
        }
        if (tdx != 0 || tdy != 0) {
          int nx = gs.target_x + tdx;
          int ny = gs.target_y + tdy;
          int rdx = nx - gs.player.x;
          int rdy = ny - gs.player.y;
          bool in_range = rdx * rdx + rdy * rdy <= spell.range * spell.range;
          if (level.map.in_bounds(nx, ny) && in_range) {
            gs.target_x = nx;
            gs.target_y = ny;
          }
        }
        continue;
      }

      if (gs.mode == Mode::RangedAttack) {
        const Weapon& weapon = gs.player.weapon;

        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
          continue;
        }
        if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
          // Same shape as a spell cast (Mode::Targeting above), just sourced from the
          // weapon instead of a Spell and with no mana cost. Both bonuses come from the
          // shared helpers, so a fired Bow lands with exactly the accuracy and damage
          // that same Bow would have if a monster were shooting it at you:
          // damage_bonus_for() gives a ranged weapon the Dexterity bonus, and
          // accuracy_bonus carries the caster's Dexterity accuracy term (the weapon's
          // own hit-dice are still rolled fresh on impact).

          // Remember what's under the cursor now, before firing — see the Targeting
          // handler's identical comment above.
          int hit_index = hostile_monster_at(level.monsters, gs.target_x, gs.target_y);
          if (hit_index >= 0) gs.last_target_id = level.monsters[static_cast<size_t>(hit_index)].id;

          Projectile proj;
          proj.path = trace_path(gs.player.x, gs.player.y, gs.target_x, gs.target_y);
          proj.speed = kInstantSpellSpeed;
          proj.dice_count = weapon.dice_count;
          proj.dice_sides = weapon.dice_sides;
          proj.hit_dice_count = weapon.hit_dice_count;
          proj.hit_dice_sides = weapon.hit_dice_sides;
          proj.prev_x = gs.player.x;
          proj.prev_y = gs.player.y;
          proj.bonus = weapon.bonus + damage_bonus_for(gs.player, weapon);
          proj.accuracy_bonus = (gs.player.dexterity + gs.player.temp_dex_bonus) * kAccuracyPerDexPoint;
          proj.name = weapon.name;
          proj.glyph = '-';
          proj.color = tcod::ColorRGB{200, 170, 100};
          proj.owner_allegiance = Allegiance::Player;  // explicit, see the spell cast above
          proj.owner_is_player = true;
          level.projectiles.push_back(proj);

          add_message(gs, "You fire your " + weapon.name + ".");
          gs.mode = Mode::Playing;
          end_turn(gs);
          continue;
        }

        // Movement keys move the targeting cursor instead of the player, clamped to
        // the weapon's own range — same circular-radius shape a spell's Targeting uses.
        int tdx = 0;
        int tdy = 0;
        switch (event.key.key) {
          case SDLK_UP:
          case SDLK_K:
            tdy = -1;
            break;
          case SDLK_DOWN:
          case SDLK_J:
            tdy = 1;
            break;
          case SDLK_LEFT:
          case SDLK_H:
            tdx = -1;
            break;
          case SDLK_RIGHT:
          case SDLK_L:
            tdx = 1;
            break;
          case SDLK_Y:
            tdx = -1;
            tdy = -1;
            break;
          case SDLK_U:
            tdx = 1;
            tdy = -1;
            break;
          case SDLK_B:
            tdx = -1;
            tdy = 1;
            break;
          case SDLK_N:
            tdx = 1;
            tdy = 1;
            break;
          default:
            break;
        }
        if (tdx != 0 || tdy != 0) {
          int nx = gs.target_x + tdx;
          int ny = gs.target_y + tdy;
          int rdx = nx - gs.player.x;
          int rdy = ny - gs.player.y;
          bool in_range = rdx * rdx + rdy * rdy <= weapon.attack_range * weapon.attack_range;
          if (level.map.in_bounds(nx, ny) && in_range) {
            gs.target_x = nx;
            gs.target_y = ny;
          }
        }
        continue;
      }

      if (gs.mode == Mode::Look) {
        if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_X) {
          gs.mode = Mode::Playing;
          continue;
        }

        // Movement keys move the cursor — no range limit and no confirm action, just
        // look around and back out with Esc/'x' (no turn spent either way).
        int tdx = 0;
        int tdy = 0;
        switch (event.key.key) {
          case SDLK_UP:
          case SDLK_K:
            tdy = -1;
            break;
          case SDLK_DOWN:
          case SDLK_J:
            tdy = 1;
            break;
          case SDLK_LEFT:
          case SDLK_H:
            tdx = -1;
            break;
          case SDLK_RIGHT:
          case SDLK_L:
            tdx = 1;
            break;
          case SDLK_Y:
            tdx = -1;
            tdy = -1;
            break;
          case SDLK_U:
            tdx = 1;
            tdy = -1;
            break;
          case SDLK_B:
            tdx = -1;
            tdy = 1;
            break;
          case SDLK_N:
            tdx = 1;
            tdy = 1;
            break;
          default:
            break;
        }
        if (tdx != 0 || tdy != 0) {
          int nx = gs.target_x + tdx;
          int ny = gs.target_y + tdy;
          if (level.map.in_bounds(nx, ny)) {
            gs.target_x = nx;
            gs.target_y = ny;
          }
        }
        continue;
      }

      if (gs.mode == Mode::Drop) {
        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          auto slots = drop_slots(gs.player);
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          if (idx < slots.size()) {
            const ItemSlot& slot = slots[idx];
            std::string dropped_name;
            if (slot.kind == ItemKind::Weapon) {
              Weapon dropped;
              if (slot.index == -1) {
                dropped = gs.player.weapon;
                gs.player.weapon = kFists;
              } else {
                size_t inv_idx = static_cast<size_t>(slot.index);
                dropped = gs.player.weapons[inv_idx];
                gs.player.weapons.erase(gs.player.weapons.begin() + static_cast<long>(inv_idx));
              }
              level.items.push_back(GroundItem{gs.player.x, gs.player.y, dropped});
              dropped_name = dropped.name;
            } else if (slot.kind == ItemKind::Armor) {
              Armor dropped;
              if (slot.index == -1) {
                dropped = gs.player.armor;
                gs.player.armor = kNoArmor;
              } else {
                size_t inv_idx = static_cast<size_t>(slot.index);
                dropped = gs.player.armors[inv_idx];
                gs.player.armors.erase(gs.player.armors.begin() + static_cast<long>(inv_idx));
              }
              level.armor_items.push_back(GroundArmor{gs.player.x, gs.player.y, dropped});
              dropped_name = dropped.name;
            } else {
              size_t inv_idx = static_cast<size_t>(slot.index);
              Potion dropped = gs.player.potions[inv_idx];
              gs.player.potions.erase(gs.player.potions.begin() + static_cast<long>(inv_idx));
              level.potions.push_back(GroundPotion{gs.player.x, gs.player.y, dropped});
              dropped_name = dropped.name;
            }
            add_message(gs, "You drop the " + dropped_name + ".");
            gs.mode = Mode::Playing;
            end_turn(gs);
          }
        }
        continue;
      }

      // Mode::Playing
      if (event.key.key == SDLK_ESCAPE) {
        gs.running = false;
        continue;
      }

      if (event.key.key == SDLK_W) {
        gs.mode = Mode::WeaponMenu;
        continue;
      }
      if (event.key.key == SDLK_A) {
        gs.mode = Mode::ArmorMenu;
        continue;
      }
      if (event.key.key == SDLK_D) {
        gs.mode = Mode::Drop;
        continue;
      }
      if (event.key.key == SDLK_Q) {
        gs.mode = Mode::PotionMenu;
        continue;
      }
      if (event.key.key == SDLK_G) {
        // Picks up *everything* on the player's current tile (no auto-pickup on step),
        // each as its own message rather than one combined line. This used to take only
        // the first item of each kind, which was fine when the floor was the only source
        // of loot — but a dead monster now drops its whole pack on one tile (an Orc
        // Archer leaves both a Short Bow and a Short Sword), and leaving half of it
        // behind with no indication anything remained was just confusing. One turn
        // total, however much is here.
        //
        // Each loop walks backwards so erasing the current element can't shift an
        // unvisited one out from under the index.
        bool picked_up_anything = false;
        for (int i = static_cast<int>(level.items.size()) - 1; i >= 0; --i) {
          const GroundItem& ground = level.items[static_cast<size_t>(i)];
          if (ground.x != gs.player.x || ground.y != gs.player.y) continue;
          add_message(gs, "You pick up a " + ground.weapon.name + ". Press 'w' to equip.");
          gs.player.weapons.push_back(ground.weapon);
          level.items.erase(level.items.begin() + i);
          picked_up_anything = true;
        }
        for (int i = static_cast<int>(level.armor_items.size()) - 1; i >= 0; --i) {
          const GroundArmor& ground = level.armor_items[static_cast<size_t>(i)];
          if (ground.x != gs.player.x || ground.y != gs.player.y) continue;
          add_message(gs, "You pick up a " + ground.armor.name + ". Press 'a' to equip.");
          gs.player.armors.push_back(ground.armor);
          level.armor_items.erase(level.armor_items.begin() + i);
          picked_up_anything = true;
        }
        for (int i = static_cast<int>(level.potions.size()) - 1; i >= 0; --i) {
          const GroundPotion& ground = level.potions[static_cast<size_t>(i)];
          if (ground.x != gs.player.x || ground.y != gs.player.y) continue;
          add_message(gs, "You pick up a " + ground.potion.name + ". Press 'q' to drink.");
          gs.player.potions.push_back(ground.potion);
          level.potions.erase(level.potions.begin() + i);
          picked_up_anything = true;
        }
        if (picked_up_anything) {
          end_turn(gs);
        } else {
          add_message(gs, "There's nothing here to pick up.");
        }
        continue;
      }
      if (event.key.key == SDLK_Z) {
        gs.mode = Mode::SpellMenu;
        continue;
      }
      if (event.key.key == SDLK_F) {
        // Fire the equipped weapon at range — only meaningful for a ranged weapon
        // (Weapon::attack_range > 1, e.g. Bow); a melee weapon still only attacks by
        // bumping into an adjacent monster.
        if (gs.player.weapon.attack_range <= 1) {
          add_message(gs, "Your " + gs.player.weapon.name + " isn't a ranged weapon.");
        } else {
          // Same auto-aim as SpellMenu -> Targeting above, see auto_target_hostile().
          int auto_id = auto_target_hostile(level.monsters, gs.player, level.map, gs.last_target_id,
                                             gs.player.weapon.attack_range);
          int auto_idx = actor_index_by_id(level.monsters, auto_id);
          if (auto_idx >= 0) {
            gs.target_x = level.monsters[static_cast<size_t>(auto_idx)].x;
            gs.target_y = level.monsters[static_cast<size_t>(auto_idx)].y;
          } else {
            gs.target_x = gs.player.x;
            gs.target_y = gs.player.y;
          }
          gs.mode = Mode::RangedAttack;
        }
        continue;
      }
      if (event.key.key == SDLK_M) {
        if (count_minions(level.monsters) == 0) {
          add_message(gs, "You have no minions to command.");
        } else {
          gs.mode = Mode::MinionRoster;
        }
        continue;
      }
      // 'o'/'p' cycle command focus straight to the next/previous minion (skipping
      // the roster menu — a faster path for the same thing), landing in
      // Mode::MinionFocus with the cursor on that minion. Shift+P resets focus
      // without opening anything — see Mode::MinionFocus's own handling of these
      // same keys for tabbing between minions without leaving that mode in between.
      if (event.key.key == SDLK_O || event.key.key == SDLK_P) {
        bool shift_held = (event.key.mod & SDL_KMOD_SHIFT) != 0;
        if (event.key.key == SDLK_P && shift_held) {
          gs.focused_minion_id = -1;
          continue;
        }
        if (!cycle_minion_focus(event.key.key == SDLK_O ? 1 : -1)) {
          add_message(gs, "You have no minions to command.");
        } else {
          gs.mode = Mode::MinionFocus;
        }
        continue;
      }
      if (event.key.key == SDLK_RIGHTBRACKET) {
        gs.mode = Mode::MessageLog;
        gs.log_scroll = 0;  // always open showing the most recent messages
        continue;
      }
      if (event.key.key == SDLK_X) {
        // Starts the look cursor on the player's own tile, same as Targeting/
        // MinionFocus do — free to open/close, no turn spent either way.
        gs.mode = Mode::Look;
        gs.target_x = gs.player.x;
        gs.target_y = gs.player.y;
        continue;
      }
      // '?' is Shift+/ on a US layout, so check both the dedicated keycode and the
      // unshifted one with the modifier set — same pattern the stairs keys use below.
      if (event.key.key == SDLK_QUESTION ||
          (event.key.key == SDLK_SLASH && (event.key.mod & SDL_KMOD_SHIFT))) {
        gs.mode = Mode::Help;
        continue;
      }
      // SDL reports keycodes for the *unshifted* key on a US layout, so Shift+Period
      // arrives as SDLK_PERIOD with the shift modifier set, not SDLK_GREATER — check
      // both forms so '>' / '<' work regardless of how the layout reports it.
      bool pressed_stairs_down =
          event.key.key == SDLK_GREATER || (event.key.key == SDLK_PERIOD && (event.key.mod & SDL_KMOD_SHIFT));
      bool pressed_stairs_up =
          event.key.key == SDLK_LESS || (event.key.key == SDLK_COMMA && (event.key.mod & SDL_KMOD_SHIFT));

      // Taking stairs costs a turn like any other action. end_turn(gs) runs *before* the
      // transition, so the floor you're leaving gets one parting action — anything
      // adjacent to the stairs gets a swing in as you go, rather than the stairs being a
      // free escape from a losing fight. That also means you can die on the way out,
      // hence the mode check before actually moving floors.
      //
      // Deliberately called here rather than inside descend()/ascend(): the --floor=N
      // debug flag replays descend() in a loop at startup, and running a full turn of
      // monster AI on every intermediate floor before the game even opens would be
      // wrong.
      if (pressed_stairs_down) {
        if (gs.player.x == level.stairs_down_x && gs.player.y == level.stairs_down_y) {
          end_turn(gs);
          if (gs.mode != Mode::Dead) descend(gs);
        } else {
          add_message(gs, "There are no stairs down here.");
        }
        continue;
      }
      if (pressed_stairs_up) {
        if (level.has_stairs_up && gs.player.x == level.entry_x && gs.player.y == level.entry_y) {
          end_turn(gs);
          if (gs.mode != Mode::Dead) ascend(gs);
        } else {
          add_message(gs, "There are no stairs up here.");
        }
        continue;
      }
      // Plain '.' (no shift, which is claimed above for '>') passes the turn without
      // moving or attacking — handy for watching what monsters do on their own.
      if (event.key.key == SDLK_PERIOD && !(event.key.mod & SDL_KMOD_SHIFT)) {
        add_message(gs, "You wait.");
        end_turn(gs);
        continue;
      }

      int dx = 0;
      int dy = 0;
      switch (event.key.key) {
        case SDLK_UP:
        case SDLK_K:
          dy = -1;
          break;
        case SDLK_DOWN:
        case SDLK_J:
          dy = 1;
          break;
        case SDLK_LEFT:
        case SDLK_H:
          dx = -1;
          break;
        case SDLK_RIGHT:
        case SDLK_L:
          dx = 1;
          break;
        // Vim-style diagonals: y/u/b/n for up-left/up-right/down-left/down-right.
        case SDLK_Y:
          dx = -1;
          dy = -1;
          break;
        case SDLK_U:
          dx = 1;
          dy = -1;
          break;
        case SDLK_B:
          dx = -1;
          dy = 1;
          break;
        case SDLK_N:
          dx = 1;
          dy = 1;
          break;
        default:
          break;
      }
      if (dx == 0 && dy == 0) continue;

      int new_x = gs.player.x + dx;
      int new_y = gs.player.y + dy;

      int target_index = -1;
      for (size_t i = 0; i < level.monsters.size(); ++i) {
        if (level.monsters[i].x == new_x && level.monsters[i].y == new_y) {
          target_index = static_cast<int>(i);
          break;
        }
      }

      if (target_index >= 0 && level.monsters[static_cast<size_t>(target_index)].allegiance == Allegiance::Player) {
        // Bump into your own minion: swap places instead of attacking it — you're
        // squeezing past an ally, not fighting one.
        Actor& minion = level.monsters[static_cast<size_t>(target_index)];
        std::swap(gs.player.x, minion.x);
        std::swap(gs.player.y, minion.y);
        level.map.update_fov(gs.player.x, gs.player.y, FOV_RADIUS);
        end_turn(gs);
      } else if (target_index >= 0) {
        // Bump attack: walking into a monster attacks it instead of moving. Exactly the
        // same call a monster makes when it swings at you — the dodge roll, the armor
        // reduction, the XP and the loot drop all live in resolve_attack(), not here.
        resolve_attack(gs, gs.player, level.monsters[static_cast<size_t>(target_index)], gs.player.weapon);
        end_turn(gs);  // any monster(s) still adjacent (including the one just hit) get to act
      } else if (level.map.is_walkable(new_x, new_y)) {
        gs.player.x = new_x;
        gs.player.y = new_y;
        level.map.update_fov(gs.player.x, gs.player.y, FOV_RADIUS);
        end_turn(gs);
      }
    }
  }

  return 0;
}

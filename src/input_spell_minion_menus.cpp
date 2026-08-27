// Input handling for the spell/minion menus: LevelUp, SchoolChoice, SpellMenu,
// MinionRoster, MinionAbilityMenu. Split out of input.cpp purely to keep that file's
// size down (see input_internal.hpp and CLAUDE.md's "Known limitations" note) — reached
// the same way every other mode is, through handle_event()'s one switch. Each function
// still owns exactly one Mode, same as before the split.

#include "input.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "actors.hpp"
#include "content.hpp"
#include "input_internal.hpp"
#include "level.hpp"
#include "projectile.hpp"
#include "render.hpp"
#include "rng.hpp"
#include "rules.hpp"
#include "run_history.hpp"
#include "spells.hpp"
#include "turn.hpp"

void handle_level_up_input(GameState& gs, const SDL_Event& event) {
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
  return;
}

void handle_school_choice_input(GameState& gs, const SDL_Event& event) {
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
    // sites — announces the school's own entry spell (Fireball/Summon Imp/
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
  return;
}

void handle_spell_menu_input(GameState& gs, const SDL_Event& event) {
  Level& level = gs.level();
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
        const MinionTemplate& tmpl = kMinionTable[static_cast<size_t>(spell.summon_template_index)];
        // Each summon spell guards its own pool (Spell::minion_cap), independent of any
        // other minion source — summoning Imps doesn't crowd out room for a Demon or a
        // raised corpse.
        if (spell.minion_cap >= 0 && count_minions_named(level.monsters, tmpl.name) >= spell.minion_cap) {
          add_message(gs, "You can't summon any more " + tmpl.name + "s right now.");
          gs.mode = Mode::Playing;  // free cancel, no turn spent
        } else if (gs.player.mana < spell.mana_cost) {
          add_message(gs, "Not enough mana to cast " + spell.name + ".");
          gs.mode = Mode::Playing;  // free cancel, no turn spent
        } else if (!free_adjacent_tile(level.map, level.monsters, gs.player.x, gs.player.y, spawn_x, spawn_y)) {
          add_message(gs, "There's no room to summon here!");
          gs.mode = Mode::Playing;  // free cancel, no turn spent
        } else {
          gs.player.mana -= spell.mana_cost;
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
      } else if (spell.is_raise) {
        gs.casting_spell_index = spell_idx;
        gs.casting_actor_id = -1;
        // Auto-aim at the closest corpse in range with a clear line — the same three
        // conditions Enter checks, so the cursor never starts somewhere the cast would
        // be refused. Falls back to the player's own tile when there's nothing to raise.
        int best = -1, best_dist = -1;
        for (size_t i = 0; i < level.corpses.size(); ++i) {
          const Corpse& corpse = level.corpses[i];
          int dx = corpse.x - gs.player.x, dy = corpse.y - gs.player.y;
          int dist = dx * dx + dy * dy;
          if (dist > spell.range * spell.range) continue;
          if (!line_clear(gs.player.x, gs.player.y, corpse.x, corpse.y, level.map)) continue;
          if (best < 0 || dist < best_dist) {
            best = static_cast<int>(i);
            best_dist = dist;
          }
        }
        if (best >= 0) {
          gs.target_x = level.corpses[static_cast<size_t>(best)].x;
          gs.target_y = level.corpses[static_cast<size_t>(best)].y;
        } else {
          gs.target_x = gs.player.x;
          gs.target_y = gs.player.y;
        }
        gs.mode = Mode::Targeting;
      } else if (spell.is_swap) {
        gs.casting_spell_index = spell_idx;
        gs.casting_actor_id = -1;  // the player is casting this one
        // Auto-aim at the closest minion you could actually swap with — in range and
        // with a clear line (see closest_own_minion()) — else fall back to the player's
        // own tile; Enter rejects the cast with a message if the cursor isn't on a
        // valid one.
        int auto_id = closest_own_minion(level.monsters, gs.player, level.map, spell.range);
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
        gs.casting_actor_id = -1;  // the player is casting this one
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
  return;
}

void handle_minion_roster_input(GameState& gs, const SDL_Event& event) {
  Level& level = gs.level();
  if (event.key.key == SDLK_ESCAPE) {
    gs.mode = Mode::Playing;
    return;
  }
  if (event.key.key == SDLK_A && (event.key.mod & SDL_KMOD_SHIFT) != 0) {
    // Shift+A always means "All", regardless of which letter it actually landed
    // on this frame (that shifts with the pack's current size) — a fast path so
    // you don't have to read the list to find the right letter every time.
    gs.commanding_all_minions = true;
    gs.target_x = gs.player.x;
    gs.target_y = gs.player.y;
    gs.mode = Mode::MinionFocus;
    return;
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
  return;
}

// Abilities of the minion currently focused in Mode::MinionFocus. Empty (and the menu
// unreachable) for a minion whose kMinionTable row lists none.
std::vector<int> focused_minion_abilities(GameState& gs) {
  int idx = actor_index_by_id(gs.level().monsters, gs.focused_minion_id);
  if (idx < 0) return {};
  return gs.level().monsters[static_cast<size_t>(idx)].abilities;
}

void handle_minion_ability_menu_input(GameState& gs, const SDL_Event& event) {
  Level& level = gs.level();
  if (event.key.key == SDLK_ESCAPE) {
    gs.mode = Mode::MinionFocus;  // back to the cursor, free
    return;
  }
  if (event.key.key < SDLK_A || event.key.key > SDLK_Z) return;

  auto abilities = focused_minion_abilities(gs);
  size_t pick = static_cast<size_t>(event.key.key - SDLK_A);
  if (pick >= abilities.size()) return;

  int minion_idx = actor_index_by_id(level.monsters, gs.focused_minion_id);
  if (minion_idx < 0) {  // died while the menu was open
    gs.mode = Mode::Playing;
    return;
  }
  const Actor& minion = level.monsters[static_cast<size_t>(minion_idx)];
  int spell_idx = abilities[pick];
  const Spell& spell = kSpellTable[static_cast<size_t>(spell_idx)];

  // Checked here as well as at Enter so an unaffordable ability is rejected before the
  // player aims it, rather than after.
  if (minion.mana < spell.mana_cost) {
    add_message(gs, "Your " + minion.name + " hasn't the mana for " + spell.name + ".");
    gs.mode = Mode::MinionFocus;
    return;
  }

  gs.casting_spell_index = spell_idx;
  gs.casting_actor_id = minion.id;
  // Aim from the *minion*: auto_target_hostile() measures range and picks the closest
  // qualifying hostile relative to whichever Actor it's handed, so passing the minion
  // gives exactly the right candidate set. Falls back to the minion's own tile, which is
  // always trivially in range.
  int auto_id = auto_target_hostile(level.monsters, minion, level.map, gs.last_target_id, spell.range);
  int auto_idx = actor_index_by_id(level.monsters, auto_id);
  if (auto_idx >= 0) {
    gs.target_x = level.monsters[static_cast<size_t>(auto_idx)].x;
    gs.target_y = level.monsters[static_cast<size_t>(auto_idx)].y;
  } else {
    gs.target_x = minion.x;
    gs.target_y = minion.y;
  }
  gs.mode = Mode::Targeting;
}

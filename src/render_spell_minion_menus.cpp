// Rendering for the spell/minion menus: SpellMenu, SchoolChoice, MinionRoster,
// MinionAbilityMenu. Split out of render.cpp purely to keep that file's size down (see
// render_internal.hpp and CLAUDE.md's "Known limitations" note) — reached the same way
// every other mode is, through render_frame()'s one switch. Each function still owns
// exactly one Mode, same as before the split.

#include "render.hpp"

#include "actors.hpp"
#include "content.hpp"
#include "render_internal.hpp"
#include "rules.hpp"
#include "spells.hpp"

void render_spell_menu(GameState& gs, tcod::Console& console) {
  Level& level = gs.level();
  tcod::print(console, {0, 0}, "Spells - press a letter to cast, Esc to close", tcod::ColorRGB{255, 255, 255},
              std::nullopt);

  auto known = known_spell_indices(gs.player.intelligence, gs.player.chosen_school);
  if (known.empty()) {
    tcod::print(console, {0, 2}, "(no spells known yet)", tcod::ColorRGB{120, 120, 120}, std::nullopt);
  }
  for (size_t i = 0; i < known.size(); ++i) {
    const Spell& s = kSpellTable[static_cast<size_t>(known[i])];
    bool is_active = gs.active_toggle_spell == known[i];
    std::string line;
    bool at_minion_cap = false;
    if (s.is_toggle) {
      line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (" +
             std::to_string(s.tick_damage) + " dmg/turn in " + std::to_string(2 * s.aoe_radius + 1) + "x" +
             std::to_string(2 * s.aoe_radius + 1) + ", " + std::to_string(s.tick_mana_cost) + " MP/turn) - " +
             std::to_string(s.mana_cost) + " MP to toggle" + (is_active ? " [ACTIVE]" : "");
    } else if (s.is_summon) {
      const MinionTemplate& tmpl = kMinionTable[static_cast<size_t>(s.summon_template_index)];
      std::string duration_str =
          tmpl.duration_turns > 0 ? std::to_string(tmpl.duration_turns) + " turns" : "permanent";
      at_minion_cap = s.minion_cap >= 0 && count_minions_named(level.monsters, tmpl.name) >= s.minion_cap;
      line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (summons a " + tmpl.name + ", " +
             duration_str + ") - " + std::to_string(s.mana_cost) + " MP" + (at_minion_cap ? " [AT CAP]" : "");
    } else if (s.is_raise) {
      at_minion_cap = s.minion_cap >= 0 && count_raised_minions(level.monsters) >= s.minion_cap;
      line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (raise a corpse as a minion) - " +
             std::to_string(s.mana_cost) + " MP" + (at_minion_cap ? " [AT CAP]" : "");
    } else if (s.is_melee_buff) {
      line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (+" + std::to_string(s.buff_amount) +
             " melee damage, " + std::to_string(s.buff_turns) + " turns) - " + std::to_string(s.mana_cost) +
             " MP";
    } else if (s.is_armor_buff) {
      line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (+" + std::to_string(s.buff_amount) +
             " armor, " + std::to_string(s.buff_turns) + " turns) - " + std::to_string(s.mana_cost) + " MP";
    } else if (s.is_haste_buff) {
      line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (+" + std::to_string(s.buff_amount) +
             " action/turn, " + std::to_string(s.buff_turns) + " turns) - " + std::to_string(s.mana_cost) + " MP";
    } else if (s.is_swap) {
      line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (swap places with a minion) - " +
             std::to_string(s.mana_cost) + " MP";
    } else {
      line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (" + std::to_string(s.dice_count) +
             "d" + std::to_string(s.dice_sides) + "+INT/3) - " + std::to_string(s.mana_cost) + " MP";
    }
    // Dimmed red instead of the usual grey once you can't actually afford it — a
    // currently-active toggle is always "affordable" to select again (turning it
    // off is always free) so it doesn't get the red treatment.
    bool affordable = is_active || (gs.player.mana >= s.mana_cost && !at_minion_cap);
    tcod::print(console, {0, 2 + static_cast<int>(i)}, line,
                affordable ? tcod::ColorRGB{200, 200, 200} : tcod::ColorRGB{150, 80, 80}, std::nullopt);
  }
}

// One focused minion's abilities. Deliberately the same lettered shape as the player's
// spell menu, since it's the same gesture ('z') aimed at a different caster — the only
// extra column is whose mana pays.
void render_minion_ability_menu(GameState& gs, tcod::Console& console) {
  const Level& level = gs.level();
  int idx = actor_index_by_id(level.monsters, gs.focused_minion_id);
  if (idx < 0) {
    tcod::print(console, {0, 0}, "That minion is gone. Esc to close.", tcod::ColorRGB{255, 255, 255}, std::nullopt);
    return;
  }
  const Actor& minion = level.monsters[static_cast<size_t>(idx)];

  tcod::print(console, {0, 0}, minion.name + " abilities - press a letter to use, Esc to go back",
              tcod::ColorRGB{255, 255, 255}, std::nullopt);
  tcod::print(console, {0, 1},
              "Mana: " + std::to_string(minion.mana) + "/" + std::to_string(minion.max_mana) +
                  (minion.mana_regen_turns == 0 ? "  (does not regenerate)" : ""),
              tcod::ColorRGB{150, 200, 255}, std::nullopt);

  if (minion.abilities.empty()) {
    tcod::print(console, {0, 3}, "(none)", tcod::ColorRGB{120, 120, 120}, std::nullopt);
    return;
  }
  for (size_t i = 0; i < minion.abilities.size(); ++i) {
    const Spell& s = kSpellTable[static_cast<size_t>(minion.abilities[i])];
    bool affordable = minion.mana >= s.mana_cost;
    std::string line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (" +
                       std::to_string(s.mana_cost) + " MP, range " + std::to_string(s.range) + ")";
    if (s.is_debuff) {
      line += "  " + std::to_string(s.buff_amount) + " melee damage to the target for " +
              std::to_string(s.buff_turns) + " turns";
    }
    if (!affordable) line += "  [not enough mana]";
    tcod::print(console, {0, 3 + static_cast<int>(i)}, line,
                affordable ? tcod::ColorRGB{200, 200, 200} : tcod::ColorRGB{120, 120, 120}, std::nullopt);
  }
}

void render_minion_roster(GameState& gs, tcod::Console& console) {
  Level& level = gs.level();
  tcod::print(console, {0, 0}, "Command a minion - press a letter, Esc to close", tcod::ColorRGB{255, 255, 255},
              std::nullopt);
  // "All" is a fixed hotkey (Shift+A) pinned above the roster rather than a letter
  // tacked onto the end of it — a trailing letter shifts around as the pack's size
  // changes (and got long enough with a real attack target named to run off the
  // sidebar in the equivalent per-minion list, see minion_order_flag() above), so
  // anchoring it first keeps the ordering predictable regardless of pack size.
  tcod::print(console, {0, 2}, "Shift+A) All minions at once", tcod::ColorRGB{200, 200, 200}, std::nullopt);
  // Each living minion then gets its own letter, in level.monsters order (stable
  // turn to turn barring a death).
  int row = 4;
  char letter = 'a';
  for (const auto& m : level.monsters) {
    if (m.allegiance != Allegiance::Player || !m.is_alive()) continue;
    std::string line =
        std::string(1, letter) + ") " + m.name + " (" + describe_minion_order(m, level.monsters) + ")";
    tcod::print(console, {0, row}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
    ++row;
    ++letter;
  }
}

// Static text; reads no game state, hence no GameState parameter.
void render_school_choice(tcod::Console& console) {
  // Full-screen forced prompt, same shape as MinionRoster above rather than
  // LevelUp's one-line CONTEXT_ROW style — this needs room to explain all three
  // paths, since it's a permanent, run-defining choice rather than a quick stat bump.
  tcod::print(console, {0, 0},
              "You have grown wise enough to specialize your magic. Choose a path - this choice is permanent.",
              tcod::ColorRGB{255, 255, 255}, std::nullopt);
  tcod::print(console, {0, 2},
              "Shift+C) Caster      -- offensive magic: Fireball, Sandstorm, Lightning Bolt",
              tcod::ColorRGB{200, 200, 200}, std::nullopt);
  tcod::print(console, {0, 3},
              "Shift+U) Summoner    -- minions: Summon Imp, Place Swap, Raise Dead, Summon Demon",
              tcod::ColorRGB{200, 200, 200}, std::nullopt);
  tcod::print(console, {0, 4},
              "Shift+M) Combat Mage -- self-buffs: Battle Fury, Iron Skin, Haste",
              tcod::ColorRGB{200, 200, 200}, std::nullopt);
}

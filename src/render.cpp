#include "render.hpp"

#include <algorithm>
#include <filesystem>

#include "actors.hpp"
#include "content.hpp"
#include "projectile.hpp"
#include "rules.hpp"
#include "spells.hpp"

// Darkens a color for the "remembered, but not currently visible" rendering tier.
tcod::ColorRGB dim_color(tcod::ColorRGB c) {
  return tcod::ColorRGB{static_cast<uint8_t>(c.r / 3), static_cast<uint8_t>(c.g / 3), static_cast<uint8_t>(c.b / 3)};
}

// Common monospace font paths, one per Linux distro this project's README documents
// setup for. Tried in order; the first one found is used. This approximates "use the
// font your terminal uses" without a fontconfig dependency or bundling a font file:
// on an unconfigured terminal (no custom font override), these paths ARE what
// fontconfig's "monospace" alias resolves to on each respective distro.
const std::vector<std::string> kPreferredFontPaths = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",         // Debian/Ubuntu
    "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",  // Fedora
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",                     // Arch
};

// Loads the first font from kPreferredFontPaths that exists on disk, rendered at
// tile_size x tile_size pixels per cell. Falls back to libtcod's built-in font (same
// one used before this project had any font-selection logic) if none of them exist.
tcod::TilesetPtr load_best_tileset(int tile_size) {
  for (const auto& path : kPreferredFontPaths) {
    if (!std::filesystem::exists(path)) continue;
    tcod::TilesetPtr tileset{TCOD_load_truetype_font_(path.c_str(), tile_size, tile_size)};
    if (tileset) return tileset;
  }
  return tcod::tileset::new_fallback_tileset({tile_size, tile_size});
}

// Hand-drawn ASCII box: corners '+', horizontal '-', vertical '|', with an optional
// title embedded in the top border (" Title "). libtcod's own tcod::print_frame() is
// deprecated upstream in favor of exactly this ("print your own banners for frames"),
// so this is the recommended shape, not a workaround. (x, y) is the box's top-left
// corner in console cells; the box is w x h cells including the border, so a panel's
// drawable interior is (x+1, y+1) through (x+w-2, y+h-2).
void draw_panel(tcod::Console& console, int x, int y, int w, int h, const std::string& title,
                 tcod::ColorRGB color) {
  for (int i = x + 1; i < x + w - 1; ++i) {
    console.at(i, y).ch = '-';
    console.at(i, y).fg = color;
    console.at(i, y + h - 1).ch = '-';
    console.at(i, y + h - 1).fg = color;
  }
  for (int j = y + 1; j < y + h - 1; ++j) {
    console.at(x, j).ch = '|';
    console.at(x, j).fg = color;
    console.at(x + w - 1, j).ch = '|';
    console.at(x + w - 1, j).fg = color;
  }
  console.at(x, y).ch = '+';
  console.at(x, y).fg = color;
  console.at(x + w - 1, y).ch = '+';
  console.at(x + w - 1, y).fg = color;
  console.at(x, y + h - 1).ch = '+';
  console.at(x, y + h - 1).fg = color;
  console.at(x + w - 1, y + h - 1).ch = '+';
  console.at(x + w - 1, y + h - 1).fg = color;
  if (!title.empty()) {
    tcod::print(console, {x + 2, y}, " " + title + " ", tcod::ColorRGB{255, 255, 255}, std::nullopt);
  }
}

namespace {

void render_weapon_menu(GameState& gs, tcod::Console& console) {
  tcod::print(console, {0, 0}, "Weapons - press a letter to equip, Esc to close", tcod::ColorRGB{255, 255, 255},
              std::nullopt);
  tcod::print(console, {0, 1}, "Equipped: " + gs.player.weapon.name + " (" + describe_weapon(gs.player.weapon) + ")",
              tcod::ColorRGB{200, 200, 100}, std::nullopt);

  // Fists is always slot 'a', so you can always bail back to unarmed; carried
  // weapons fill 'b' onward.
  std::string fists_line = "a) Fists (" + describe_weapon(kFists) + ")";
  if (gs.player.weapon.is_intrinsic) fists_line += " [equipped]";
  tcod::print(console, {0, 3}, fists_line, tcod::ColorRGB{200, 200, 200}, std::nullopt);

  for (size_t i = 0; i < gs.player.weapons.size(); ++i) {
    std::string line = std::string(1, static_cast<char>('b' + i)) + ") " + gs.player.weapons[i].name + " (" +
                        describe_weapon(gs.player.weapons[i]) + ")";
    tcod::print(console, {0, 4 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
  }
}

void render_armor_menu(GameState& gs, tcod::Console& console) {
  tcod::print(console, {0, 0}, "Armor - press a letter to equip, Esc to close", tcod::ColorRGB{255, 255, 255},
              std::nullopt);
  tcod::print(console, {0, 1}, "Equipped: " + gs.player.armor.name + " (" + describe_armor(gs.player.armor) + ")",
              tcod::ColorRGB{200, 200, 100}, std::nullopt);

  // "Nothing" is always slot 'a', so you can always bail back to unarmored; carried
  // armor fills 'b' onward.
  std::string none_line = "a) " + kNoArmor.name + " (" + describe_armor(kNoArmor) + ")";
  if (gs.player.armor.is_intrinsic) none_line += " [equipped]";
  tcod::print(console, {0, 3}, none_line, tcod::ColorRGB{200, 200, 200}, std::nullopt);

  for (size_t i = 0; i < gs.player.armors.size(); ++i) {
    std::string line = std::string(1, static_cast<char>('b' + i)) + ") " + gs.player.armors[i].name + " (" +
                        describe_armor(gs.player.armors[i]) + ")";
    tcod::print(console, {0, 4 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
  }
}

void render_potion_menu(GameState& gs, tcod::Console& console) {
  tcod::print(console, {0, 0}, "Potions - press a letter to drink, Esc to close", tcod::ColorRGB{255, 255, 255},
              std::nullopt);

  if (gs.player.potions.empty()) {
    tcod::print(console, {0, 2}, "(no potions carried)", tcod::ColorRGB{120, 120, 120}, std::nullopt);
  }
  for (size_t i = 0; i < gs.player.potions.size(); ++i) {
    std::string line = std::string(1, static_cast<char>('a' + i)) + ") " + gs.player.potions[i].name + " (" +
                        describe_potion(gs.player.potions[i]) + ")";
    tcod::print(console, {0, 2 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
  }
}

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
      at_minion_cap = count_minions(level.monsters) >= kMaxMinions;
      line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (summons a " + tmpl.name + ", " +
             duration_str + ") - " + std::to_string(s.mana_cost) + " MP" + (at_minion_cap ? " [AT CAP]" : "");
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

void render_drop_screen(GameState& gs, tcod::Console& console) {
  tcod::print(console, {0, 0}, "Drop - press a letter to drop, Esc to cancel", tcod::ColorRGB{255, 255, 255},
              std::nullopt);

  auto slots = drop_slots(gs.player);
  if (slots.empty()) {
    tcod::print(console, {0, 2}, "(nothing to drop)", tcod::ColorRGB{120, 120, 120}, std::nullopt);
  }
  for (size_t i = 0; i < slots.size(); ++i) {
    char letter = static_cast<char>('a' + i);
    std::string line;
    if (slots[i].kind == ItemKind::Weapon) {
      const Weapon& w = (slots[i].index == -1) ? gs.player.weapon : gs.player.weapons[static_cast<size_t>(slots[i].index)];
      line = std::string(1, letter) + ") " + w.name + " (" + describe_weapon(w) + ")";
      if (slots[i].index == -1) line += " [equipped]";
    } else if (slots[i].kind == ItemKind::Armor) {
      const Armor& a = (slots[i].index == -1) ? gs.player.armor : gs.player.armors[static_cast<size_t>(slots[i].index)];
      line = std::string(1, letter) + ") " + a.name + " (" + describe_armor(a) + ")";
      if (slots[i].index == -1) line += " [equipped]";
    } else {
      const Potion& p = gs.player.potions[static_cast<size_t>(slots[i].index)];
      line = std::string(1, letter) + ") " + p.name + " (" + describe_potion(p) + ")";
    }
    tcod::print(console, {0, 2 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
  }
}

void render_death_screen(GameState& gs, tcod::Console& console) {
  tcod::print(console, {0, 0}, "You died, slain by the " + gs.death_cause + ".", tcod::ColorRGB{255, 80, 80},
              std::nullopt);
  tcod::print(console, {0, 2}, "Press any key to start a new game, or Esc to quit.", tcod::ColorRGB{200, 200, 200},
              std::nullopt);
}

void render_message_log(GameState& gs, tcod::Console& console) {
  tcod::print(console, {0, 0}, "Message Log - j/k or arrows to scroll, ']' or Esc to close",
              tcod::ColorRGB{255, 255, 255}, std::nullopt);

  int visible_rows = SCREEN_HEIGHT - 1;
  int total = static_cast<int>(gs.message_log.size());
  int max_scroll = std::max(0, total - visible_rows);
  gs.log_scroll = std::min(gs.log_scroll, max_scroll);  // clamp in case the log shrank (e.g. after a restart)

  // Oldest at top, newest at bottom, like a terminal scrollback — log_scroll is how
  // many lines scrolled up from the bottom (0 = showing the most recent messages).
  int end_index = total - gs.log_scroll;
  int start_index = std::max(0, end_index - visible_rows);
  for (int i = start_index; i < end_index; ++i) {
    int row = 1 + (i - start_index);
    tcod::print(console, {0, row}, gs.message_log[static_cast<size_t>(i)], tcod::ColorRGB{200, 200, 200},
                std::nullopt);
  }
}

// Static text; reads no game state, hence no GameState parameter.
void render_help(tcod::Console& console) {
  tcod::print(console, {0, 0}, "Controls - '?' or Esc to close", tcod::ColorRGB{255, 255, 255}, std::nullopt);
  static const std::vector<std::string> kHelpLines = {
      "",
      "Arrows / hjkl / yubn (diagonals)  Move; walks into an enemy to attack, or",
      "                                  swaps places with your own minion",
      ".                                 Wait a turn",
      ">  <                              Stairs down/up (must be standing on them)",
      "g                                 Pick up everything on your tile",
      "w  a  q                           Weapon / Armor / Potion menu (equip or drink)",
      "d                                 Drop a weapon, armor, or potion",
      "f                                 Fire the equipped ranged weapon (move to target,",
      "                                  Enter to loose it, Esc to cancel)",
      "z                                 Cast a known spell",
      "m                                 Command a minion or all of them (roster menu; Shift+A",
      "                                  there jumps straight to All)",
      "o  p                              Cycle command focus to the next/previous minion",
      "Shift+P                           Return focus to yourself",
      "f  g  Enter                       While focused on a minion instead: Follow / go",
      "                                  Aggressive / confirm Attack or Hold (sidebar shows",
      "                                  each minion's order as [F]ollow / [G]o aggressive /",
      "                                  [H]old / [A]ttack)",
      "x                                 Look around (move the cursor, side panel shows",
      "                                  details); x or Esc to close",
      "]                                 Message log (full scrollback)",
      "Shift+S  Shift+D  Shift+I         On level up: spend the point on STR/DEX/INT",
      "Shift+C  Shift+U  Shift+M         At Intelligence 4: choose Caster, Summoner, or",
      "                                  Combat Mage (once, permanent)",
      "?                                 This screen",
      "Esc                               Quit (or close the current menu)",
  };
  for (size_t i = 0; i < kHelpLines.size(); ++i) {
    tcod::print(console, {0, 1 + static_cast<int>(i)}, kHelpLines[i], tcod::ColorRGB{200, 200, 200},
                std::nullopt);
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
              "Shift+U) Summoner    -- conjury and command: Raise Skeleton, Place Swap, Summon Demon",
              tcod::ColorRGB{200, 200, 200}, std::nullopt);
  tcod::print(console, {0, 4},
              "Shift+M) Combat Mage -- self-buffs: Battle Fury, Iron Skin, Haste",
              tcod::ColorRGB{200, 200, 200}, std::nullopt);
  tcod::print(console, {0, 6}, "Magic Dart and Energy Lance stay available whichever path you pick.",
              tcod::ColorRGB{120, 120, 120}, std::nullopt);
}

void render_context_row(GameState& gs, tcod::Console& console) {
  Level& level = gs.level();
  // Row CONTEXT_ROW: a transient action prompt (level-up / spell targeting /
  // minion command) when one of those modes is active. Deliberately separate from
  // the message-log panel below — a long-running prompt shouldn't crowd out or get
  // crowded out by ordinary log messages.
  if (gs.mode == Mode::LevelUp) {
    std::string prompt = "*** LEVEL UP (now level " + std::to_string(gs.player.level) +
                          ")! Press Shift+S/D/I to raise Strength/Dexterity/Intelligence. ***";
    tcod::print(console, {0, CONTEXT_ROW}, prompt, tcod::ColorRGB{255, 255, 100}, std::nullopt);
  } else if (gs.mode == Mode::Targeting) {
    const Spell& casting_spell = kSpellTable[static_cast<size_t>(gs.casting_spell_index)];
    std::string prompt = "Casting " + casting_spell.name + " (" + std::to_string(casting_spell.mana_cost) +
                          " MP) - move to target, Enter to fire, Esc to cancel.";
    tcod::print(console, {0, CONTEXT_ROW}, prompt, tcod::ColorRGB{255, 255, 100}, std::nullopt);
  } else if (gs.mode == Mode::MinionFocus) {
    std::string who = "your minion";
    if (gs.commanding_all_minions) {
      who = "all minions";
    } else {
      int fi = actor_index_by_id(level.monsters, gs.focused_minion_id);
      if (fi >= 0) who = level.monsters[static_cast<size_t>(fi)].name;
    }
    std::string prompt = "Commanding " + who +
                          " - move to a monster (attack), your own tile (follow), or elsewhere "
                          "(hold), Enter to confirm, F to follow, Esc to cancel.";
    tcod::print(console, {0, CONTEXT_ROW}, prompt, tcod::ColorRGB{255, 255, 100}, std::nullopt);
  } else if (gs.mode == Mode::Look) {
    tcod::print(console, {0, CONTEXT_ROW}, "Looking around - move the cursor to inspect, Esc to close.",
                tcod::ColorRGB{255, 255, 100}, std::nullopt);
  } else if (gs.mode == Mode::RangedAttack) {
    std::string prompt = "Firing your " + gs.player.weapon.name + " - move to target, Enter to fire, Esc to cancel.";
    tcod::print(console, {0, CONTEXT_ROW}, prompt, tcod::ColorRGB{255, 255, 100}, std::nullopt);
  }
}

void render_sidebar(GameState& gs, tcod::Console& console) {
  Level& level = gs.level();
  // --- Sidebar: character stats, then who's around ("at a glance") ---
  draw_panel(console, SIDEBAR_X, SIDEBAR_Y, SIDEBAR_W, SIDEBAR_H, "Status");
  int sb_x = SIDEBAR_X + 1;
  int sb_row = SIDEBAR_Y + 1;
  int sb_bottom = SIDEBAR_Y + SIDEBAR_H - 2;  // last usable interior row
  // Appends one line and advances sb_row, silently dropping anything past the
  // panel's interior instead of overflowing into its border. Also clips the text
  // itself to the panel's interior width — the sidebar sits flush against the
  // console's right edge with nothing beyond it, so an unclipped long line (e.g. a
  // minion's status naming a long monster name) would run right off the window
  // instead of just looking crowded.
  int sb_max_width = SIDEBAR_W - 2;
  auto sb_print = [&](const std::string& text, tcod::ColorRGB color) {
    if (sb_row > sb_bottom) return;
    std::string clipped = text;
    if (static_cast<int>(clipped.size()) > sb_max_width) {
      clipped = clipped.substr(0, static_cast<size_t>(std::max(0, sb_max_width - 3))) + "...";
    }
    tcod::print(console, {sb_x, sb_row}, clipped, color, std::nullopt);
    ++sb_row;
  };

  sb_print("HP: " + std::to_string(gs.player.hp) + "/" + std::to_string(gs.player.max_hp),
           tcod::ColorRGB{255, 255, 255});
  sb_print("MP: " + std::to_string(gs.player.mana) + "/" + std::to_string(gs.player.max_mana),
           tcod::ColorRGB{255, 255, 255});
  sb_print("Lvl: " + std::to_string(gs.player.level) + "  Floor: " + std::to_string(gs.current_level + 1),
           tcod::ColorRGB{255, 255, 255});

  // Appends "+N" to a stat only while its temp buff is active, so the HUD reflects
  // Potion of Strength/Dexterity/Intelligence without a separate buff tracker.
  auto stat_str = [](int base, int bonus) {
    return std::to_string(base) + (bonus > 0 ? "+" + std::to_string(bonus) : "");
  };
  sb_print("STR: " + stat_str(gs.player.strength, gs.player.temp_str_bonus), tcod::ColorRGB{200, 200, 200});
  sb_print("DEX: " + stat_str(gs.player.dexterity, gs.player.temp_dex_bonus), tcod::ColorRGB{200, 200, 200});
  sb_print("INT: " + stat_str(gs.player.intelligence, gs.player.temp_int_bonus), tcod::ColorRGB{200, 200, 200});
  // Evasion is a real, comparable number now — the same rating a monster's table row
  // authors — so it's worth showing rather than leaving DEX's effect implicit.
  sb_print("Eva: " + std::to_string(gs.player.evasion), tcod::ColorRGB{200, 200, 200});
  sb_print("Wpn: " + gs.player.weapon.name, tcod::ColorRGB{200, 200, 200});
  sb_print("  (" + describe_weapon(gs.player.weapon) + ")", tcod::ColorRGB{150, 150, 150});
  sb_print("Arm: " + gs.player.armor.name, tcod::ColorRGB{200, 200, 200});
  sb_print("  (" + describe_armor(gs.player.armor) + ")", tcod::ColorRGB{150, 150, 150});
  // A running toggle spell (e.g. Sandstorm) has no other on-screen presence besides
  // its aura tile overlay — this tag is the only text indicator it's still active.
  if (gs.active_toggle_spell >= 0) {
    sb_print("[" + kSpellTable[static_cast<size_t>(gs.active_toggle_spell)].name + "]",
             tcod::ColorRGB{255, 255, 100});
  }

  // Every active temporary buff, with what it's worth and how many turns are left.
  // The STR/DEX/INT lines above already show their potion bonuses inline as "+N", but
  // that says nothing about how much longer they last, and the three spell buffs
  // (Battle Fury / Iron Skin / Haste) had no on-screen presence at all before this —
  // Haste in particular needs one, since "the world didn't move" is otherwise
  // indistinguishable from a dropped keypress. One list, so there's a single place to
  // look and a single place to add the next buff.
  //
  // Sits above the Enemies/Minions lists for the same reason the Look block does:
  // sb_print silently drops anything past the panel's bottom, and a timer you're
  // playing around matters more than the tail of a long enemy list. Rows only exist
  // while something is actually active, so the usual case costs nothing.
  {
    auto buff_row = [&](const std::string& label, int bonus, int turns) {
      if (turns <= 0) return;
      sb_print("  " + label + " +" + std::to_string(bonus) + " (" + std::to_string(turns) + ")",
               tcod::ColorRGB{160, 255, 160});
    };
    bool any_buff = gs.player.temp_str_turns > 0 || gs.player.temp_dex_turns > 0 || gs.player.temp_int_turns > 0 ||
                    gs.player.temp_melee_damage_turns > 0 || gs.player.temp_armor_turns > 0 ||
                    gs.player.temp_extra_actions_turns > 0;
    if (any_buff) {
      sb_print("Buffs:", tcod::ColorRGB{200, 255, 200});
      buff_row("STR", gs.player.temp_str_bonus, gs.player.temp_str_turns);
      buff_row("DEX", gs.player.temp_dex_bonus, gs.player.temp_dex_turns);
      buff_row("INT", gs.player.temp_int_bonus, gs.player.temp_int_turns);
      buff_row("Melee dmg", gs.player.temp_melee_damage_bonus, gs.player.temp_melee_damage_turns);
      buff_row("Armor", gs.player.temp_armor_bonus, gs.player.temp_armor_turns);
      buff_row("Actions", gs.player.temp_extra_actions_bonus, gs.player.temp_extra_actions_turns);
    }
  }

  // Placed ahead of Enemies/Minions (rather than after) so it can never get
  // silently dropped by sb_print's bottom-of-panel clamp when the pack/enemy
  // lists below run long — the thing you're actively examining is the more
  // important thing to keep on screen right now.
  if (gs.mode == Mode::Look) {
    ++sb_row;
    sb_print("Looking:", tcod::ColorRGB{200, 200, 255});
    bool explored = level.map.is_explored(gs.target_x, gs.target_y);
    if (!explored && !gs.reveal_all) {
      sb_print("  (unexplored)", tcod::ColorRGB{120, 120, 120});
    } else {
      bool tile_in_fov = level.map.is_in_fov(gs.target_x, gs.target_y);
      bool is_stairs_down = (gs.target_x == level.stairs_down_x && gs.target_y == level.stairs_down_y);
      bool is_stairs_up = level.has_stairs_up && (gs.target_x == level.entry_x && gs.target_y == level.entry_y);
      std::string terrain = is_stairs_down                             ? "Stairs down"
                             : is_stairs_up                             ? "Stairs up"
                             : level.map.at(gs.target_x, gs.target_y).is_hole ? "Hole"
                             : level.map.is_walkable(gs.target_x, gs.target_y) ? "Floor"
                                                                          : "Wall";
      sb_print("  " + terrain, tcod::ColorRGB{200, 200, 200});

      if (!tile_in_fov && !gs.reveal_all) {
        // Remembered terrain layout is fine to show, but not live occupant
        // details — same rule the map rendering itself already follows (items/
        // monsters only ever show up while actually in view).
        sb_print("  (out of view)", tcod::ColorRGB{120, 120, 120});
      } else {
        bool found_anything = false;
        int mi = monster_at(level.monsters, gs.target_x, gs.target_y);
        if (mi >= 0) {
          const Actor& m = level.monsters[static_cast<size_t>(mi)];
          sb_print("  " + m.name, m.color);
          sb_print("    HP: " + std::to_string(m.hp) + "/" + std::to_string(m.max_hp),
                   tcod::ColorRGB{200, 200, 200});
          sb_print("    Wpn: " + m.weapon.name + " (" + describe_weapon(m.weapon) + ")",
                   tcod::ColorRGB{200, 200, 200});
          // Monsters wear armor and carry packs now, so 'x' is the way to see what
          // a fight is actually going to cost you — and what it'll drop.
          if (!m.armor.is_intrinsic) {
            sb_print("    Arm: " + m.armor.name + " (" + describe_armor(m.armor) + ")",
                     tcod::ColorRGB{200, 200, 200});
          }
          sb_print("    Eva: " + std::to_string(m.evasion) + "  STR: " + std::to_string(m.strength),
                   tcod::ColorRGB{150, 150, 150});
          // A caster's remaining mana is the single most useful thing to know about
          // it — the Goblin Shaman's whole design is a finite dart budget, so being
          // able to check how much of it is left is what makes waiting it out a real
          // decision rather than a guess. Only shown for something that actually
          // casts; every other monster has max_mana 0 and would just print "0/0".
          if (m.spell_index >= 0) {
            sb_print("    MP: " + std::to_string(m.mana) + "/" + std::to_string(m.max_mana) + " (" +
                         kSpellTable[static_cast<size_t>(m.spell_index)].name + ")",
                     tcod::ColorRGB{150, 150, 220});
          }
          for (const auto& carried : m.weapons) sb_print("    - " + carried.name, tcod::ColorRGB{170, 170, 200});
          for (const auto& carried : m.potions) sb_print("    - " + carried.name, carried.color);
          if (m.allegiance == Allegiance::Player) {
            sb_print("    " + describe_minion_order(m, level.monsters), tcod::ColorRGB{200, 200, 200});
          }
          found_anything = true;
        }
        for (const auto& gi : level.items) {
          if (gi.x != gs.target_x || gi.y != gs.target_y) continue;
          sb_print("  " + gi.weapon.name + " (" + describe_weapon(gi.weapon) + ")",
                   tcod::ColorRGB{200, 200, 255});
          found_anything = true;
        }
        for (const auto& ga : level.armor_items) {
          if (ga.x != gs.target_x || ga.y != gs.target_y) continue;
          sb_print("  " + ga.armor.name + " (" + describe_armor(ga.armor) + ")", tcod::ColorRGB{180, 220, 200});
          found_anything = true;
        }
        for (const auto& gp : level.potions) {
          if (gp.x != gs.target_x || gp.y != gs.target_y) continue;
          sb_print("  " + gp.potion.name + " (" + describe_potion(gp.potion) + ")", gp.potion.color);
          found_anything = true;
        }
        if (!found_anything) sb_print("  (nothing else here)", tcod::ColorRGB{120, 120, 120});
      }
    }
    ++sb_row;
  }

  sb_print("Enemies:", tcod::ColorRGB{255, 150, 150});
  bool any_enemy = false;
  for (const auto& m : level.monsters) {
    if (m.allegiance != Allegiance::Hostile || !m.is_alive()) continue;
    if (!level.map.is_in_fov(m.x, m.y)) continue;
    sb_print("  " + m.name + " (" + std::to_string(m.hp) + "/" + std::to_string(m.max_hp) + ")", m.color);
    any_enemy = true;
  }
  if (!any_enemy) sb_print("  (none in view)", tcod::ColorRGB{120, 120, 120});

  ++sb_row;
  sb_print("Minions:", tcod::ColorRGB{150, 220, 255});
  bool any_minion = false;
  for (const auto& m : level.monsters) {
    if (m.allegiance != Allegiance::Player || !m.is_alive()) continue;
    bool focused = !gs.commanding_all_minions && m.id == gs.focused_minion_id;
    tcod::ColorRGB color = focused ? tcod::ColorRGB{255, 255, 100} : m.color;
    // [F]ollow / [H]old / [A]ttack — a flag instead of describe_minion_order()'s
    // full sentence, which could run past the sidebar's width once it named an
    // attack target (e.g. "attacking the Goblin Slinger").
    sb_print("  " + std::string(focused ? "*" : " ") + m.name + " [" + minion_order_flag(m) + "]", color);
    any_minion = true;
  }
  if (!any_minion) sb_print("  (none)", tcod::ColorRGB{120, 120, 120});
}

void render_log_panel(GameState& gs, tcod::Console& console) {
  // --- Message log panel: always exactly the last MESSAGE_ROWS distinct
  // messages, oldest on top, one per line — never wrapped or combined, even if
  // several things happened on the same turn. (']' opens full scrollback.)
  draw_panel(console, LOG_PANEL_X, LOG_PANEL_Y, LOG_PANEL_W, LOG_PANEL_H, "Log");
  int log_total = static_cast<int>(gs.message_log.size());
  for (int row = 0; row < MESSAGE_ROWS; ++row) {
    int idx = log_total - MESSAGE_ROWS + row;
    if (idx < 0) continue;
    tcod::print(console, {LOG_PANEL_X + 1, LOG_PANEL_Y + 1 + row}, gs.message_log[static_cast<size_t>(idx)],
                tcod::ColorRGB{255, 255, 100}, std::nullopt);
  }
}

void render_map_panel(GameState& gs, tcod::Console& console, const Camera& camera) {
  Level& level = gs.level();
  draw_panel(console, MAP_PANEL_X, MAP_PANEL_Y, MAP_PANEL_W, MAP_PANEL_H,
             "Floor " + std::to_string(gs.current_level + 1));

  int view_x_end = std::min(camera.x + MAP_VIEW_W, level.map.width());
  int view_y_end = std::min(camera.y + MAP_VIEW_H, level.map.height());
  for (int y = camera.y; y < view_y_end; ++y) {
    for (int x = camera.x; x < view_x_end; ++x) {
      // Never seen and not revealing: leave blank.
      if (!level.map.is_explored(x, y) && !gs.reveal_all) continue;

      bool walkable = level.map.is_walkable(x, y);
      bool is_hole = level.map.at(x, y).is_hole;
      bool visible = level.map.is_in_fov(x, y);
      bool is_stairs_down = (x == level.stairs_down_x && y == level.stairs_down_y);
      bool is_stairs_up = level.has_stairs_up && (x == level.entry_x && y == level.entry_y);

      auto& cell = console.at(camera.screen_x(x), camera.screen_y(y));
      if (is_stairs_down) {
        cell.ch = '>';
      } else if (is_stairs_up) {
        cell.ch = '<';
      } else if (is_hole) {
        cell.ch = '^';
      } else {
        cell.ch = walkable ? '.' : '#';
      }

      if (visible) {
        if (is_stairs_down || is_stairs_up) {
          cell.fg = tcod::ColorRGB{255, 255, 150};
        } else if (is_hole) {
          cell.fg = tcod::ColorRGB{180, 90, 40};
        } else {
          cell.fg = walkable ? tcod::ColorRGB{160, 160, 160} : tcod::ColorRGB{90, 90, 90};
        }
      } else {
        // Remembered but currently out of sight: dimmed fog-of-war shading.
        if (is_stairs_down || is_stairs_up) {
          cell.fg = tcod::ColorRGB{110, 110, 70};
        } else if (is_hole) {
          cell.fg = tcod::ColorRGB{70, 35, 15};
        } else {
          cell.fg = walkable ? tcod::ColorRGB{60, 60, 60} : tcod::ColorRGB{35, 35, 35};
        }
      }
    }
  }

  // Remembered monster sightings: dimmed, drawn only where we can't currently see
  // (the live loop below draws anything actually visible, on top, at full brightness).
  for (const auto& remembered : level.remembered_monsters) {
    if (level.map.is_in_fov(remembered.x, remembered.y)) continue;
    if (!camera.in_view(remembered.x, remembered.y)) continue;
    auto& cell = console.at(camera.screen_x(remembered.x), camera.screen_y(remembered.y));
    cell.ch = remembered.glyph;
    cell.fg = dim_color(remembered.color);
  }

  // Items/monsters only show up while actually in view, unlike remembered terrain
  // — unless --reveal is forcing them on, in which case out-of-fov ones are drawn
  // dimmed, same tier as remembered terrain/monsters.
  for (const auto& item : level.items) {
    bool visible = level.map.is_in_fov(item.x, item.y);
    if (!visible && !gs.reveal_all) continue;
    if (!camera.in_view(item.x, item.y)) continue;
    auto& cell = console.at(camera.screen_x(item.x), camera.screen_y(item.y));
    cell.ch = '/';
    tcod::ColorRGB color{200, 200, 255};
    cell.fg = visible ? color : dim_color(color);
  }

  for (const auto& armor_item : level.armor_items) {
    bool visible = level.map.is_in_fov(armor_item.x, armor_item.y);
    if (!visible && !gs.reveal_all) continue;
    if (!camera.in_view(armor_item.x, armor_item.y)) continue;
    auto& cell = console.at(camera.screen_x(armor_item.x), camera.screen_y(armor_item.y));
    cell.ch = '[';
    tcod::ColorRGB color{180, 220, 200};
    cell.fg = visible ? color : dim_color(color);
  }

  for (const auto& ground_potion : level.potions) {
    bool visible = level.map.is_in_fov(ground_potion.x, ground_potion.y);
    if (!visible && !gs.reveal_all) continue;
    if (!camera.in_view(ground_potion.x, ground_potion.y)) continue;
    auto& cell =
        console.at(camera.screen_x(ground_potion.x), camera.screen_y(ground_potion.y));
    cell.ch = ground_potion.potion.glyph;
    cell.fg = visible ? ground_potion.potion.color : dim_color(ground_potion.potion.color);
  }

  for (const auto& monster : level.monsters) {
    bool visible = level.map.is_in_fov(monster.x, monster.y);
    if (!visible && !gs.reveal_all) continue;
    if (!camera.in_view(monster.x, monster.y)) continue;
    auto& cell = console.at(camera.screen_x(monster.x), camera.screen_y(monster.y));
    cell.ch = monster.glyph;
    cell.fg = visible ? monster.color : dim_color(monster.color);
  }

  // Spells currently in flight (only visible ones matter, same as monsters/items).
  for (const auto& proj : level.projectiles) {
    if (proj.path_index == 0 || proj.path_index > proj.path.size()) continue;
    auto [px, py] = proj.path[proj.path_index - 1];
    if (!level.map.is_in_fov(px, py)) continue;
    if (!camera.in_view(px, py)) continue;
    auto& cell = console.at(camera.screen_x(px), camera.screen_y(py));
    cell.ch = proj.glyph;
    cell.fg = proj.color;
  }

  // The player is always inside the viewport by construction (the camera clamp
  // keeps whatever it's centered on in view), so this is drawn unconditionally.
  console.at(camera.screen_x(gs.player.x), camera.screen_y(gs.player.y)).ch = gs.player.glyph;
  console.at(camera.screen_x(gs.player.x), camera.screen_y(gs.player.y)).fg = gs.player.color;

  // A running toggle spell (e.g. Sandstorm) gets a persistent highlight around the
  // player showing its current radius, recentered every frame since the aura
  // follows the player rather than sitting still — same recolor-not-overwrite
  // treatment as the AoE targeting preview below, so monsters/terrain inside it
  // stay visible. Uses the spell's own color so different toggle spells (if more
  // are ever added) read as visually distinct auras.
  if (gs.active_toggle_spell >= 0) {
    const Spell& storm = kSpellTable[static_cast<size_t>(gs.active_toggle_spell)];
    for (int by = gs.player.y - storm.aoe_radius; by <= gs.player.y + storm.aoe_radius; ++by) {
      for (int bx = gs.player.x - storm.aoe_radius; bx <= gs.player.x + storm.aoe_radius; ++bx) {
        if (bx < 0 || by < 0 || bx >= level.map.width() || by >= level.map.height()) continue;
        if (!level.map.is_explored(bx, by) && !gs.reveal_all) continue;
        if (!camera.in_view(bx, by)) continue;
        console.at(camera.screen_x(bx), camera.screen_y(by)).fg = storm.color;
      }
    }
    // Re-mark the player's own tile on top so they stay visible inside the tint.
    console.at(camera.screen_x(gs.player.x), camera.screen_y(gs.player.y)).fg = gs.player.color;
  }
}

void render_targeting_overlay(GameState& gs, tcod::Console& console, const Camera& camera) {
  Level& level = gs.level();
  if (gs.mode == Mode::Targeting) {
    const Spell& previewed_spell = kSpellTable[static_cast<size_t>(gs.casting_spell_index)];

    if (previewed_spell.is_swap) {
      // No projectile/line to preview for a swap — just mark the target tile,
      // colored by whether there's actually a minion there to swap with (matches
      // the Enter-fire check in own_minion_at()).
      if (camera.in_view(gs.target_x, gs.target_y)) {
        bool has_minion = own_minion_at(level.monsters, gs.target_x, gs.target_y) >= 0;
        auto& cell = console.at(camera.screen_x(gs.target_x), camera.screen_y(gs.target_y));
        cell.ch = 'X';
        cell.fg = has_minion ? tcod::ColorRGB{100, 220, 255} : tcod::ColorRGB{120, 60, 60};
      }
    } else {
      // Preview the shot: trace the same path a cast would take, and stop drawing at
      // the first tile that would actually stop it, so what you see is what you'd hit.
      auto preview = trace_path(gs.player.x, gs.player.y, gs.target_x, gs.target_y);
      for (size_t i = 0; i < preview.size(); ++i) {
        auto [x, y] = preview[i];
        bool blocked = level.map.blocks_projectile(x, y);
        bool has_monster = hostile_monster_at(level.monsters, x, y) >= 0;
        // A piercing spell's preview marks every hostile tile along the line as a
        // hit but keeps drawing the line past it — only a wall (or reaching max
        // range) actually stops it, matching advance_projectiles()'s own pierce
        // handling. Non-piercing spells are unchanged.
        bool pierce_hit = has_monster && previewed_spell.pierces;
        bool stops_here = blocked || (has_monster && !previewed_spell.pierces) || i + 1 == preview.size();
        if (camera.in_view(x, y)) {
          auto& cell = console.at(camera.screen_x(x), camera.screen_y(y));
          cell.ch = (stops_here || pierce_hit) ? 'X' : '*';
          cell.fg = (stops_here || pierce_hit) ? tcod::ColorRGB{255, 60, 60} : tcod::ColorRGB{150, 60, 60};
        }
        if (blocked || (has_monster && !previewed_spell.pierces)) break;
      }

      // AoE spells (Fireball etc.) also highlight the blast radius around wherever the
      // shot would actually come to rest — find_impact() applies the exact same
      // stopping rules advance_projectiles() uses, so this matches what firing now
      // would do. Recolors tiles rather than overwriting their glyph, so monsters/
      // terrain caught in the blast stay visible underneath the highlight.
      if (previewed_spell.aoe_radius > 0) {
        auto [impact_x, impact_y] = find_impact(preview, gs.player.x, gs.player.y, level.map, level.monsters);
        int radius = previewed_spell.aoe_radius;
        for (int by = impact_y - radius; by <= impact_y + radius; ++by) {
          for (int bx = impact_x - radius; bx <= impact_x + radius; ++bx) {
            if (bx < 0 || by < 0 || bx >= level.map.width() || by >= level.map.height()) continue;
            if (!level.map.is_explored(bx, by) && !gs.reveal_all) continue;
            if (!camera.in_view(bx, by)) continue;
            console.at(camera.screen_x(bx), camera.screen_y(by)).fg = tcod::ColorRGB{255, 140, 60};
          }
        }
        // Re-mark the impact tile on top so the center stays visually distinct.
        if (camera.in_view(impact_x, impact_y)) {
          auto& impact_cell = console.at(camera.screen_x(impact_x), camera.screen_y(impact_y));
          impact_cell.ch = 'X';
          impact_cell.fg = tcod::ColorRGB{255, 60, 60};
        }
      }
    }
  }
}

void render_ranged_overlay(GameState& gs, tcod::Console& console, const Camera& camera) {
  Level& level = gs.level();
  if (gs.mode == Mode::RangedAttack) {
    // Same aim-preview line as Mode::Targeting above, minus the AoE step — no
    // player weapon has a blast radius today, so there's nothing extra to predict.
    auto preview = trace_path(gs.player.x, gs.player.y, gs.target_x, gs.target_y);
    for (size_t i = 0; i < preview.size(); ++i) {
      auto [x, y] = preview[i];
      bool blocked = level.map.blocks_projectile(x, y);
      bool has_monster = hostile_monster_at(level.monsters, x, y) >= 0;
      bool stops_here = blocked || has_monster || i + 1 == preview.size();
      if (camera.in_view(x, y)) {
        auto& cell = console.at(camera.screen_x(x), camera.screen_y(y));
        cell.ch = stops_here ? 'X' : '*';
        cell.fg = stops_here ? tcod::ColorRGB{255, 60, 60} : tcod::ColorRGB{150, 60, 60};
      }
      if (blocked || has_monster) break;
    }
  }
}

void render_minion_focus_overlay(GameState& gs, tcod::Console& console, const Camera& camera) {
  Level& level = gs.level();
  if (gs.mode == Mode::MinionFocus) {
    // Highlights whichever minion(s) are currently being commanded — once the
    // cursor wanders away from a minion's own tile there's otherwise no way to
    // tell who you're still aiming for. Recolors the glyph (keeps it, rather than
    // overwriting with a marker) so it still reads as "that minion", just lit up.
    for (const auto& m : level.monsters) {
      if (m.allegiance != Allegiance::Player || !m.is_alive()) continue;
      if (!gs.commanding_all_minions && m.id != gs.focused_minion_id) continue;
      bool visible = level.map.is_in_fov(m.x, m.y);
      if (!visible && !gs.reveal_all) continue;  // not drawn at all this frame either way
      if (!camera.in_view(m.x, m.y)) continue;
      console.at(camera.screen_x(m.x), camera.screen_y(m.y)).fg = tcod::ColorRGB{255, 255, 100};
    }

    // If any commanded minion currently has an AttackTarget order — or an
    // Aggressive one that's actively chasing something — highlight that monster
    // too, in a color distinct from the minion tint above — with more than one of
    // the same monster type in view (two Goblins, say) there's otherwise no way to
    // tell which one is actually assigned versus just standing nearby.
    for (const auto& m : level.monsters) {
      if (m.allegiance != Allegiance::Player || !m.is_alive()) continue;
      if (!gs.commanding_all_minions && m.id != gs.focused_minion_id) continue;
      if (m.order != MinionOrder::AttackTarget && m.order != MinionOrder::Aggressive) continue;
      int ti = actor_index_by_id(level.monsters, m.attack_target_id);
      if (ti < 0) continue;  // no current target — AttackTarget will revert to Follow on its own,
                              // Aggressive just has nothing in view to chase yet
      const Actor& target = level.monsters[static_cast<size_t>(ti)];
      bool target_visible = level.map.is_in_fov(target.x, target.y);
      if (!target_visible && !gs.reveal_all) continue;
      if (!camera.in_view(target.x, target.y)) continue;
      console.at(camera.screen_x(target.x), camera.screen_y(target.y)).fg =
          tcod::ColorRGB{255, 60, 255};
    }

    // No line trace or AoE like a spell — confirming here either attacks (a
    // hostile monster under the cursor) or holds (any other walkable tile), see
    // the Enter handler below, so the cursor color previews which one: red for
    // attack (the monster's own glyph stays visible, just tinted, same as the
    // spell-targeting cursor above), green for hold, dim grey for an invalid tile
    // (a wall, or something already standing there that isn't a valid target). The
    // camera follows this cursor (see camera_focus_x/y above) so it's always in
    // view, unlike a spell's range-limited targeting cursor.
    if (camera.in_view(gs.target_x, gs.target_y)) {
      auto& cell = console.at(camera.screen_x(gs.target_x), camera.screen_y(gs.target_y));
      bool walkable = level.map.is_walkable(gs.target_x, gs.target_y);
      int hostile_hit = hostile_monster_at(level.monsters, gs.target_x, gs.target_y);
      if (hostile_hit >= 0) {
        cell.fg = tcod::ColorRGB{255, 60, 60};
      } else if (walkable && monster_at(level.monsters, gs.target_x, gs.target_y) < 0) {
        cell.ch = 'X';
        cell.fg = tcod::ColorRGB{100, 220, 140};
      } else {
        cell.ch = 'X';
        cell.fg = tcod::ColorRGB{120, 120, 120};
      }
    }
  }
}

void render_look_overlay(GameState& gs, tcod::Console& console, const Camera& camera) {
  if (gs.mode == Mode::Look) {
    // Plain recolor, no glyph override — unlike Targeting/MinionFocus there's no
    // action being previewed here, just "this is what the cursor is on", so
    // whatever's actually there (terrain/item/monster) should stay fully visible.
    if (camera.in_view(gs.target_x, gs.target_y)) {
      console.at(camera.screen_x(gs.target_x), camera.screen_y(gs.target_y)).fg =
          tcod::ColorRGB{255, 255, 255};
    }
  }
}

}  // namespace

Camera camera_for(const GameState& gs) {
  int focus_x = gs.player.x;
  int focus_y = gs.player.y;
  if (gs.mode == Mode::Targeting || gs.mode == Mode::MinionFocus || gs.mode == Mode::Look ||
      gs.mode == Mode::RangedAttack) {
    focus_x = gs.target_x;
    focus_y = gs.target_y;
  }
  const Map& map = gs.level().map;
  return Camera{std::clamp(focus_x - MAP_VIEW_W / 2, 0, std::max(0, map.width() - MAP_VIEW_W)),
                std::clamp(focus_y - MAP_VIEW_H / 2, 0, std::max(0, map.height() - MAP_VIEW_H))};
}

void render_frame(GameState& gs, tcod::Console& console) {
  console.clear();

  // Every full-screen mode takes over the whole console and returns. Mode::Playing and
  // the modes that overlay it (Targeting, RangedAttack, MinionFocus, Look, LevelUp)
  // share the sectioned HUD drawn below.
  switch (gs.mode) {
    case Mode::WeaponMenu:   render_weapon_menu(gs, console);   return;
    case Mode::ArmorMenu:    render_armor_menu(gs, console);    return;
    case Mode::PotionMenu:   render_potion_menu(gs, console);   return;
    case Mode::SpellMenu:    render_spell_menu(gs, console);    return;
    case Mode::Drop:         render_drop_screen(gs, console);   return;
    case Mode::Dead:         render_death_screen(gs, console);  return;
    case Mode::MessageLog:   render_message_log(gs, console);   return;
    case Mode::Help:         render_help(console);          return;
    case Mode::MinionRoster: render_minion_roster(gs, console); return;
    case Mode::SchoolChoice: render_school_choice(console); return;
    default: break;
  }

  update_monster_memory(gs.level());

  const Camera camera = camera_for(gs);
  render_context_row(gs, console);
  render_sidebar(gs, console);
  render_log_panel(gs, console);
  render_map_panel(gs, console, camera);

  // Overlays draw on top of the finished map panel, each guarding on its own mode. They
  // recolor cells rather than overwriting glyphs, so whatever is standing in a
  // highlighted tile stays visible underneath it.
  render_targeting_overlay(gs, console, camera);
  render_ranged_overlay(gs, console, camera);
  render_minion_focus_overlay(gs, console, camera);
  render_look_overlay(gs, console, camera);
}

// Rendering for the item/inventory menus: WeaponMenu, ArmorMenu, PotionMenu, Drop,
// Pickup. Split out of render.cpp purely to keep that file's size down (see
// render_internal.hpp and CLAUDE.md's "Known limitations" note) — reached the same way
// every other mode is, through render_frame()'s one switch. Each function still owns
// exactly one Mode, same as before the split.

#include "render.hpp"

#include "actors.hpp"
#include "content.hpp"
#include "level.hpp"
#include "render_internal.hpp"

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

// One line per item on the player's tile, with a checkbox. Parallel to
// gs.pickup_selected by index — see ground_slots_at(), whose ordering is what makes that
// safe. Describing an item reuses the same describe_* helpers the Drop screen uses.
void render_pickup_screen(GameState& gs, tcod::Console& console) {
  const Level& level = gs.level();
  auto slots = ground_slots_at(level, gs.player.x, gs.player.y);

  tcod::print(console, {0, 0}, "Pick up - letters to toggle, Enter to take, Esc to cancel",
              tcod::ColorRGB{255, 255, 255}, std::nullopt);

  int selected = 0;
  for (size_t i = 0; i < slots.size(); ++i) {
    bool on = i < gs.pickup_selected.size() && gs.pickup_selected[i];
    if (on) ++selected;
    char letter = static_cast<char>('a' + i);
    std::string name, detail;
    if (slots[i].kind == ItemKind::Weapon) {
      const Weapon& w = level.items[static_cast<size_t>(slots[i].index)].weapon;
      name = w.name;
      detail = describe_weapon(w);
    } else if (slots[i].kind == ItemKind::Armor) {
      const Armor& a = level.armor_items[static_cast<size_t>(slots[i].index)].armor;
      name = a.name;
      detail = describe_armor(a);
    } else {
      const Potion& p = level.potions[static_cast<size_t>(slots[i].index)].potion;
      name = p.name;
      detail = describe_potion(p);
    }
    std::string line = std::string("  ") + letter + ") [" + (on ? "x" : " ") + "] " + name + " (" + detail + ")";
    tcod::print(console, {0, 2 + static_cast<int>(i)}, line,
                on ? tcod::ColorRGB{220, 220, 220} : tcod::ColorRGB{130, 130, 130}, std::nullopt);
  }

  int footer = 3 + static_cast<int>(slots.size());
  tcod::print(console, {0, footer},
              "  Enter) take " + std::to_string(selected) + " selected      Shift+A) select all / none",
              tcod::ColorRGB{150, 150, 150}, std::nullopt);
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

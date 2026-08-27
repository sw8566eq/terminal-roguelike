// Input handling for the item/inventory menus: WeaponMenu, ArmorMenu, PotionMenu, Drop,
// Pickup. Split out of input.cpp purely to keep that file's size down (see
// input_internal.hpp and CLAUDE.md's "Known limitations" note) — reached the same way
// every other mode is, through handle_event()'s one switch. Each function still owns
// exactly one Mode, same as before the split.

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

void handle_weapon_menu_input(GameState& gs, const SDL_Event& event) {
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
  return;
}

void handle_armor_menu_input(GameState& gs, const SDL_Event& event) {
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
  return;
}

void handle_potion_menu_input(GameState& gs, const SDL_Event& event) {
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
  return;
}

void handle_pickup_input(GameState& gs, const SDL_Event& event) {
  Level& level = gs.level();
  auto slots = ground_slots_at(level, gs.player.x, gs.player.y);

  if (event.key.key == SDLK_ESCAPE) {
    gs.mode = Mode::Playing;  // free cancel — nothing taken, no turn spent
    return;
  }

  // Shift+A flips the whole list: all-on if anything is unchecked, all-off otherwise, so
  // one key covers both "take everything" and "start from nothing". Scoped to this mode,
  // like MinionRoster's Shift+A, so it can't collide with plain 'a' in normal play.
  if (event.key.key == SDLK_A && (event.key.mod & SDL_KMOD_SHIFT)) {
    bool any_off = false;
    for (size_t i = 0; i < slots.size(); ++i) {
      if (i >= gs.pickup_selected.size() || !gs.pickup_selected[i]) any_off = true;
    }
    gs.pickup_selected.assign(slots.size(), any_off);
    return;
  }

  if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
    std::vector<ItemSlot> chosen;
    for (size_t i = 0; i < slots.size(); ++i) {
      if (i < gs.pickup_selected.size() && gs.pickup_selected[i]) chosen.push_back(slots[i]);
    }
    if (chosen.empty()) {
      gs.mode = Mode::Playing;  // nothing checked: same free cancel as Esc
      return;
    }
    pick_up_ground_items(gs, chosen);
    gs.mode = Mode::Playing;
    end_turn(gs);  // one turn total, however much was taken
    return;
  }

  // Letters toggle. Free — selecting costs nothing until Enter commits.
  if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
    size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
    if (idx < slots.size() && idx < gs.pickup_selected.size()) {
      gs.pickup_selected[idx] = !gs.pickup_selected[idx];
    }
    return;
  }
}

void handle_drop_input(GameState& gs, const SDL_Event& event) {
  Level& level = gs.level();
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
  return;
}

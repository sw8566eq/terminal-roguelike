#pragma once

#include <libtcod.hpp>
#include <string>

// A melee weapon: damage is dice_count dice of dice_sides sides each, plus a
// flat bonus. E.g. {"Short Sword", 1, 6, 0} is a 1d6.
struct Weapon {
  std::string name;
  int dice_count = 1;
  int dice_sides = 2;
  int bonus = 0;
  // True for weapons that aren't real pickups (e.g. bare fists, monster bite/claws) —
  // these don't get returned to the player's inventory when swapped out.
  bool is_intrinsic = false;
};

// Rolls this weapon's damage for one attack.
int roll_damage(const Weapon& weapon);

// Worn armor: a flat reduction applied to damage from any attack that actually lands
// (evasion is checked separately, before this). E.g. {"Chainmail", 3} soaks 3 damage
// per hit.
struct Armor {
  std::string name;
  int defense = 0;
  // True for armor that isn't a real pickup (bare skin) — doesn't get returned to the
  // player's inventory when swapped out.
  bool is_intrinsic = false;
};

// Which attribute a temporary-buff potion raises. None means the potion doesn't buff a
// stat at all (e.g. Heal Potion).
enum class StatKind { None, Strength, Dexterity, Intelligence };

// A consumable potion: drinking it applies its effect immediately and uses it up —
// unlike Weapon/Armor, there's nothing to equip or swap back out. A potion is either a
// heal (heal_percent > 0) or a temporary stat buff (buff_stat != StatKind::None); the
// table never mixes both on one entry.
struct Potion {
  std::string name;
  int heal_percent = 0;  // percent of max HP restored, instantly, when drunk
  StatKind buff_stat = StatKind::None;  // which stat this potion temporarily raises
  int buff_amount = 0;                  // how much (e.g. +5)
  int buff_turns = 0;                   // how long, in turns, before it wears off
  char glyph = '!';
  tcod::ColorRGB color{255, 255, 255};
};

// A living thing on the map: the player or a monster. Combat is symmetric
// (same stats, same roll_damage call on both sides), so both share this one
// representation for now rather than separate Player/Monster types.
struct Actor {
  int x = 0;
  int y = 0;
  int hp = 1;
  int max_hp = 1;
  char glyph = '?';
  tcod::ColorRGB color{255, 255, 255};
  std::string name;
  Weapon weapon;
  Armor armor;

  // Player-progression stats; monsters leave these at their defaults. Strength drives
  // both max HP and melee damage (no separate Vitality stat). Dexterity drives
  // evasion (see below). Intelligence is tracked now but doesn't affect anything
  // mechanically yet — that's a follow-up system.
  int strength = 1;
  int dexterity = 1;
  int intelligence = 1;
  int level = 1;
  int xp = 0;

  int xp_reward = 0;  // monsters only: XP granted to the player on kill

  // Percent chance (0-100) to completely avoid an incoming attack. For the player this
  // is recomputed from dexterity whenever it changes; monsters get a fixed value per
  // template at spawn (monsters don't wear armor, so this is their only defense).
  int evasion = 0;

  // Player only: fractional HP banked toward the next point of passive regen (HP/turn
  // is usually not a whole number, so this carries the remainder between turns).
  float hp_regen_accumulator = 0.0f;

  // Player only: temporary stat bonuses from stat potions (Potion of Strength/
  // Dexterity/Intelligence), and turns remaining before each reverts. Ticked down once
  // per turn in end_turn(); drinking another potion of the same stat while one is
  // already active just refreshes the timer rather than stacking the bonus. Strength's
  // bonus feeds max_hp_for_strength() and melee damage the same as the permanent stat —
  // but unlike leveling up, gaining or losing it never changes current HP, only the
  // ceiling. Intelligence's bonus feeds spell damage, but deliberately NOT
  // known_spell_indices() — only permanent, unmodified intelligence unlocks new spells.
  int temp_str_bonus = 0;
  int temp_str_turns = 0;
  int temp_dex_bonus = 0;
  int temp_dex_turns = 0;
  int temp_int_bonus = 0;
  int temp_int_turns = 0;

  bool is_alive() const { return hp > 0; }
};

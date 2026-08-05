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

  bool is_alive() const { return hp > 0; }
};

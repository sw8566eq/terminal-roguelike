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

  // Player-progression stats; monsters leave these at their defaults. Strength drives
  // both max HP and melee damage (no separate Vitality stat). Dexterity and
  // Intelligence are tracked now but don't affect anything mechanically yet —
  // evasion/accuracy and INT-driven systems are a follow-up.
  int strength = 1;
  int dexterity = 1;
  int intelligence = 1;
  int level = 1;
  int xp = 0;

  int xp_reward = 0;  // monsters only: XP granted to the player on kill

  bool is_alive() const { return hp > 0; }
};

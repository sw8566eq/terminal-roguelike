#include "entity.hpp"

#include "rng.hpp"

int roll_damage(const Weapon& weapon) { return roll_dice(weapon.dice_count, weapon.dice_sides) + weapon.bonus; }

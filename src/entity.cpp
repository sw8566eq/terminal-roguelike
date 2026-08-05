#include "entity.hpp"

#include "rng.hpp"

int roll_damage(const Weapon& weapon) {
  int total = weapon.bonus;
  for (int i = 0; i < weapon.dice_count; ++i) {
    total += random_int(1, weapon.dice_sides);
  }
  return total;
}

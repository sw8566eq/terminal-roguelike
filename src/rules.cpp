#include "rules.hpp"

#include <algorithm>
#include <cmath>

#include "rng.hpp"

int max_hp_for_level_and_strength(int level, int strength) {
  return 10 + (level - 1) * kHpPerLevel + strength * kHpPerStrength;
}

int max_mana_for_intelligence(int intelligence) { return 4 + static_cast<int>(std::ceil(1.8 * intelligence)); }

int evasion_for_dexterity(int dexterity) { return dexterity * kDodgePerDexPoint; }

int total_actions_for(const Actor& actor) {
  return std::max(1, 1 + actor.extra_actions + actor.temp_extra_actions_bonus);
}

int xp_needed_for_level(int level) { return level * 20; }

int reanimated_hp(int living_max_hp) {
  // Integer ceiling of living_max_hp * pct / 100, and clamped to at least 1 so a
  // hypothetical 0 HP row can't produce a minion that's dead on arrival.
  int hp = (living_max_hp * kReanimatedHpPercent + 99) / 100;
  return std::max(1, hp);
}

int accuracy_roll(const Actor& attacker, int hit_dice_count, int hit_dice_sides) {
  return (attacker.dexterity + attacker.temp_dex_bonus) * kAccuracyPerDexPoint +
         roll_dice(hit_dice_count, hit_dice_sides);
}

int dodge_chance_vs_accuracy(const Actor& defender, int accuracy) {
  return std::clamp(kDodgeBaseline + defender.evasion - accuracy, kDodgeFloor, kDodgeCeiling);
}

int dodge_chance(const Actor& defender, const Actor& attacker, const Weapon& weapon) {
  return dodge_chance_vs_accuracy(defender, accuracy_roll(attacker, weapon.hit_dice_count, weapon.hit_dice_sides));
}

int damage_bonus_for(const Actor& attacker, const Weapon& weapon) {
  if (weapon.attack_range > 1) return (attacker.dexterity + attacker.temp_dex_bonus) / 3;
  return attacker.strength + attacker.temp_str_bonus;
}

double expected_damage(const Actor& actor, const Weapon& weapon) {
  return weapon.dice_count * (weapon.dice_sides + 1) / 2.0 + damage_bonus_for(actor, weapon);
}

double expected_spell_damage(const Actor& actor, const Spell& spell) {
  return spell.dice_count * (spell.dice_sides + 1) / 2.0 + (actor.intelligence + actor.temp_int_bonus) / 3.0;
}

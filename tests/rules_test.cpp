// Minimal, dependency-free unit tests for the pure math in src/rules.hpp — the one
// place CLAUDE.md's "Known limitations" section flags as genuinely unit-testable
// (no GameState, no window, no libtcod console): derived-stat formulas, the dodge/
// accuracy/damage math, and the small AI/regen helpers. Everything else in this project
// (combat, AI, rendering, input) still has nothing but manual playtest and the
// --seed=N + --dump-loot generation snapshot behind it — this doesn't change that, it
// just gives the one layer that's actually pure functions over an Actor/Weapon/Spell a
// real regression net too.
//
// No test framework: this project's only two dependencies are SDL3 and libtcod (see
// CLAUDE.md), and pulling in a third just to check a handful of arithmetic formulas
// would be a bigger addition than the thing being tested. `check`/`check_eq` below are
// the whole harness.
//
// Built as a separate CMake target (roguelike_tests, see CMakeLists.txt) linking only
// rules.cpp + rng.cpp — the actual dependency closure of rules.hpp's declarations, not
// the whole game — so this file can never define a second `main` for the real
// executable's link step to trip over.

#include "rules.hpp"

#include <cmath>
#include <cstdio>
#include <string>

#include "rng.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& description) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", description.c_str());
  }
}

void check_eq(int actual, int expected, const std::string& description) {
  check(actual == expected,
        description + " (expected " + std::to_string(expected) + ", got " + std::to_string(actual) + ")");
}

void check_near(double actual, double expected, const std::string& description) {
  check(std::abs(actual - expected) < 1e-9,
        description + " (expected " + std::to_string(expected) + ", got " + std::to_string(actual) + ")");
}

// --- Derived stats ---------------------------------------------------------------------

void test_max_hp_for_level_and_strength() {
  // A fresh level-1, Strength-2 character (the game's actual starting stats) has 24 max
  // HP — matching what a real headless run reports, so this doubles as a check that the
  // formula hasn't drifted from what start_new_game() actually produces.
  check_eq(max_hp_for_level_and_strength(1, 2), 24, "level 1, STR 2 (the real starting build)");
  check_eq(max_hp_for_level_and_strength(1, 0), 10, "level 1, STR 0: just the base 10");
  check_eq(max_hp_for_level_and_strength(5, 3), 10 + 4 * kHpPerLevel + 3 * kHpPerStrength,
            "level and strength terms both apply");
}

void test_max_mana_for_intelligence() {
  // Also cross-checked against real numbers already stated elsewhere: the starting
  // player (INT 2) shows 8/8 MP, and the Goblin Shaman's row is authored to match
  // max_mana_for_intelligence(3) (see content.cpp's comment on that row) = 10.
  check_eq(max_mana_for_intelligence(2), 8, "INT 2 (the real starting build)");
  check_eq(max_mana_for_intelligence(3), 10, "INT 3 (matches the Goblin Shaman's authored pool)");
  check_eq(max_mana_for_intelligence(0), 4, "INT 0: just the base 4");
}

void test_evasion_for_dexterity() {
  // Cross-checked against a real run: the starting player (DEX 2) shows Eva: 24.
  check_eq(evasion_for_dexterity(2), 24, "DEX 2 (the real starting build)");
  check_eq(evasion_for_dexterity(0), 0, "DEX 0: no evasion at all");
}

void test_total_actions_for() {
  Actor a{};
  check_eq(total_actions_for(a), 1, "a plain actor gets exactly one action");
  a.extra_actions = 1;
  check_eq(total_actions_for(a), 2, "extra_actions adds one");
  a.temp_extra_actions_bonus = 1;
  check_eq(total_actions_for(a), 3, "a Haste-style temp bonus stacks with extra_actions");
  a.extra_actions = -10;
  a.temp_extra_actions_bonus = 0;
  check_eq(total_actions_for(a), 1, "clamped to a floor of 1 even given a hypothetical negative");
}

void test_xp_needed_for_level() {
  check_eq(xp_needed_for_level(1), 20, "level 1 -> 20 XP to advance");
  check_eq(xp_needed_for_level(5), 100, "scales linearly with level");
}

// --- Regeneration / AI thresholds -------------------------------------------------------

void test_reanimated_hp() {
  // The Troll (22 max HP) is CLAUDE.md's own worked example for the reanimation power
  // gate: it should come back at 14, under the Demon's 20 max HP.
  check_eq(reanimated_hp(22), 14, "a raised Troll (22 max HP) comes back at 14 — under the Demon's 20");
  check_eq(reanimated_hp(4), 3, "rounds up, not down: 60% of 4 is 2.4, not 2");
  check_eq(reanimated_hp(0), 1, "clamped to at least 1 even for a hypothetical 0 HP row");
}

// --- Damage math (the parts with no dice roll in them) ----------------------------------

void test_damage_bonus_for() {
  Actor a{};
  a.strength = 5;
  a.temp_str_bonus = 2;
  a.dexterity = 4;
  a.temp_dex_bonus = 1;

  Weapon melee{};
  melee.attack_range = 1;
  check_eq(damage_bonus_for(a, melee), 7, "melee damage comes from strength + temp_str_bonus");

  Weapon ranged{};
  ranged.attack_range = 8;
  check_eq(damage_bonus_for(a, ranged), (4 + 1) / 3, "ranged damage comes from (dexterity + temp_dex_bonus) / 3");
}

void test_expected_damage() {
  Actor a{};
  a.strength = 3;
  Weapon w{};
  w.dice_count = 2;
  w.dice_sides = 6;
  w.attack_range = 1;
  // Average of 2d6 is 2 * (6+1)/2 = 7, plus the flat Strength bonus.
  check_near(expected_damage(a, w), 7.0 + 3.0, "average weapon dice plus the strength bonus");
}

void test_expected_spell_damage() {
  Actor a{};
  a.intelligence = 6;
  Spell s{};
  s.dice_count = 1;
  s.dice_sides = 6;
  // Average of 1d6 is 3.5, plus INT/3 (no rounding — this is an expected value, not a
  // rolled one).
  check_near(expected_spell_damage(a, s), 3.5 + 2.0, "average spell dice plus INT/3, unrounded");
}

// --- Dodge math (the deterministic half, and the dice-rolling half's guaranteed bounds) -

void test_dodge_chance_vs_accuracy() {
  Actor defender{};
  defender.evasion = 20;
  check_eq(dodge_chance_vs_accuracy(defender, 0), kDodgeBaseline + 20, "baseline + evasion, no accuracy to subtract");
  check_eq(dodge_chance_vs_accuracy(defender, 1000), kDodgeFloor, "clamped to the floor against overwhelming accuracy");

  defender.evasion = 1000;
  check_eq(dodge_chance_vs_accuracy(defender, 0), kDodgeCeiling, "clamped to the ceiling against huge evasion");
}

void test_dodge_chance_bounds() {
  // dodge_chance() rolls the weapon's hit-dice internally (via accuracy_roll()), so its
  // exact output isn't hand-checkable — but the roll is bounded, so the dodge% it
  // produces must be too. Compute both ends analytically from dodge_chance_vs_accuracy()
  // (already proven correct above) and check every roll lands inside them.
  Actor defender{};
  defender.evasion = 20;
  Actor attacker{};
  attacker.dexterity = 3;  // accuracy's flat Dexterity term: 3 * kAccuracyPerDexPoint
  Weapon w{};
  w.hit_dice_count = 2;
  w.hit_dice_sides = 4;  // rolls in [2, 8]

  int dex_term = attacker.dexterity * kAccuracyPerDexPoint;
  int min_accuracy = dex_term + w.hit_dice_count * 1;
  int max_accuracy = dex_term + w.hit_dice_count * w.hit_dice_sides;
  // Dodge% falls as accuracy rises, so the accuracy bounds flip when mapped to dodge%.
  int max_possible_dodge = dodge_chance_vs_accuracy(defender, min_accuracy);
  int min_possible_dodge = dodge_chance_vs_accuracy(defender, max_accuracy);

  for (int i = 0; i < 200; ++i) {
    int result = dodge_chance(defender, attacker, w);
    check(result >= min_possible_dodge && result <= max_possible_dodge,
          "dodge_chance() with a 2d4 weapon stays within its analytically bounded range");
  }
}

}  // namespace

int main() {
  seed_rng(12345);  // fixed seed: makes the dice-rolling tests reproducible, same spirit
                     // as this project's only other regression check (--seed=N + --dump-loot)

  test_max_hp_for_level_and_strength();
  test_max_mana_for_intelligence();
  test_evasion_for_dexterity();
  test_total_actions_for();
  test_xp_needed_for_level();
  test_reanimated_hp();
  test_damage_bonus_for();
  test_expected_damage();
  test_expected_spell_damage();
  test_dodge_chance_vs_accuracy();
  test_dodge_chance_bounds();

  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}

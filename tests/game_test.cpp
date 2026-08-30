// Phase 1 of the testing overhaul (see CLAUDE.md's "Testing overhaul" section):
// integration-level tests over GameState — resolve_attack()'s and on_actor_killed()'s
// actual side effects (HP, message log, corpses, XP, mode transitions) — as opposed to
// tests/rules_test.cpp, which already covers the pure math underneath them
// (dodge_chance(), accuracy_roll(), damage_bonus_for(), ...) with no GameState at all.
// This file is deliberately not re-deriving that math; see each test's own comment for
// exactly where the line is drawn.
//
// Built via the tests/arena.* headless helpers, over the same hand-rolled check()/
// check_eq() harness style as rules_test.cpp — see CMakeLists.txt's roguelike_game_tests
// target for the (SDL-free) dependency closure this links.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "actors.hpp"
#include "arena.hpp"
#include "content.hpp"
#include "game.hpp"
#include "level.hpp"
#include "rules.hpp"

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

// A small open floor room, walled on every side, at the map's origin — enough space for
// this phase's tests (and later phases') to place a player and a monster or two without
// touching generate()'s randomness at all.
const std::vector<std::string> kSmallRoom = {
    "#####",
    "#...#",
    "#...#",
    "#...#",
    "#####",
};

// kMonsterTable is ordered by roughly-increasing depth/toughness and could grow or be
// reordered; every test below looks a row up by name rather than hardcoding an index, so
// this file can't quietly start testing the wrong monster after a content-table edit.
int monster_index_named(const std::string& name) {
  for (size_t i = 0; i < kMonsterTable.size(); ++i) {
    if (kMonsterTable[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

// --- Phase 1: combat resolution ----------------------------------------------------

// resolve_attack() rolls a dodge internally (dodge_chance(), already unit-tested as
// pure math in tests/rules_test.cpp) — this isn't re-deriving that formula, it's
// checking resolve_attack() *wires the roll up correctly*: whichever way any given roll
// lands, exactly one of "dodged, HP unchanged" or "hit, HP dropped by a damage figure
// consistent with the weapon/armor math" must be true. Run over many trials (so both
// branches actually get exercised, regardless of which way any single roll lands)
// rather than trying to force one outcome deterministically, since forcing a specific
// dodge roll would mean depending on the RNG's internal call order — exactly the
// coupling this test is designed to not care about.
void test_resolve_attack_dodge_or_hit_invariant() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);

  Actor attacker{};
  attacker.name = "Attacker";
  attacker.dexterity = 5;
  attacker.strength = 4;
  Weapon weapon{};
  weapon.name = "Test Weapon";
  weapon.dice_count = 2;
  weapon.dice_sides = 6;
  weapon.hit_dice_count = 2;
  weapon.hit_dice_sides = 4;
  weapon.attack_range = 1;

  Actor defender{};
  defender.name = "Defender";
  defender.evasion = 10;
  defender.armor.defense = 2;

  int max_possible_damage = weapon.dice_count * weapon.dice_sides + damage_bonus_for(attacker, weapon);
  int min_possible_damage = weapon.dice_count * 1 + damage_bonus_for(attacker, weapon) - defender.armor.defense;
  check(min_possible_damage > 0,
        "test setup: every landed hit must deal at least 1 damage, so dealt==0 unambiguously means dodged");

  bool saw_dodge = false;
  bool saw_hit = false;
  for (int i = 0; i < 300; ++i) {
    defender.hp = 1000;
    defender.max_hp = 1000;
    resolve_attack(gs, attacker, defender, weapon);
    int dealt = 1000 - defender.hp;
    if (dealt == 0) {
      saw_dodge = true;
    } else {
      saw_hit = true;
      check(dealt >= min_possible_damage && dealt <= max_possible_damage,
            "a landed hit deals between the weapon's min and max possible damage");
    }
  }
  check(saw_dodge, "over 300 trials against 10 evasion, at least one attack was dodged");
  check(saw_hit, "over 300 trials with 2d6+STR damage, at least one attack landed");
}

// --- Phase 1: on_actor_killed() branch coverage -------------------------------------
//
// Every branch here is deterministic control flow (which path is taken never depends on
// a dice roll) except the corpse chance, which is handled the same many-trials way as
// the dodge test above. All of it runs from a scratch working directory (see main()) so
// the is_player/final-boss branches' append_run_history_entry() calls can be exercised
// for real without any risk of writing into the actual project's run_history.txt — that
// file is a fixed relative path (run_history.hpp), exactly like load_best_tileset()'s
// "run from repo root" font-loading assumption.

void test_on_actor_killed_player_death() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  Actor victim = gs.player;  // on_actor_killed() reads is_player off the victim, not gs.player
  victim.is_player = true;
  on_actor_killed(gs, victim, /*killed_by_player_side=*/false, "A Test Monster");
  check(gs.mode == Mode::Dead, "killing the player routes to Mode::Dead");
  check(gs.death_cause == "A Test Monster", "death_cause records the cause string passed in");
}

void test_on_actor_killed_final_boss_wins_regardless_of_killer() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int overlord_index = monster_index_named("Dungeon Overlord");
  check(overlord_index >= 0, "test setup: kMonsterTable still has a Dungeon Overlord row");
  Actor victim = spawn_monster(overlord_index, 1, 1);
  // killed_by_player_side=false on purpose: the final boss's death ends the run "whoever
  // or whatever actually landed the blow" (see CLAUDE.md's Win condition section) — this
  // is the one thing this test is actually checking, not just that killing it wins.
  on_actor_killed(gs, victim, /*killed_by_player_side=*/false, "irrelevant");
  check(gs.mode == Mode::Win, "killing the final boss routes to Mode::Win even if a monster landed the blow");
  check(gs.win_cause == "Dungeon Overlord", "win_cause records the final boss's own name, not the cause argument");
}

void test_on_actor_killed_ordinary_boss_never_leaves_corpse() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int warlord_index = monster_index_named("Orc Warlord");
  check(warlord_index >= 0, "test setup: kMonsterTable still has an Orc Warlord row");
  for (int i = 0; i < 100; ++i) {
    gs.level().corpses.clear();
    Actor victim = spawn_monster(warlord_index, 1, 1);
    on_actor_killed(gs, victim, /*killed_by_player_side=*/true, "you");
    check(gs.level().corpses.empty(), "is_boss always wins over the corpse roll, no matter how many trials");
  }
}

void test_on_actor_killed_leaves_corpse_false_never_leaves_one() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int skeleton_index = monster_index_named("Skeleton");
  check(skeleton_index >= 0, "test setup: kMonsterTable still has a Skeleton row");
  for (int i = 0; i < 100; ++i) {
    gs.level().corpses.clear();
    Actor victim = spawn_monster(skeleton_index, 1, 1);
    on_actor_killed(gs, victim, /*killed_by_player_side=*/true, "you");
    check(gs.level().corpses.empty(), "leaves_corpse=false (animated bones) never leaves a body, over 100 trials");
  }
}

void test_on_actor_killed_ordinary_hostile_sometimes_leaves_corpse_and_grants_xp() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int rat_index = monster_index_named("Rat");
  check(rat_index >= 0, "test setup: kMonsterTable still has a Rat row");
  int rat_xp_reward = kMonsterTable[static_cast<size_t>(rat_index)].xp_reward;
  check(rat_xp_reward > 0 && rat_xp_reward < xp_needed_for_level(1),
        "test setup: a single Rat kill shouldn't be enough XP to also trigger a level-up");

  bool saw_corpse = false;
  bool saw_no_corpse = false;
  for (int i = 0; i < 200; ++i) {
    gs.level().corpses.clear();
    gs.player.xp = 0;
    Actor victim = spawn_monster(rat_index, 1, 1);
    on_actor_killed(gs, victim, /*killed_by_player_side=*/true, "you");
    check(gs.player.xp == rat_xp_reward, "killed_by_player_side grants exactly the victim's xp_reward");
    if (gs.level().corpses.empty()) {
      saw_no_corpse = true;
    } else {
      saw_corpse = true;
      check(gs.level().corpses.size() == 1 && gs.level().corpses[0].monster_template_index == rat_index,
            "a left corpse records the victim's own species");
    }
  }
  check(saw_corpse, "over 200 trials at kCorpseChancePercent=50, at least one Rat left a corpse");
  check(saw_no_corpse, "over 200 trials at kCorpseChancePercent=50, at least one Rat did not");
}

void test_on_actor_killed_no_xp_when_not_killed_by_player_side() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int rat_index = monster_index_named("Rat");
  gs.player.xp = 0;
  Actor victim = spawn_monster(rat_index, 1, 1);
  // Mirrors a hostile monster killing another hostile, or a hostile killing a minion —
  // resolve_attack() computes killed_by_player_side from the *attacker*, so this is the
  // shape a hostile-vs-minion kill actually takes; on_actor_killed() itself has no
  // allegiance check on the victim for XP, only for the corpse-eligibility branch.
  on_actor_killed(gs, victim, /*killed_by_player_side=*/false, "another monster");
  check(gs.player.xp == 0, "no XP flows to the player when the kill wasn't credited to their side");
}

}  // namespace

int main() {
  // Never let a test's on_actor_killed() call touch the real run_history.txt — see the
  // block comment above test_on_actor_killed_player_death().
  std::filesystem::path scratch = std::filesystem::temp_directory_path() / "roguelike_game_tests_scratch";
  std::filesystem::create_directories(scratch);
  std::filesystem::current_path(scratch);

  test_resolve_attack_dodge_or_hit_invariant();
  test_on_actor_killed_player_death();
  test_on_actor_killed_final_boss_wins_regardless_of_killer();
  test_on_actor_killed_ordinary_boss_never_leaves_corpse();
  test_on_actor_killed_leaves_corpse_false_never_leaves_one();
  test_on_actor_killed_ordinary_hostile_sometimes_leaves_corpse_and_grants_xp();
  test_on_actor_killed_no_xp_when_not_killed_by_player_side();

  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}

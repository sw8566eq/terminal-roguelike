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
#include "map.hpp"
#include "projectile.hpp"
#include "rng.hpp"
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

// A straight one-tile-wide corridor, long enough to line up several targets in a row —
// what the piercing/AoE/friendly-fire projectile tests below need, that kSmallRoom is
// too small for.
const std::vector<std::string> kCorridor = {
    "########",
    "#......#",
    "########",
};

// A bare-bones Projectile with every field a real cast/fire sets, tuned so a hit is
// overwhelmingly likely (huge accuracy_bonus against 0 evasion) — these tests are about
// advance_projectiles()'s travel/stopping/multi-target logic, not re-proving the dodge
// formula (already covered by rules_test.cpp and this file's own resolve_attack test
// above), so nearly-guaranteed hits keep them from being incidentally flaky.
Projectile make_test_projectile(int from_x, int from_y, int to_x, int to_y, Allegiance owner) {
  Projectile proj;
  proj.path = trace_path(from_x, from_y, to_x, to_y);
  proj.prev_x = from_x;
  proj.prev_y = from_y;
  proj.speed = kInstantSpellSpeed;
  proj.dice_count = 2;
  proj.dice_sides = 6;
  proj.bonus = 3;
  proj.hit_dice_count = 4;
  proj.hit_dice_sides = 6;
  proj.accuracy_bonus = 500;
  proj.name = "Test Bolt";
  proj.owner_allegiance = owner;
  proj.owner_is_player = (owner == Allegiance::Player);
  proj.owner_name = proj.owner_is_player ? "you" : "the monster";
  return proj;
}

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

// --- Phase 1: projectile mechanics --------------------------------------------------

// A projectile with speed < kInstantSpellSpeed only advances that many tiles per call to
// advance_projectiles() — the mechanism that makes Fireball's orb visibly cross several
// turns instead of resolving instantly (see Spells' "Turn-based projectile travel, not
// animation" note). One call per step, asserting path_index only ever grows by exactly
// `speed`, and the target is untouched until the projectile's path actually reaches it.
void test_advance_projectiles_slow_travel_per_turn() {
  GameState gs = arena::make_gamestate(kCorridor, 1, 1);
  int rat_index = monster_index_named("Rat");
  int target_id = arena::place_monster(gs, rat_index, 6, 1);
  Actor* target = arena::find_actor(gs, target_id);
  target->hp = target->max_hp = 1000;

  Projectile proj = make_test_projectile(1, 1, 6, 1, Allegiance::Player);
  proj.speed = 1;
  size_t path_len = proj.path.size();
  check(path_len == 5, "trace_path from (1,1) to (6,1) covers x=2..6, five tiles");
  gs.level().projectiles.push_back(proj);

  for (int step = 1; step < static_cast<int>(path_len); ++step) {
    advance_projectiles(gs);
    check(gs.level().projectiles.size() == 1, "a speed-1 projectile short of its target isn't consumed yet");
    check(gs.level().projectiles[0].path_index == static_cast<size_t>(step),
          "path_index advances by exactly one tile per call, not the whole path at once");
    check(target->hp == 1000, "the target is untouched until the projectile's path actually reaches it");
  }
  advance_projectiles(gs);  // the final step: reaches the target's own tile
  check(gs.level().projectiles.empty(), "reaching the target's tile consumes a non-piercing projectile");
}

// explode() (aoe_radius > 0) hits every living monster within Chebyshev radius of the
// impact tile, independently rolled per target, and leaves anything farther away
// completely untouched — the untouched half is a deterministic invariant regardless of
// any roll, so it's checked on every trial; the "does the blast really reach radius 1"
// half needs many trials, the same reasoning as the resolve_attack() dodge-or-hit test
// above (accuracy_bonus is tuned deliberately huge so a hit is overwhelmingly likely).
void test_advance_projectiles_aoe_explode_radius() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int rat_index = monster_index_named("Rat");
  int center_id = arena::place_monster(gs, rat_index, 3, 1);  // the impact tile itself
  int near_id = arena::place_monster(gs, rat_index, 3, 2);    // Chebyshev distance 1: in radius
  int far_id = arena::place_monster(gs, rat_index, 1, 3);     // Chebyshev distance 2: out of radius

  bool saw_hit_center = false;
  bool saw_hit_near = false;
  for (int trial = 0; trial < 200; ++trial) {
    Actor* center = arena::find_actor(gs, center_id);
    Actor* near = arena::find_actor(gs, near_id);
    Actor* far = arena::find_actor(gs, far_id);
    center->hp = center->max_hp = 1000;
    center->evasion = near->evasion = 0;
    near->hp = near->max_hp = 1000;
    far->hp = far->max_hp = 1000;

    Projectile proj = make_test_projectile(1, 1, 3, 1, Allegiance::Player);
    proj.aoe_radius = 1;
    gs.level().projectiles.push_back(proj);
    advance_projectiles(gs);

    check(far->hp == 1000, "a monster outside aoe_radius is never touched, no matter how the rolls land");
    if (center->hp < 1000) saw_hit_center = true;
    if (near->hp < 1000) saw_hit_near = true;
  }
  check(saw_hit_center, "over 200 trials, the blast's own epicenter was hit at least once");
  check(saw_hit_near, "over 200 trials, a monster at radius 1 (not the epicenter) was hit at least once");
}

// A piercing spell (Lightning Bolt) keeps traveling through every hostile on its line
// instead of stopping at the first one, resolving an independent hit against each.
// Tracks whether each of three lined-up targets was *ever* hit across many trials — the
// only way to confirm the beam reaches the second and third targets, not just the one
// closest to the caster.
void test_advance_projectiles_pierce_hits_everyone_in_line() {
  GameState gs = arena::make_gamestate(kCorridor, 1, 1);
  int rat_index = monster_index_named("Rat");
  int id_a = arena::place_monster(gs, rat_index, 2, 1);
  int id_b = arena::place_monster(gs, rat_index, 4, 1);
  int id_c = arena::place_monster(gs, rat_index, 6, 1);

  bool saw_hit[3] = {false, false, false};
  for (int trial = 0; trial < 200; ++trial) {
    int ids[3] = {id_a, id_b, id_c};
    for (int id : ids) {
      Actor* a = arena::find_actor(gs, id);
      a->hp = a->max_hp = 1000;
      a->evasion = 0;
    }

    Projectile proj = make_test_projectile(1, 1, 6, 1, Allegiance::Player);
    proj.pierces = true;
    gs.level().projectiles.push_back(proj);
    advance_projectiles(gs);
    check(gs.level().projectiles.empty(), "a piercing shot is still consumed once it runs off the end of its path");

    for (int i = 0; i < 3; ++i) {
      if (arena::find_actor(gs, ids[i])->hp < 1000) saw_hit[i] = true;
    }
  }
  check(saw_hit[0], "over 200 trials, the nearest target in the line was hit at least once");
  check(saw_hit[1], "over 200 trials, the middle target was hit — proving pierce didn't stop at the first");
  check(saw_hit[2], "over 200 trials, the farthest target was hit — pierce reaches the whole line");
}

// Friendly fire never happens, in either direction: a player-owned shot passing over the
// player's own minion doesn't target it and keeps flying past to a hostile beyond it; a
// hostile-owned shot passing over another hostile does the same and keeps flying toward
// the player. Both halves are deterministic — projectile_target_at() excludes an exact
// allegiance match outright, no roll involved — so a single trial is enough.
void test_advance_projectiles_friendly_fire_immunity() {
  {
    GameState gs = arena::make_gamestate(kCorridor, 1, 1);
    int minion_id = arena::place_minion(gs, kMinionTable[0], 3, 1);  // sits directly on the flight path
    int hostile_id = arena::place_monster(gs, monster_index_named("Rat"), 6, 1);
    Actor* minion = arena::find_actor(gs, minion_id);
    Actor* hostile = arena::find_actor(gs, hostile_id);
    minion->hp = minion->max_hp = 1000;
    hostile->hp = hostile->max_hp = 1000;

    Projectile proj = make_test_projectile(1, 1, 6, 1, Allegiance::Player);
    gs.level().projectiles.push_back(proj);
    advance_projectiles(gs);

    check(minion->hp == 1000, "a player-owned shot never targets the player's own minion in its path");
    check(gs.level().projectiles.empty(), "it still keeps flying past the minion and is consumed by the hostile beyond it");
  }
  {
    GameState gs = arena::make_gamestate(kCorridor, 1, 1);
    // A hostile "shooter" at (6,1) firing at the player at (1,1); another hostile sits at
    // (3,1), directly on that line.
    int bystander_id = arena::place_monster(gs, monster_index_named("Rat"), 3, 1);
    Actor* bystander = arena::find_actor(gs, bystander_id);
    bystander->hp = bystander->max_hp = 1000;
    gs.player.hp = gs.player.max_hp = 1000;

    Projectile proj = make_test_projectile(6, 1, 1, 1, Allegiance::Hostile);
    gs.level().projectiles.push_back(proj);
    advance_projectiles(gs);

    check(bystander->hp == 1000, "a hostile-owned shot never targets another hostile standing in its path");
    check(gs.level().projectiles.empty(), "it still keeps flying past the bystander and reaches the player beyond it");
  }
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
  seed_rng(12345);  // fixed seed: a failure among the many-trials tests below should be
                     // reproducible, the same spirit as rules_test.cpp's own seeding and
                     // this project's --seed=N + --dump-loot regression check.

  // Never let a test's on_actor_killed() call touch the real run_history.txt — see the
  // block comment above test_on_actor_killed_player_death().
  std::filesystem::path scratch = std::filesystem::temp_directory_path() / "roguelike_game_tests_scratch";
  std::filesystem::create_directories(scratch);
  std::filesystem::current_path(scratch);

  test_resolve_attack_dodge_or_hit_invariant();
  test_advance_projectiles_slow_travel_per_turn();
  test_advance_projectiles_aoe_explode_radius();
  test_advance_projectiles_pierce_hits_everyone_in_line();
  test_advance_projectiles_friendly_fire_immunity();
  test_on_actor_killed_player_death();
  test_on_actor_killed_final_boss_wins_regardless_of_killer();
  test_on_actor_killed_ordinary_boss_never_leaves_corpse();
  test_on_actor_killed_leaves_corpse_false_never_leaves_one();
  test_on_actor_killed_ordinary_hostile_sometimes_leaves_corpse_and_grants_xp();
  test_on_actor_killed_no_xp_when_not_killed_by_player_side();

  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}

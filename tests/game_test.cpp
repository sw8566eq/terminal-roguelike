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
#include <fstream>
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
#include "run_history.hpp"
#include "turn.hpp"

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

// --- apply_potion() / try_actor_use_potion() (game.cpp) -----------------------------
//
// apply_potion() is the single definition of what every potion does, shared by the
// player's own q menu and a monster/minion deciding to drink — only the heal_percent
// branch got any incidental coverage before this (via input_test.cpp's PotionMenu
// test), so the STR/DEX/INT/teleport branches and try_actor_use_potion()'s own
// decision logic were entirely untested.

int potion_index_named(const std::string& name) {
  for (size_t i = 0; i < kPotionTable.size(); ++i) {
    if (kPotionTable[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

void test_apply_potion_strength_buff_refreshes_not_stacks() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  const Potion& str_potion = kPotionTable[static_cast<size_t>(potion_index_named("Potion of Strength"))];
  int max_hp_before = gs.player.max_hp;
  apply_potion(gs, gs.player, str_potion);
  check(gs.player.temp_str_bonus == str_potion.buff_amount && gs.player.temp_str_turns == str_potion.buff_turns,
        "drinking sets the bonus and starts the timer");
  check(gs.player.max_hp == max_hp_before + str_potion.buff_amount * kHpPerStrength,
        "the max HP ceiling rises by exactly the delta");

  // Drink a second one before the first wears off: refreshes the timer, doesn't stack
  // the bonus (or the max HP delta) a second time.
  apply_potion(gs, gs.player, str_potion);
  check(gs.player.temp_str_bonus == str_potion.buff_amount, "the bonus itself doesn't compound on a second drink");
  check(gs.player.max_hp == max_hp_before + str_potion.buff_amount * kHpPerStrength,
        "max HP doesn't rise a second time either");
  check(gs.player.temp_str_turns == str_potion.buff_turns, "the timer refreshes back to the full duration");
}

void test_apply_potion_dexterity_buff() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  const Potion& dex_potion = kPotionTable[static_cast<size_t>(potion_index_named("Potion of Dexterity"))];
  int evasion_before = gs.player.evasion;
  apply_potion(gs, gs.player, dex_potion);
  check(gs.player.temp_dex_bonus == dex_potion.buff_amount && gs.player.temp_dex_turns == dex_potion.buff_turns,
        "drinking sets the bonus and starts the timer");
  check(gs.player.evasion == evasion_before + dex_potion.buff_amount * kDodgePerDexPoint,
        "evasion rises by exactly the delta");
}

void test_apply_potion_intelligence_buff() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  const Potion& int_potion = kPotionTable[static_cast<size_t>(potion_index_named("Potion of Intelligence"))];
  int max_mana_before = gs.player.max_mana;
  int expected_delta = max_mana_for_intelligence(gs.player.intelligence + int_potion.buff_amount) -
                        max_mana_for_intelligence(gs.player.intelligence);
  apply_potion(gs, gs.player, int_potion);
  check(gs.player.temp_int_bonus == int_potion.buff_amount && gs.player.temp_int_turns == int_potion.buff_turns,
        "drinking sets the bonus and starts the timer");
  check(gs.player.max_mana == max_mana_before + expected_delta, "max mana rises by exactly the delta");
}

void test_apply_potion_teleport_moves_to_a_free_tile() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  const Potion& teleport_potion = kPotionTable[static_cast<size_t>(potion_index_named("Potion of Teleportation"))];
  apply_potion(gs, gs.player, teleport_potion);
  check(gs.level().map.is_walkable(gs.player.x, gs.player.y), "teleporting lands on a walkable tile");
}

void test_apply_potion_monster_drink_message_only_if_visible() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int id = arena::place_monster(gs, monster_index_named("Rat"), 2, 1);  // in the player's small, lit room
  Actor* rat = arena::find_actor(gs, id);
  const Potion& heal_potion = kPotionTable[static_cast<size_t>(potion_index_named("Heal Potion"))];
  size_t log_before = gs.message_log.size();
  apply_potion(gs, *rat, heal_potion);
  check(gs.message_log.size() > log_before, "a monster drinking in view logs a message about it");
}

void test_try_actor_use_potion_heals_when_badly_hurt() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int id = arena::place_monster(gs, monster_index_named("Rat"), 2, 1);
  Actor* rat = arena::find_actor(gs, id);
  rat->potions.push_back(kPotionTable[static_cast<size_t>(potion_index_named("Heal Potion"))]);
  rat->hp = 1;  // well below kAiDrinkHealBelowPercent of max_hp
  bool drank = try_actor_use_potion(gs, *rat, /*enemy_near=*/false);
  check(drank, "badly hurt with a heal potion carried: it drinks");
  check(rat->potions.empty(), "the potion is consumed");
  check(rat->hp > 1, "and it actually healed");
}

void test_try_actor_use_potion_buffs_when_enemy_near() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int id = arena::place_monster(gs, monster_index_named("Rat"), 2, 1);
  Actor* rat = arena::find_actor(gs, id);
  rat->potions.push_back(kPotionTable[static_cast<size_t>(potion_index_named("Potion of Strength"))]);
  bool drank_far = try_actor_use_potion(gs, *rat, /*enemy_near=*/false);
  check(!drank_far, "a buff potion isn't wasted with nothing around to fight");
  check(!rat->potions.empty(), "the potion is still carried");

  bool drank_near = try_actor_use_potion(gs, *rat, /*enemy_near=*/true);
  check(drank_near, "an enemy near: the buff potion is worth drinking now");
  check(rat->potions.empty(), "the potion is consumed");
  check(rat->temp_str_turns > 0, "the Strength buff actually applied");
}

void test_try_actor_use_potion_false_when_nothing_wanted() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int id = arena::place_monster(gs, monster_index_named("Rat"), 2, 1);
  Actor* rat = arena::find_actor(gs, id);
  rat->potions.push_back(kPotionTable[static_cast<size_t>(potion_index_named("Heal Potion"))]);
  // Full HP, no enemy near: a carried heal potion is wanted only when badly hurt.
  bool drank = try_actor_use_potion(gs, *rat, /*enemy_near=*/false);
  check(!drank, "full HP and nothing pressing: the potion is left alone");
  check(!rat->potions.empty(), "and it's still carried");
}

// --- tick_upkeep(): regen accumulators and temp-buff expiry (turn.cpp) --------------
//
// tick_upkeep() itself is file-local to turn.cpp — exercised here through end_turn(),
// the same way every other turn.cpp behavior in this suite is (see ai_test.cpp for the
// AI side of it). A fresh arena GameState has no monsters/projectiles/toggle spell
// active, so end_turn() here exercises upkeep and nothing else.

void test_hp_and_mana_regen_accumulate_and_cap_at_max() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  gs.player.max_hp = 10;
  gs.player.hp = 5;
  gs.player.hp_regen_turns = 10;  // exactly 1.0 HP/turn — the accumulator crosses 1.0 in a single turn
  gs.player.hp_regen_accumulator = 0.0f;
  gs.player.max_mana = 10;
  gs.player.mana = 5;
  gs.player.mana_regen_turns = 10;
  gs.player.mana_regen_accumulator = 0.0f;
  end_turn(gs);
  check(gs.player.hp == 6, "HP regen adds exactly 1 HP after one turn at a 1.0/turn rate");
  check(gs.player.mana == 6, "mana regen adds exactly 1 mana after one turn at a 1.0/turn rate");

  gs.player.hp = gs.player.max_hp;
  gs.player.mana = gs.player.max_mana;
  end_turn(gs);
  check(gs.player.hp == gs.player.max_hp, "regen does nothing once already at max HP — no overheal");
  check(gs.player.mana == gs.player.max_mana, "same for mana once already at max");
}

void test_str_buff_expiry_reverts_max_hp_delta() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  gs.player.hp_regen_turns = 0;  // isolate: no regen interfering with the HP check below
  gs.player.max_hp = 100;
  gs.player.hp = 100;
  gs.player.temp_str_bonus = 5;
  gs.player.temp_str_turns = 1;
  end_turn(gs);
  check(gs.player.temp_str_bonus == 0 && gs.player.temp_str_turns == 0, "the Strength buff expires after its last turn");
  check(gs.player.max_hp == 100 - 5 * kHpPerStrength, "max HP reverts by exactly the delta the buff added");
  check(gs.player.hp == gs.player.max_hp, "current HP clamps down to the new, lower ceiling");
}

void test_dex_buff_expiry_reverts_evasion_delta() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int evasion_before = gs.player.evasion;
  gs.player.temp_dex_bonus = 3;
  gs.player.temp_dex_turns = 1;
  end_turn(gs);
  check(gs.player.temp_dex_bonus == 0 && gs.player.temp_dex_turns == 0, "the Dexterity buff expires");
  check(gs.player.evasion == evasion_before - 3 * kDodgePerDexPoint, "evasion reverts by exactly the delta");
}

void test_int_buff_expiry_reverts_max_mana_delta() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int max_mana_before = gs.player.max_mana;
  int expected_delta =
      max_mana_for_intelligence(gs.player.intelligence + 4) - max_mana_for_intelligence(gs.player.intelligence);
  gs.player.max_mana += expected_delta;  // as if the buff had already lifted the ceiling
  gs.player.mana = gs.player.max_mana;
  gs.player.temp_int_bonus = 4;
  gs.player.temp_int_turns = 1;
  end_turn(gs);
  check(gs.player.temp_int_bonus == 0 && gs.player.temp_int_turns == 0, "the Intelligence buff expires");
  check(gs.player.max_mana == max_mana_before, "max mana reverts to exactly its pre-buff ceiling");
}

void test_combat_mage_buff_expiry() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  gs.player.temp_melee_damage_bonus = 4;
  gs.player.temp_melee_damage_turns = 1;
  gs.player.temp_armor_bonus = 3;
  gs.player.temp_armor_turns = 1;
  end_turn(gs);
  check(gs.player.temp_melee_damage_bonus == 0 && gs.player.temp_melee_damage_turns == 0, "Battle Fury's buff expires");
  check(gs.player.temp_armor_bonus == 0 && gs.player.temp_armor_turns == 0, "Iron Skin's buff expires");
}

void test_haste_buff_expiry() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  gs.player.temp_extra_actions_bonus = 1;
  gs.player.temp_extra_actions_turns = 1;
  // Setting temp_extra_actions_bonus here means total_actions_for(player) is now 2 — so
  // the *first* end_turn() call is itself consumed as the free action the buff grants
  // (see end_turn()'s free-action guard) and returns before upkeep ever runs. A second
  // call is the actual world turn that ticks the buff timer down.
  end_turn(gs);
  check(gs.player.temp_extra_actions_bonus == 1, "the first call is spent as the buff's own free action; upkeep hasn't run yet");
  end_turn(gs);
  check(gs.player.temp_extra_actions_bonus == 0 && gs.player.temp_extra_actions_turns == 0,
        "the second call is a real world turn, and Haste's buff expires on it");
}

// --- load_run_history() (run_history.cpp) -------------------------------------------
//
// append_run_history_entry() is already exercised indirectly via the on_actor_killed()
// death/win tests above (from the scratch working directory main() below chdir()s
// into); this is the one function nothing else in this suite calls — parsing real
// pipe-delimited lines back into RunHistoryEntry values.

void test_load_run_history_parses_real_entries() {
  std::ofstream out("run_history.txt");  // truncates whatever an earlier test left there
  out << "WIN|15|10|42|Dungeon Overlord\n";
  out << "DEAD|3|2|random|Goblin\n";
  out.close();

  auto entries = load_run_history();
  check(entries.size() == 2, "both lines parsed into entries");
  check(entries[0].won && entries[0].floor_reached == 15 && entries[0].player_level == 10 &&
            entries[0].seed_display == "42" && entries[0].cause == "Dungeon Overlord",
        "the WIN entry's fields all parse correctly");
  check(!entries[1].won && entries[1].floor_reached == 3 && entries[1].player_level == 2 &&
            entries[1].seed_display == "random" && entries[1].cause == "Goblin",
        "the DEAD entry's fields all parse correctly");
}

// --- find_impact() / projectile_possessive() / projectile_subject() (projectile.cpp) -

void test_find_impact_three_stopping_rules() {
  GameState gs = arena::make_gamestate(kCorridor, 1, 1);  // interior x=1..6
  auto path_past_the_wall = trace_path(1, 1, 10, 1);      // runs well past the corridor's real end at x=7

  auto impact_wall = find_impact(path_past_the_wall, 1, 1, gs.level().map, gs.level().monsters);
  check(impact_wall.first == 6 && impact_wall.second == 1, "a wall stops the impact one tile short of it");

  arena::place_monster(gs, monster_index_named("Rat"), 4, 1);
  auto impact_hostile = find_impact(path_past_the_wall, 1, 1, gs.level().map, gs.level().monsters);
  check(impact_hostile.first == 4 && impact_hostile.second == 1, "a hostile in the way stops the impact on its own tile");

  GameState gs2 = arena::make_gamestate(kCorridor, 1, 1);  // fresh floor, no hostile
  auto path_within_bounds = trace_path(1, 1, 5, 1);        // stays well inside the open corridor
  auto impact_end = find_impact(path_within_bounds, 1, 1, gs2.level().map, gs2.level().monsters);
  check(impact_end.first == 5 && impact_end.second == 1,
        "reaching the end of the path with nothing there stops on the final tile");
}

void test_projectile_phrasing_player_minion_and_hostile() {
  Projectile player_proj;
  player_proj.owner_is_player = true;
  player_proj.owner_allegiance = Allegiance::Player;
  player_proj.name = "Magic Dart";
  check(projectile_possessive(player_proj) == "your", "the player's own shot reads as \"your\", ignoring owner_name");
  check(projectile_subject(player_proj) == "Your Magic Dart", "...and \"Your <spell>\" as the subject");

  Projectile minion_proj;
  minion_proj.owner_is_player = false;
  minion_proj.owner_allegiance = Allegiance::Player;
  minion_proj.owner_name = "Demon";
  minion_proj.name = "Wither Curse";
  check(projectile_possessive(minion_proj) == "your Demon's", "a minion's shot reads as \"your <name>'s\"");
  check(projectile_subject(minion_proj) == "Your Demon's Wither Curse", "...and \"Your <name>'s <spell>\" as the subject");

  Projectile hostile_proj;
  hostile_proj.owner_is_player = false;
  hostile_proj.owner_allegiance = Allegiance::Hostile;
  hostile_proj.owner_name = "Goblin Shaman";
  hostile_proj.name = "Magic Dart";
  check(projectile_possessive(hostile_proj) == "the Goblin Shaman's", "a hostile's shot reads as \"the <name>'s\"");
  check(projectile_subject(hostile_proj) == "The Goblin Shaman's Magic Dart", "...and \"The <name>'s <spell>\" as the subject");
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

  test_apply_potion_strength_buff_refreshes_not_stacks();
  test_apply_potion_dexterity_buff();
  test_apply_potion_intelligence_buff();
  test_apply_potion_teleport_moves_to_a_free_tile();
  test_apply_potion_monster_drink_message_only_if_visible();
  test_try_actor_use_potion_heals_when_badly_hurt();
  test_try_actor_use_potion_buffs_when_enemy_near();
  test_try_actor_use_potion_false_when_nothing_wanted();

  test_hp_and_mana_regen_accumulate_and_cap_at_max();
  test_str_buff_expiry_reverts_max_hp_delta();
  test_dex_buff_expiry_reverts_evasion_delta();
  test_int_buff_expiry_reverts_max_mana_delta();
  test_combat_mage_buff_expiry();
  test_haste_buff_expiry();

  test_load_run_history_parses_real_entries();

  test_find_impact_three_stopping_rules();
  test_projectile_phrasing_player_minion_and_hostile();

  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}

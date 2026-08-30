// Phase 2 of the testing overhaul (see CLAUDE.md's "Testing overhaul" section): monster
// and minion AI behavior, driven through the real end_turn() pipeline (or, where a
// production function is already a pure decision over an Actor/distance —
// equip_best_weapon_for_range() — called directly, which is both simpler and more
// precise than simulating several turns of chase to provoke the same decision).
//
// Same hand-rolled check()/check_eq() harness style as rules_test.cpp/game_test.cpp —
// see CMakeLists.txt's roguelike_ai_tests target for the dependency closure.

#include <cstdio>
#include <string>
#include <vector>

#include "actors.hpp"
#include "arena.hpp"
#include "content.hpp"
#include "game.hpp"
#include "level.hpp"
#include "rng.hpp"
#include "rules.hpp"
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

int monster_index_named(const std::string& name) {
  for (size_t i = 0; i < kMonsterTable.size(); ++i) {
    if (kMonsterTable[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

// True if any message logged from `from_index` onward contains `substring` — used
// instead of checking gs.message_log.back() alone, since an instant spell/shot can log a
// second message (a dodge/hit/kill) in the same end_turn() call right after its cast/fire
// line.
bool any_message_since(const GameState& gs, size_t from_index, const std::string& substring) {
  for (size_t i = from_index; i < gs.message_log.size(); ++i) {
    if (gs.message_log[i].find(substring) != std::string::npos) return true;
  }
  return false;
}

const std::vector<std::string> kSmallRoom = {
    "#####",
    "#...#",
    "#...#",
    "#...#",
    "#####",
};

// A single-row open corridor, long enough to place monsters at controlled distances from
// each other and from the player — what the pack-alert and wander tests need, and wide
// enough that a monster placed mid-corridor has open floor to actually wander into.
const std::vector<std::string> kLongOpenCorridor = {
    "###############",
    "#.............#",
    "###############",
};

// --- Wander / chase / attack transitions ---------------------------------------------

void test_hostile_attacks_when_already_in_range() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int id = arena::place_monster(gs, monster_index_named("Rat"), 2, 1);  // adjacent to the player
  Actor* monster = arena::find_actor(gs, id);
  size_t log_before = gs.message_log.size();
  end_turn(gs);
  check(monster->x == 2 && monster->y == 1, "an already-adjacent monster attacks in place rather than moving");
  check(any_message_since(gs, log_before, "Rat") || any_message_since(gs, log_before, "you"),
        "the attack logged at least one new message this turn");
}

void test_hostile_chases_via_real_pathfinding() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int id = arena::place_monster(gs, monster_index_named("Rat"), 3, 3);  // visible, out of melee range
  Actor* monster = arena::find_actor(gs, id);
  auto expected_path = gs.level().map.find_path(3, 3, 1, 1);
  check(!expected_path.empty(), "test setup: a path exists across an open room");
  auto [expected_x, expected_y] = expected_path[0];
  end_turn(gs);
  check(monster->x == expected_x && monster->y == expected_y,
        "an out-of-range but visible monster takes exactly Map::find_path()'s first step");
}

void test_hostile_wander_vs_hold() {
  // wanders=false: never moves on an idle turn, however many pass — Actor::wanders is
  // what lets the Dungeon Overlord hold its chamber. Deterministic: the wander roll is
  // never even attempted when wanders is false, so this needs no trials.
  {
    GameState gs = arena::make_gamestate(kLongOpenCorridor, 1, 1);
    int id = arena::place_monster(gs, monster_index_named("Rat"), 13, 1);  // out of FOV_RADIUS of the player
    Actor* monster = arena::find_actor(gs, id);
    monster->wanders = false;
    for (int i = 0; i < 20; ++i) {
      end_turn(gs);
      check(monster->x == 13 && monster->y == 1, "wanders=false never moves on an idle turn, however many pass");
    }
  }
  // wanders=true (the default): a coin-flip chance per idle turn, so this needs many
  // trials rather than a single call — over enough idle turns it should eventually move.
  {
    GameState gs = arena::make_gamestate(kLongOpenCorridor, 1, 1);
    int id = arena::place_monster(gs, monster_index_named("Rat"), 13, 1);
    Actor* monster = arena::find_actor(gs, id);
    bool moved = false;
    for (int i = 0; i < 100 && !moved; ++i) {
      end_turn(gs);
      if (monster->x != 13 || monster->y != 1) moved = true;
    }
    check(moved, "wanders=true (the default) eventually moves on its own over enough idle turns");
  }
}

// Regression test for a historical bug (see CLAUDE.md's Monster AI section): a
// caster/attacker's turn always ends in `continue` from its cast/attack branch, and
// last_seen_player_x/y used to only be written further down in the chase block, which
// that early `continue` never reached — so a Goblin Shaman that spent every turn casting
// at a visible player never recorded having seen them, and wandered instead of chasing
// the instant line of sight broke.
void test_perception_recorded_even_when_turn_ends_via_cast() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int id = arena::place_monster(gs, monster_index_named("Goblin Shaman"), 3, 1);  // in range/LOS of Magic Dart
  Actor* shaman = arena::find_actor(gs, id);
  check(shaman->mana > 0, "test setup: a fresh Goblin Shaman has mana to cast with");

  end_turn(gs);  // should cast, ending its turn in `continue` — the exact shape that hid the bug
  check(shaman->last_seen_player_x == 1 && shaman->last_seen_player_y == 1,
        "perception is recorded even on a turn that ends via the cast branch's continue");
}

// --- Pack alert -------------------------------------------------------------------------

void test_pack_alert_same_species_only() {
  GameState gs = arena::make_gamestate(kLongOpenCorridor, 1, 1);
  int rat_index = monster_index_named("Rat");
  int goblin_index = monster_index_named("Goblin");

  int spotter_id = arena::place_monster(gs, rat_index, 8, 1);      // sees the player directly
  int ally_id = arena::place_monster(gs, rat_index, 13, 1);        // same species, in the spotter's alert range
  int stranger_id = arena::place_monster(gs, goblin_index, 12, 1); // different species, same range

  Actor* spotter = arena::find_actor(gs, spotter_id);
  Actor* ally = arena::find_actor(gs, ally_id);
  Actor* stranger = arena::find_actor(gs, stranger_id);

  check(distance_between(*spotter, *ally) <= kPackAlertRadius,
        "test setup: the ally is within pack-alert range of the spotter");
  check(distance_between(gs.player, *ally) > FOV_RADIUS,
        "test setup: the ally is too far to ever see the player on its own");
  check(distance_between(gs.player, *stranger) > FOV_RADIUS,
        "test setup: the stranger is too far to ever see the player on its own");

  end_turn(gs);

  check(spotter->last_seen_player_x == gs.player.x && spotter->last_seen_player_y == gs.player.y,
        "the spotter, which can actually see the player, records its position directly");
  check(ally->last_seen_player_x == gs.player.x && ally->last_seen_player_y == gs.player.y,
        "a same-species ally within pack-alert range gets the sighting planted, despite no LOS of its own");
  check(stranger->last_seen_player_x == -1 && stranger->last_seen_player_y == -1,
        "a different-species monster in an equivalent spot is NOT alerted — same-species only, by design");
}

// same_pack() (content.cpp) extends the rule above from exact-species to a shared
// MonsterTemplate::faction — a kin group like the Orc line. This is the same test
// shape as test_pack_alert_same_species_only() above, just with the "ally" being a
// genuinely different species that happens to share the spotter's faction, and the
// "stranger" being a different species with no shared faction at all (unaffected by
// this feature, still governed by the same-species fallback).
void test_pack_alert_faction_extends_to_kin_species() {
  GameState gs = arena::make_gamestate(kLongOpenCorridor, 1, 1);
  int orc_index = monster_index_named("Orc");
  int orc_archer_index = monster_index_named("Orc Archer");
  int goblin_index = monster_index_named("Goblin");

  arena::place_monster(gs, orc_index, 8, 1);                            // sees the player directly
  int kin_ally_id = arena::place_monster(gs, orc_archer_index, 13, 1);  // different species, same faction ("Orc")
  int unrelated_id = arena::place_monster(gs, goblin_index, 12, 1);     // different species, different faction

  Actor* kin_ally = arena::find_actor(gs, kin_ally_id);
  Actor* unrelated = arena::find_actor(gs, unrelated_id);
  check(distance_between(gs.player, *kin_ally) > FOV_RADIUS,
        "test setup: the kin ally is too far to ever see the player on its own");
  check(distance_between(gs.player, *unrelated) > FOV_RADIUS,
        "test setup: the unrelated monster is too far to ever see the player on its own");

  end_turn(gs);

  check(kin_ally->last_seen_player_x == gs.player.x && kin_ally->last_seen_player_y == gs.player.y,
        "an Orc Archer within pack-alert range of an alerted Orc gets the sighting planted "
        "— same faction, different species");
  check(unrelated->last_seen_player_x == -1 && unrelated->last_seen_player_y == -1,
        "a Goblin in an equivalent spot is not alerted by an Orc — different factions stay separate");
}

// --- Caster escort ----------------------------------------------------------------------
//
// A caster that can't currently cast (out of range or, here, out of mana) and isn't in
// melee range either would otherwise fall through to the same chase logic as any
// ordinary monster. If a same-pack ally is already melee-engaged with the target, it
// holds position instead — see turn.cpp's own comment on the exact guard.

void test_caster_escort_holds_when_ally_engaged() {
  GameState gs = arena::make_gamestate(kLongOpenCorridor, 1, 1);
  arena::place_monster(gs, monster_index_named("Orc"), 2, 1);  // already melee-engaged with the player
  int wizard_id = arena::place_monster(gs, monster_index_named("Orc Wizard"), 5, 1);
  Actor* wizard = arena::find_actor(gs, wizard_id);
  wizard->mana = 0;  // can't afford Magic Dart or Fireball, so it would otherwise chase

  end_turn(gs);

  check(wizard->x == 5 && wizard->y == 1,
        "a caster with a same-pack ally already melee-engaged with the target holds position "
        "instead of closing in on foot");
}

void test_caster_chases_normally_without_an_engaged_ally() {
  GameState gs = arena::make_gamestate(kLongOpenCorridor, 1, 1);
  int wizard_id = arena::place_monster(gs, monster_index_named("Orc Wizard"), 5, 1);
  Actor* wizard = arena::find_actor(gs, wizard_id);
  wizard->mana = 0;  // same as above, but with no ally at all this time

  auto expected_path = gs.level().map.find_path(5, 1, 1, 1);
  check(!expected_path.empty(), "test setup: a path exists back toward the player");
  auto [ex, ey] = expected_path[0];

  end_turn(gs);

  check(wizard->x == ex && wizard->y == ey,
        "with no pack ally already engaged, a caster that can't cast still chases normally, "
        "same as any ordinary monster");
}

// --- Goblin Slinger snipe-then-engage-then-rearm ----------------------------------------
//
// equip_best_weapon_for_range() is a pure decision over an Actor and a distance — calling
// it directly is both simpler and more precise than simulating several turns of the
// player and the Slinger both moving to provoke the same sequence of distances.

void test_goblin_slinger_snipe_then_engage_then_rearm() {
  int slinger_index = monster_index_named("Goblin Slinger");
  Actor slinger = spawn_monster(slinger_index, 0, 0);
  check(slinger.weapon.name == "Rock", "test setup: a fresh Goblin Slinger starts armed with its Rock");
  check(!slinger.melee_engaged, "test setup: not yet melee-engaged");

  equip_best_weapon_for_range(slinger, 5);  // at the Rock's own range
  check(slinger.weapon.name == "Rock", "still keeps sniping with the Rock at range 5");
  check(!slinger.melee_engaged, "not melee-engaged while still at range");

  equip_best_weapon_for_range(slinger, 1);  // the player closed to melee
  check(slinger.weapon.name == "Dagger", "forced adjacent, it draws the higher-hit-dice Dagger instead");
  check(slinger.melee_engaged, "melee_engaged flips true the moment it's fighting in melee");

  equip_best_weapon_for_range(slinger, 3);  // stepped back one tile, still within the Rock's own reach
  check(slinger.melee_engaged, "still committed to melee at range 3 — 'stepped back one tile', not 'fight's over'");
  check(slinger.weapon.name == "Dagger", "stays on the Dagger while melee-engaged, even though the Rock could reach here");

  // Beyond everything it carries (the Rock's own range is 5): melee_engaged clears, but
  // nothing is actually "usable" at distance 6 either (not even the Rock reaches that
  // far) — usable() and the re-arm threshold are the same longest_reach, so of course
  // nothing can be swapped to right at the moment the flag clears. The weapon itself
  // only visibly re-arms once the target is back within something it carries — see below.
  equip_best_weapon_for_range(slinger, 6);
  check(!slinger.melee_engaged, "retreating beyond every weapon it carries clears melee_engaged — the fight is over");

  equip_best_weapon_for_range(slinger, 5);  // back within the Rock's own range, now unengaged
  check(slinger.weapon.name == "Rock",
        "once back within something it carries and no longer melee-engaged, it re-arms its Rock");
}

// --- Orc Wizard multi-spell scoring ------------------------------------------------------

void test_orc_wizard_prefers_higher_scoring_affordable_spell() {
  int wizard_index = monster_index_named("Orc Wizard");

  {
    GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
    int id = arena::place_monster(gs, wizard_index, 3, 1);  // distance 2: well within both spells' range 8
    Actor* wizard = arena::find_actor(gs, id);
    wizard->mana = wizard->max_mana;  // affords either spell
    size_t log_before = gs.message_log.size();
    end_turn(gs);
    check(any_message_since(gs, log_before, "Fireball"),
          "with a full mana pool, it prefers Fireball — the higher expected_spell_damage() option");
  }
  {
    GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
    int id = arena::place_monster(gs, wizard_index, 3, 1);
    Actor* wizard = arena::find_actor(gs, id);
    wizard->mana = 1;  // enough for Magic Dart (1) but not Fireball (3)
    size_t log_before = gs.message_log.size();
    end_turn(gs);
    check(any_message_since(gs, log_before, "Magic Dart"),
          "with too little mana for Fireball, it falls back to Magic Dart instead");
  }
}

// --- Minion orders -----------------------------------------------------------------------

void test_minion_follow_paths_toward_player() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);  // player at (1,1)
  int id = arena::place_minion(gs, kMinionTable[0], 3, 3);
  Actor* minion = arena::find_actor(gs, id);
  check(minion->order == MinionOrder::Follow, "test setup: a freshly placed minion defaults to Follow");
  auto expected_path = gs.level().map.find_path(3, 3, 1, 1);
  check(!expected_path.empty(), "test setup: a path exists back to the player");
  auto [ex, ey] = expected_path[0];
  end_turn(gs);
  check(minion->x == ex && minion->y == ey, "a Follow minion takes Map::find_path()'s first step toward the player");
}

void test_minion_hold_paths_toward_hold_point_then_stays() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int id = arena::place_minion(gs, kMinionTable[0], 1, 3);
  Actor* minion = arena::find_actor(gs, id);
  minion->order = MinionOrder::Hold;
  minion->hold_x = 3;
  minion->hold_y = 1;
  auto expected_path = gs.level().map.find_path(1, 3, 3, 1);
  check(!expected_path.empty(), "test setup: a path exists to the hold point");
  auto [ex, ey] = expected_path[0];
  end_turn(gs);
  check(minion->x == ex && minion->y == ey,
        "a Hold minion not yet there takes Map::find_path()'s first step toward its hold point");
}

void test_minion_attack_target_engages_in_place() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int rat_id = arena::place_monster(gs, monster_index_named("Rat"), 3, 2);
  int minion_id = arena::place_minion(gs, kMinionTable[0], 2, 2);  // already adjacent to the Rat
  Actor* minion = arena::find_actor(gs, minion_id);
  minion->order = MinionOrder::AttackTarget;
  minion->attack_target_id = rat_id;
  size_t log_before = gs.message_log.size();
  end_turn(gs);
  check(minion->x == 2 && minion->y == 2, "an already-adjacent AttackTarget minion attacks in place rather than moving");
  check(gs.message_log.size() > log_before, "the attack logged at least one new message this turn");
}

void test_minion_attack_target_reverts_to_follow_when_target_dies() {
  GameState gs = arena::make_gamestate(kSmallRoom, 1, 1);
  int rat_id = arena::place_monster(gs, monster_index_named("Rat"), 3, 1);
  int minion_id = arena::place_minion(gs, kMinionTable[0], 1, 2);
  Actor* minion = arena::find_actor(gs, minion_id);
  minion->order = MinionOrder::AttackTarget;
  minion->attack_target_id = rat_id;

  // Kill the target directly rather than through combat — this test is about the order-
  // reversion logic, not about landing a killing blow. is_alive() (hp > 0) is all
  // actor_index_by_id() checks, so this takes effect immediately, before sweep_dead()
  // (end_turn()'s last phase) ever erases the corpse from the vector.
  arena::find_actor(gs, rat_id)->hp = 0;
  end_turn(gs);
  check(minion->order == MinionOrder::Follow,
        "AttackTarget reverts to Follow once actor_index_by_id() can no longer find the target");
}

// Regression test for a historical bug (see CLAUDE.md's Minion AI comments in turn.cpp):
// weapon selection for an AttackTarget minion used to always ask nearest_hostile_distance()
// — the closest hostile *anywhere on the floor* — rather than the distance to the specific
// target it was actually ordered to fight, so a merely-nearby hostile could make a ranged
// minion draw its melee weapon even while under orders to fight something farther away.
void test_minion_attack_target_weapon_selection_uses_specific_distance() {
  GameState gs = arena::make_gamestate(kLongOpenCorridor, 13, 1);  // player parked out of the way

  // A "raised" Goblin Slinger: the same shape spawn_reanimated() builds a raised minion
  // in (a live species' Actor with only allegiance/hp/xp/duration changed) — the only
  // way a minion carries a ranged weapon in this game.
  Actor raised = spawn_monster(monster_index_named("Goblin Slinger"), 1, 1);
  raised.allegiance = Allegiance::Player;
  raised.order = MinionOrder::AttackTarget;
  check(raised.weapon.name == "Rock", "test setup: a fresh Goblin Slinger minion starts armed with its Rock");
  gs.level().monsters.push_back(raised);
  int minion_id = gs.level().monsters.back().id;

  arena::place_monster(gs, monster_index_named("Rat"), 2, 1);                          // merely nearby: distance 1
  int far_target_id = arena::place_monster(gs, monster_index_named("Orc"), 6, 1);      // the actual order: distance 5

  Actor* minion = arena::find_actor(gs, minion_id);
  minion->attack_target_id = far_target_id;

  end_turn(gs);

  check(minion->weapon.name == "Rock",
        "weapon selection uses the AttackTarget's own distance (5, still in Rock range), "
        "not the merely-nearby Rat's (1)");
}

}  // namespace

int main() {
  seed_rng(12345);  // fixed seed: a failure among the many-trials tests below should be
                     // reproducible, matching rules_test.cpp/game_test.cpp's own convention.

  test_hostile_attacks_when_already_in_range();
  test_hostile_chases_via_real_pathfinding();
  test_hostile_wander_vs_hold();
  test_perception_recorded_even_when_turn_ends_via_cast();
  test_pack_alert_same_species_only();
  test_pack_alert_faction_extends_to_kin_species();
  test_caster_escort_holds_when_ally_engaged();
  test_caster_chases_normally_without_an_engaged_ally();
  test_goblin_slinger_snipe_then_engage_then_rearm();
  test_orc_wizard_prefers_higher_scoring_affordable_spell();
  test_minion_follow_paths_toward_player();
  test_minion_hold_paths_toward_hold_point_then_stays();
  test_minion_attack_target_engages_in_place();
  test_minion_attack_target_reverts_to_follow_when_target_dies();
  test_minion_attack_target_weapon_selection_uses_specific_distance();

  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}

// Phase 3 of the testing overhaul (see CLAUDE.md's "Testing overhaul" section) — the
// highest-leverage item in the whole plan relative to effort: Shift+direction travel
// (run_in_direction(), input.cpp) is the one piece of this codebase that has actually
// shipped the same class of bug three times (see README.md's "Where Claude struggled").
// This file ports the throwaway hand-drawn maps that fix was last checked against by
// hand into permanent, automated cases, using Map::paint_ascii() to hand-build the exact
// corridor/room shapes each stop condition needs.
//
// Same hand-rolled check() harness style as the rest of this suite — see CMakeLists.txt's
// roguelike_movement_tests target.

#include <cstdio>
#include <string>
#include <vector>

#include "arena.hpp"
#include "content.hpp"
#include "game.hpp"
#include "input.hpp"
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

int monster_index_named(const std::string& name) {
  for (size_t i = 0; i < kMonsterTable.size(); ++i) {
    if (kMonsterTable[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

// --- Hard stops: checked every step, the first included --------------------------------

void test_hard_stop_at_wall() {
  const std::vector<std::string> kStraightCorridor = {
      "#########",
      "#.......#",  // interior x=1..7
      "#########",
  };
  GameState gs = arena::make_gamestate(kStraightCorridor, 1, 1);
  run_in_direction(gs, 1, 0);
  check(gs.player.x == 7 && gs.player.y == 1, "travel walks all the way to the last open tile before a wall");
}

void test_hard_stop_at_occupied_tile_costs_no_turn() {
  const std::vector<std::string> kStraightCorridor = {
      "#########",
      "#.......#",
      "#########",
  };
  GameState gs = arena::make_gamestate(kStraightCorridor, 1, 1);
  arena::place_monster(gs, monster_index_named("Rat"), 2, 1);  // immediately adjacent
  size_t log_before = gs.message_log.size();
  run_in_direction(gs, 1, 0);
  check(gs.player.x == 1 && gs.player.y == 1, "a monster on the very next tile stops travel before the first step");
  check(gs.message_log.size() == log_before,
        "blocked on the first step, no turn is spent at all — end_turn() never runs");
}

// --- Soft stops: end a run in progress, but are exempt on its very first step ----------

void test_soft_stop_entering_a_room() {
  const std::vector<std::string> kCorridorIntoRoom = {
      "#########",
      "#,,,,...#",  // corridor x=1..4 (in_room=false), room x=5..7 (in_room=true)
      "#########",
  };
  GameState gs = arena::make_gamestate(kCorridorIntoRoom, 1, 1);
  run_in_direction(gs, 1, 0);
  check(gs.player.x == 4 && gs.player.y == 1,
        "a run in progress stops the tile before crossing from corridor into a room, "
        "leaving the threshold visible rather than standing in it");
}

void test_soft_stop_side_opening() {
  const std::vector<std::string> kCorridorWithBranch = {
      "##########",
      "#,,,,,,,,#",  // main corridor, y=1, x=1..8
      "#####.####",  // a branch opens downward at x=5 only
      "##########",
  };
  GameState gs = arena::make_gamestate(kCorridorWithBranch, 1, 1);
  run_in_direction(gs, 1, 0);
  check(gs.player.x == 4 && gs.player.y == 1,
        "a run in progress stops one tile before a side passage opens up, "
        "with the opening visible in front of it rather than already inside it");
}

void test_soft_stop_hostile_coming_into_view() {
  const std::vector<std::string> kLongCorridor = {
      "#################",
      "#...............#",  // interior x=1..15
      "#################",
  };
  GameState gs = arena::make_gamestate(kLongCorridor, 1, 1);
  int id = arena::place_monster(gs, monster_index_named("Rat"), 15, 1);  // distance 14: beyond FOV_RADIUS(8) at first
  // wanders=false pins it in place — monster AI still runs on every step of travel (see
  // run_in_direction()'s own comment), and a wandering Rat could shuffle closer to the
  // player on its own before it's ever actually seen, shifting exactly which tile brings
  // it into view. Without this the test's outcome depends on the RNG's wander rolls,
  // which isn't what this test is about.
  arena::find_actor(gs, id)->wanders = false;
  run_in_direction(gs, 1, 0);
  // FOV_RADIUS is 8: the first prospective tile within that of the (now-stationary)
  // monster is x=7 (distance 8) — the stop fires from the tile *before* it (x=6), the
  // same "visible one tile early" shape every soft stop has.
  check(gs.player.x == 6 && gs.player.y == 1,
        "a run in progress stops the moment the next tile would bring a hostile into view, "
        "one tile before it actually would have");
}

// The actual historical bug (see README.md's "Where Claude struggled" and CLAUDE.md's
// Movement section): soft stops used to apply on a run's very first step too, which
// silently turned "a property of the tile a run just stopped on" into "a permanent wall
// for starting a new run from that exact tile" — every soft stop became a dead end, and
// a single visible monster froze travel outright. This is the single most important test
// in this file: starting a run *from* a tile that would otherwise trigger a soft stop
// must still take that first step.
void test_first_step_exemption_prevents_deadlock() {
  const std::vector<std::string> kCorridorIntoRoom = {
      "#########",
      "#,,,,...#",
      "#########",
  };
  GameState gs = arena::make_gamestate(kCorridorIntoRoom, 4, 1);  // starting exactly at the threshold
  run_in_direction(gs, 1, 0);
  check(gs.player.x == 7 && gs.player.y == 1,
        "starting a run from the exact tile a soft stop would trigger on still takes that first "
        "step — under the historical bug this call would have moved the player nowhere at all");
}

}  // namespace

int main() {
  seed_rng(12345);  // no RNG-dependent test in this file today, but matches the rest of
                     // this suite's convention in case one is ever added here.

  test_hard_stop_at_wall();
  test_hard_stop_at_occupied_tile_costs_no_turn();
  test_soft_stop_entering_a_room();
  test_soft_stop_side_opening();
  test_soft_stop_hostile_coming_into_view();
  test_first_step_exemption_prevents_deadlock();

  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}

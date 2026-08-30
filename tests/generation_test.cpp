// Phase 4 of the testing overhaul (see CLAUDE.md's "Testing overhaul" section): turns
// two claims CLAUDE.md already records as "verified empirically across 200 test seeds"
// by a human, once, into an automated check that runs every time instead — the
// generation-side counterpart to the combat/AI suites, and a form of light fuzzing for
// generate_level() (many seeds, checking an invariant rather than an exact output, which
// is what tests/check_dump_loot_golden.sh already covers for one specific seed).
//
// Same hand-rolled check() harness style as the rest of this suite — see CMakeLists.txt's
// roguelike_generation_tests target (same headless closure as roguelike_game_tests, plus
// level.cpp's own generate_level() entry point).

#include <cstdio>
#include <string>

#include "content.hpp"
#include "game.hpp"
#include "level.hpp"
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

constexpr int kSeedsToCheck = 200;  // matches the "200 test seeds" figure already cited
                                     // in CLAUDE.md's Win condition section for this
                                     // exact claim, so a failure here is directly
                                     // comparable to what was checked by hand before.

// carve_hole_clusters() (map.cpp) is supposed to never seal the stairs behind a pit —
// every candidate patch is carved tentatively and reverted unless entry can still reach
// stairs_down afterward. This is enforced by the function itself, not by generate_level()
// separately, so this test isn't proving new production logic works; it's a permanent
// tripwire against that logic ever regressing, across far more seeds than a human would
// ever re-check by hand.
void test_ordinary_floor_stairs_always_reachable() {
  int failures_seen = 0;
  for (int seed = 1; seed <= kSeedsToCheck; ++seed) {
    seed_rng(static_cast<unsigned int>(seed));
    // Depth 3 exercises carve_hole_clusters() (every depth does) and sits below
    // kFinalFloor, so has_stairs_down is always true here.
    Level level = generate_level(MAP_WIDTH, MAP_HEIGHT, /*has_stairs_up=*/false, /*depth=*/3);
    check(level.has_stairs_down, "test setup: an ordinary depth always has real stairs down");
    auto path = level.map.find_path(level.entry_x, level.entry_y, level.stairs_down_x, level.stairs_down_y);
    if (path.empty()) ++failures_seen;
  }
  check(failures_seen == 0, "the entry can reach the stairs down on every one of " + std::to_string(kSeedsToCheck) +
                                " seeds, at a depth that carves holes");
}

// carve_special_room()'s moat is supposed to always leave the connector corridor's own
// crossing tiles as plain floor, guaranteeing a way in regardless of where the corridor
// meets the room — CLAUDE.md notes the fallback (skip the moat entirely) was "verified
// empirically never to trigger, across 200 test seeds." This test is that same check,
// automated: across kSeedsToCheck seeds, the entry must always be able to reach wherever
// the Dungeon Overlord actually ended up (its own table row places it at the special
// room's center — see generate_level()), whether or not the moat fallback ever fires.
void test_final_floor_boss_chamber_always_reachable() {
  int overlord_index = -1;
  for (size_t i = 0; i < kMonsterTable.size(); ++i) {
    if (kMonsterTable[i].name == "Dungeon Overlord") overlord_index = static_cast<int>(i);
  }
  check(overlord_index >= 0, "test setup: kMonsterTable still has a Dungeon Overlord row");

  int failures_seen = 0;
  for (int seed = 1; seed <= kSeedsToCheck; ++seed) {
    seed_rng(static_cast<unsigned int>(seed));
    Level level = generate_level(MAP_WIDTH, MAP_HEIGHT, /*has_stairs_up=*/true, /*depth=*/kFinalFloor);
    check(!level.has_stairs_down, "test setup: kFinalFloor never has real stairs down");

    const Actor* overlord = nullptr;
    for (const auto& m : level.monsters) {
      if (m.monster_template_index == overlord_index) overlord = &m;
    }
    check(overlord != nullptr, "the Dungeon Overlord actually spawned on this seed's kFinalFloor");
    if (overlord == nullptr) continue;

    auto path = level.map.find_path(level.entry_x, level.entry_y, overlord->x, overlord->y);
    if (path.empty()) ++failures_seen;
  }
  check(failures_seen == 0, "the entry can reach the Dungeon Overlord's chamber on every one of " +
                                std::to_string(kSeedsToCheck) + " seeds");
}

}  // namespace

int main() {
  test_ordinary_floor_stairs_always_reachable();
  test_final_floor_boss_chamber_always_reachable();

  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}

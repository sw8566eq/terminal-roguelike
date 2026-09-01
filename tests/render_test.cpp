// Phase 5 of the testing overhaul (see CLAUDE.md's "Testing overhaul" section): crash
// coverage, not visual correctness — the user's standing preference (see the
// prefer-playtest-instructions memory) is that interactive/visual verification happens
// via playtest instructions, not by Claude eyeballing pixels. This is a different
// question: does render_frame() survive being asked to draw every Mode at all, without
// asserting anything about what it actually looks like. Exactly the kind of "forgot to
// handle Mode::X" bug a switch-based dispatch is prone to.
//
// render_frame(GameState&, tcod::Console&) takes no SDL type and calls no SDL runtime
// function — confirmed before writing this file, same as every other headless target
// here — so this needs nothing but libtcod's headers/lib, no window, no font, no
// SDL_Init(). A crash (segfault) can't be caught by this file's own code (that's not
// what C++ exceptions are for); it doesn't need to be — ctest reports a signal-killed
// process as a failure on its own, and each mode is announced to stderr (flushed before
// the risky call) specifically so a real crash's log still names which Mode did it.

#include <cstdio>
#include <libtcod.hpp>
#include <string>

#include "arena.hpp"
#include "content.hpp"
#include "game.hpp"
#include "render.hpp"

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

const char* mode_name(Mode mode) {
  switch (mode) {
    case Mode::StartMenu: return "StartMenu";
    case Mode::SetSeed: return "SetSeed";
    case Mode::RunHistory: return "RunHistory";
    case Mode::Playing: return "Playing";
    case Mode::WeaponMenu: return "WeaponMenu";
    case Mode::ArmorMenu: return "ArmorMenu";
    case Mode::PotionMenu: return "PotionMenu";
    case Mode::Drop: return "Drop";
    case Mode::Dead: return "Dead";
    case Mode::Win: return "Win";
    case Mode::LevelUp: return "LevelUp";
    case Mode::SchoolChoice: return "SchoolChoice";
    case Mode::SpellMenu: return "SpellMenu";
    case Mode::Targeting: return "Targeting";
    case Mode::MessageLog: return "MessageLog";
    case Mode::Help: return "Help";
    case Mode::MinionRoster: return "MinionRoster";
    case Mode::MinionFocus: return "MinionFocus";
    case Mode::MinionAbilityMenu: return "MinionAbilityMenu";
    case Mode::Pickup: return "Pickup";
    case Mode::Look: return "Look";
    case Mode::RangedAttack: return "RangedAttack";
  }
  return "???";
}

// All 22 Mode values, spelled out rather than looped over a numeric range — this is a
// list of the enum's own members, not an assumption about how they're numbered, and a
// newly-added Mode has to be added here deliberately rather than silently picked up (or
// silently skipped) by a range guess.
constexpr Mode kAllModes[] = {
    Mode::StartMenu,     Mode::SetSeed,          Mode::RunHistory,         Mode::Playing,
    Mode::WeaponMenu,    Mode::ArmorMenu,        Mode::PotionMenu,         Mode::Drop,
    Mode::Dead,          Mode::Win,              Mode::LevelUp,            Mode::SchoolChoice,
    Mode::SpellMenu,     Mode::Targeting,        Mode::MessageLog,         Mode::Help,
    Mode::MinionRoster,  Mode::MinionFocus,      Mode::MinionAbilityMenu, Mode::Pickup,
    Mode::Look,          Mode::RangedAttack,
};

// monster_index_named() is arena.hpp's shared by-name lookup (tests/arena.hpp).

// A single richly-populated GameState reused across every Mode — real inventory, a real
// hostile, a real minion with an ability, spells known, a pending level-up — so each
// render function has something real to iterate rather than the trivially-empty case,
// which is the shape actual play looks like by the time a player has opened most of
// these screens at least once.
GameState build_populated_gamestate() {
  const std::vector<std::string> kRoom = {
      "##########",
      "#........#",
      "#........#",
      "#........#",
      "#........#",
      "##########",
  };
  GameState gs = arena::make_gamestate(kRoom, 2, 2);

  gs.player.intelligence = 10;  // most spells known, for SpellMenu/school-gated content
  gs.player.chosen_school = SpellSchool::Summoner;
  gs.player.weapons.push_back(kWeaponTable[0]);
  gs.player.armors.push_back(kArmorTable[0]);
  gs.player.potions.push_back(kPotionTable[0]);
  gs.pending_attribute_points = 1;  // LevelUp has something to prompt for

  arena::place_monster(gs, monster_index_named("Rat"), 6, 3);

  int demon_id = arena::place_minion(gs, kMinionTable[1], 4, 2);  // has an ability (Wither Curse)
  gs.focused_minion_id = demon_id;

  gs.target_x = gs.player.x;
  gs.target_y = gs.player.y;
  gs.casting_spell_index = 0;

  add_message(gs, "A test message, so the log panel has something to wrap/scroll.");
  return gs;
}

void test_render_frame_survives_every_mode() {
  tcod::Console console{SCREEN_WIDTH, SCREEN_HEIGHT};
  GameState gs = build_populated_gamestate();

  for (Mode mode : kAllModes) {
    gs.mode = mode;
    std::fprintf(stderr, "rendering Mode::%s...\n", mode_name(mode));
    std::fflush(stderr);  // so a real crash's log still names which Mode did it
    render_frame(gs, console);
  }
  check(true, "render_frame() survived every Mode value without crashing");
}

}  // namespace

int main() {
  test_render_frame_survives_every_mode();

  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}

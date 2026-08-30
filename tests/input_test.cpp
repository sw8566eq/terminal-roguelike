// The second half of Phase 5 (see CLAUDE.md's "Testing overhaul" section): synthesized
// SDL_Events driving handle_event() through opening and closing several menus, asserting
// on gs.mode transitions. SDL_Event is a plain struct — no SDL_Init(), no window, no real
// display needed to construct one and hand it to handle_event() directly, the same
// headless spirit as tests/movement_test.cpp (which already proved input.cpp/its
// satellites link and run fine with nothing but SDL's headers).
//
// This is narrower than exhaustive input coverage — CLAUDE.md's Known limitations
// section already flags input as still mostly manual-playtest territory, and this file
// doesn't change that. What it does cover: the specific bug shape a modal-dispatch
// switch invites (a trigger key that doesn't open its menu, or a menu that doesn't
// close), for a representative handful of modes.

#include <SDL3/SDL.h>

#include <cstdio>
#include <string>

#include "arena.hpp"
#include "content.hpp"
#include "game.hpp"
#include "input.hpp"

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

SDL_Event key_down(SDL_Keycode key, SDL_Keymod mod = SDL_KMOD_NONE) {
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = key;
  event.key.mod = mod;
  return event;
}

// Trigger `key` from Mode::Playing, confirm it opens `expected_mode`, then send Escape
// and confirm it lands back on Mode::Playing — the open/close roundtrip every full-screen
// menu is supposed to support (see CLAUDE.md's "Adding a full-screen menu" note).
void check_menu_roundtrip(GameState& gs, SDL_Keycode trigger_key, Mode expected_mode, const std::string& menu_name) {
  gs.mode = Mode::Playing;
  handle_event(gs, key_down(trigger_key));
  check(gs.mode == expected_mode, menu_name + ": trigger key opens it from Mode::Playing");
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::Playing, menu_name + ": Escape closes it back to Mode::Playing");
}

void test_menu_open_close_roundtrips() {
  const std::vector<std::string> kRoom = {
      "##########",
      "#........#",
      "#........#",
      "##########",
  };
  GameState gs = arena::make_gamestate(kRoom, 2, 1);
  gs.player.weapons.push_back(kWeaponTable[0]);
  gs.player.armors.push_back(kArmorTable[0]);
  gs.player.potions.push_back(kPotionTable[0]);
  gs.player.intelligence = 10;
  gs.player.chosen_school = SpellSchool::Summoner;
  arena::place_minion(gs, kMinionTable[0], 4, 1);  // so 'm' has a roster to open

  check_menu_roundtrip(gs, SDLK_W, Mode::WeaponMenu, "WeaponMenu ('w')");
  check_menu_roundtrip(gs, SDLK_A, Mode::ArmorMenu, "ArmorMenu ('a')");
  check_menu_roundtrip(gs, SDLK_Q, Mode::PotionMenu, "PotionMenu ('q')");
  check_menu_roundtrip(gs, SDLK_D, Mode::Drop, "Drop ('d')");
  check_menu_roundtrip(gs, SDLK_Z, Mode::SpellMenu, "SpellMenu ('z')");
  check_menu_roundtrip(gs, SDLK_M, Mode::MinionRoster, "MinionRoster ('m')");
  check_menu_roundtrip(gs, SDLK_RIGHTBRACKET, Mode::MessageLog, "MessageLog (']')");
  check_menu_roundtrip(gs, SDLK_X, Mode::Look, "Look ('x')");

  // '?' is Shift+/ on a US layout — handle_help_input() accepts either the dedicated
  // keycode or the unshifted one with the modifier, so exercise the trigger the same way
  // (see handle_playing_input()'s own comment on this).
  gs.mode = Mode::Playing;
  handle_event(gs, key_down(SDLK_QUESTION));
  check(gs.mode == Mode::Help, "Help ('?'): trigger key opens it from Mode::Playing");
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::Playing, "Help ('?'): Escape closes it back to Mode::Playing");
}

// 'm' with no minions at all doesn't open the roster — it's rejected with a message and
// stays in Mode::Playing (see handle_playing_input()'s own guard). A regression here
// would mean either an empty roster silently opens (nothing to show) or a real roster
// silently gets refused.
void test_minion_roster_requires_a_minion() {
  const std::vector<std::string> kRoom = {
      "#####",
      "#...#",
      "#####",
  };
  GameState gs = arena::make_gamestate(kRoom, 1, 1);  // no minions placed
  handle_event(gs, key_down(SDLK_M));
  check(gs.mode == Mode::Playing, "'m' with no minions on the floor stays in Mode::Playing rather than opening an empty roster");
}

}  // namespace

int main() {
  test_menu_open_close_roundtrips();
  test_minion_roster_requires_a_minion();

  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}

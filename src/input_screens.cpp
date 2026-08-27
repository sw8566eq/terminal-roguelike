// Input handling for the start-menu and end-of-run screens: StartMenu, SetSeed,
// RunHistory, Dead, Win, MessageLog, Help. Split out of input.cpp purely to keep that
// file's size down (see input_internal.hpp and CLAUDE.md's "Known limitations" note) —
// reached the same way every other mode is, through handle_event()'s one switch. Each
// function still owns exactly one Mode, same as before the split.

#include "input.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "actors.hpp"
#include "content.hpp"
#include "input_internal.hpp"
#include "level.hpp"
#include "projectile.hpp"
#include "render.hpp"
#include "rng.hpp"
#include "rules.hpp"
#include "run_history.hpp"
#include "spells.hpp"
#include "turn.hpp"

void handle_start_menu_input(GameState& gs, const SDL_Event& event) {
  constexpr int kOptionCount = 4;  // Start Game, Set Seed, Run History, Exit
  if (event.key.key == SDLK_UP || event.key.key == SDLK_K) {
    gs.start_menu_selection = (gs.start_menu_selection + kOptionCount - 1) % kOptionCount;
    return;
  }
  if (event.key.key == SDLK_DOWN || event.key.key == SDLK_J) {
    gs.start_menu_selection = (gs.start_menu_selection + 1) % kOptionCount;
    return;
  }
  if (event.key.key == SDLK_ESCAPE) {
    gs.running = false;
    return;
  }

  int chosen = -1;
  if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
    chosen = gs.start_menu_selection;
  } else if (event.key.key == SDLK_1) {
    chosen = 0;
  } else if (event.key.key == SDLK_2) {
    chosen = 1;
  } else if (event.key.key == SDLK_3) {
    chosen = 2;
  } else if (event.key.key == SDLK_4) {
    chosen = 3;
  }
  if (chosen < 0) return;
  gs.start_menu_selection = chosen;

  switch (chosen) {
    case 0:  // Start Game: discards whatever floor 1 start_new_game() already built at
             // startup and builds a fresh one, so a seed set via the menu actually takes.
      start_new_game(gs);
      break;
    case 1:  // Set Seed
      gs.seed_input.clear();
      gs.mode = Mode::SetSeed;
      break;
    case 2:  // Run History
      gs.run_history_scroll = 0;  // always open showing the most recent runs
      gs.mode = Mode::RunHistory;
      break;
    case 3:  // Exit
      gs.running = false;
      break;
  }
}

void handle_set_seed_input(GameState& gs, const SDL_Event& event) {
  if (event.key.key == SDLK_ESCAPE) {
    gs.mode = Mode::StartMenu;
    return;
  }
  if (event.key.key == SDLK_BACKSPACE) {
    if (!gs.seed_input.empty()) gs.seed_input.pop_back();
    return;
  }
  if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
    // Empty input just backs out without touching the RNG, same free-cancel shape as
    // Esc — there's nothing sensible to confirm.
    if (!gs.seed_input.empty()) {
      unsigned int value = static_cast<unsigned int>(std::strtoul(gs.seed_input.c_str(), nullptr, 10));
      seed_rng(value);
      gs.current_seed_display = gs.seed_input;
    }
    gs.mode = Mode::StartMenu;
    return;
  }
  // SDLK_0..SDLK_9 are contiguous in SDL3. Capped at 10 digits, comfortably above
  // unsigned int's range (~4.29 billion) — same "just don't overflow" spirit the
  // --seed=N flag's own hand-rolled digit check already has.
  if (event.key.key >= SDLK_0 && event.key.key <= SDLK_9 && gs.seed_input.size() < 10) {
    gs.seed_input += static_cast<char>('0' + (event.key.key - SDLK_0));
  }
}

void handle_run_history_input(GameState& gs, const SDL_Event& event) {
  if (event.key.key == SDLK_ESCAPE) {
    gs.mode = Mode::StartMenu;
    return;
  }
  // Same "lines scrolled up from the bottom" idiom handle_message_log_input uses above,
  // just against run_history.txt's entry count instead of the message log's, and with
  // render_run_history_screen()'s own visible_rows (SCREEN_HEIGHT - 3, for the title row
  // and the blank line under it) kept in sync here.
  if (event.key.key == SDLK_K || event.key.key == SDLK_UP) {
    int visible_rows = SCREEN_HEIGHT - 3;
    int max_scroll = std::max(0, static_cast<int>(load_run_history().size()) - visible_rows);
    gs.run_history_scroll = std::min(gs.run_history_scroll + 1, max_scroll);
  } else if (event.key.key == SDLK_J || event.key.key == SDLK_DOWN) {
    gs.run_history_scroll = std::max(gs.run_history_scroll - 1, 0);
  }
}

void handle_dead_input(GameState& gs, const SDL_Event& event) {
  if (event.key.key == SDLK_ESCAPE) {
    gs.running = false;
  } else {
    start_new_game(gs);
  }
  return;
}

void handle_win_input(GameState& gs, const SDL_Event& event) {
  if (event.key.key == SDLK_ESCAPE) {
    gs.running = false;
  } else {
    start_new_game(gs);
  }
  return;
}

void handle_message_log_input(GameState& gs, const SDL_Event& event) {
  if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_RIGHTBRACKET) {
    gs.mode = Mode::Playing;
  } else if (event.key.key == SDLK_K || event.key.key == SDLK_UP) {
    // Matches render_message_log()'s own max_scroll now that a message can wrap to more
    // than one row: "don't scroll past the single oldest message" rather than a fixed
    // row count, since how many messages actually fit varies with their wrapped height.
    int max_scroll = std::max(0, static_cast<int>(gs.message_log.size()) - 1);
    gs.log_scroll = std::min(gs.log_scroll + 1, max_scroll);
  } else if (event.key.key == SDLK_J || event.key.key == SDLK_DOWN) {
    gs.log_scroll = std::max(gs.log_scroll - 1, 0);
  }
  return;
}

void handle_help_input(GameState& gs, const SDL_Event& event) {
  // Same unshifted-keycode-plus-modifier check the stairs keys use below, since
  // '?' is Shift+/ on a US layout.
  bool pressed_question =
      event.key.key == SDLK_QUESTION || (event.key.key == SDLK_SLASH && (event.key.mod & SDL_KMOD_SHIFT));
  if (event.key.key == SDLK_ESCAPE || pressed_question) gs.mode = Mode::Playing;
  return;
}

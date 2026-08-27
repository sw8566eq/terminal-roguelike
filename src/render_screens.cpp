// Rendering for the start-menu and end-of-run screens: StartMenu, SetSeed, RunHistory,
// Dead, Win, MessageLog, Help. Split out of render.cpp purely to keep that file's size
// down (see render_internal.hpp and CLAUDE.md's "Known limitations" note) — reached the
// same way every other mode is, through render_frame()'s one switch. Each function still
// owns exactly one Mode, same as before the split.

#include "render.hpp"

#include <algorithm>

#include "render_internal.hpp"
#include "run_history.hpp"

void render_start_menu(GameState& gs, tcod::Console& console) {
  tcod::print(console, {0, 0}, "TERMINAL ROGUELIKE", tcod::ColorRGB{255, 210, 60}, std::nullopt);

  static const std::vector<std::string> kOptions = {"Start Game", "Set Seed", "Run History", "Exit"};
  for (size_t i = 0; i < kOptions.size(); ++i) {
    bool selected = static_cast<int>(i) == gs.start_menu_selection;
    std::string line =
        (selected ? "> " : "  ") + std::to_string(i + 1) + ") " + kOptions[i];
    if (i == 1) line += "  (current: " + gs.current_seed_display + ")";
    tcod::print(console, {0, 2 + static_cast<int>(i)}, line,
                selected ? tcod::ColorRGB{255, 255, 255} : tcod::ColorRGB{180, 180, 180}, std::nullopt);
  }
  tcod::print(console, {0, 2 + static_cast<int>(kOptions.size()) + 1},
              "Up/Down or j/k to choose, Enter to select, or press 1-4. Esc quits.",
              tcod::ColorRGB{140, 140, 140}, std::nullopt);
}

void render_set_seed_screen(GameState& gs, tcod::Console& console) {
  tcod::print(console, {0, 0}, "Set Seed", tcod::ColorRGB{255, 255, 255}, std::nullopt);
  tcod::print(console, {0, 2}, "Type digits, Enter to confirm, Backspace to edit, Esc to cancel.",
              tcod::ColorRGB{200, 200, 200}, std::nullopt);
  std::string shown = gs.seed_input.empty() ? "_" : gs.seed_input;
  tcod::print(console, {0, 4}, "Seed: " + shown, tcod::ColorRGB{255, 210, 60}, std::nullopt);
}

void render_run_history_screen(GameState& gs, tcod::Console& console) {
  tcod::print(console, {0, 0}, "Run History - j/k or arrows to scroll, Esc to go back", tcod::ColorRGB{255, 255, 255},
              std::nullopt);

  std::vector<RunHistoryEntry> history = load_run_history();
  if (history.empty()) {
    tcod::print(console, {0, 2}, "No runs recorded yet.", tcod::ColorRGB{180, 180, 180}, std::nullopt);
    return;
  }

  // Stored oldest-first (append order); shown most-recent-first. run_history_scroll is
  // how many entries scrolled up from the newest (0 = showing the most recent runs) —
  // the same idiom render_message_log() uses for log_scroll, just over run history
  // entries instead of message-log lines.
  int visible_rows = SCREEN_HEIGHT - 3;
  int total = static_cast<int>(history.size());
  int max_scroll = std::max(0, total - visible_rows);
  gs.run_history_scroll = std::min(gs.run_history_scroll, max_scroll);  // clamp if the file shrank
  int end_index = total - gs.run_history_scroll;
  int start_index = std::max(0, end_index - visible_rows);
  for (int i = start_index; i < end_index; ++i) {
    const RunHistoryEntry& entry = history[static_cast<size_t>(i)];
    int row = 2 + (end_index - 1 - i);
    std::string outcome = entry.won ? "WON " : "DIED";
    tcod::ColorRGB color = entry.won ? tcod::ColorRGB{255, 210, 60} : tcod::ColorRGB{255, 80, 80};
    std::string line = outcome + " - Floor " + std::to_string(entry.floor_reached) + ", Level " +
                        std::to_string(entry.player_level) + ", seed " + entry.seed_display + " - " + entry.cause;
    tcod::print(console, {0, row}, line, color, std::nullopt);
  }
}

void render_death_screen(GameState& gs, tcod::Console& console) {
  tcod::print(console, {0, 0}, "You died, slain by the " + gs.death_cause + ".", tcod::ColorRGB{255, 80, 80},
              std::nullopt);
  tcod::print(console, {0, 2}, "Press any key to start a new game, or Esc to quit.", tcod::ColorRGB{200, 200, 200},
              std::nullopt);
}

void render_win_screen(GameState& gs, tcod::Console& console) {
  tcod::print(console, {0, 0}, "You have slain the " + gs.win_cause + " and conquered the dungeon!",
              tcod::ColorRGB{255, 210, 60}, std::nullopt);
  tcod::print(console, {0, 2}, "Press any key to start a new game, or Esc to quit.", tcod::ColorRGB{200, 200, 200},
              std::nullopt);
}

void render_message_log(GameState& gs, tcod::Console& console) {
  tcod::print(console, {0, 0}, "Message Log - j/k or arrows to scroll, ']' or Esc to close",
              tcod::ColorRGB{255, 255, 255}, std::nullopt);

  int visible_rows = SCREEN_HEIGHT - 1;
  int total = static_cast<int>(gs.message_log.size());
  // log_scroll counts *messages* scrolled up from the newest (0 = ending on the most
  // recent one) — unchanged from before wrapping existed, and still what
  // handle_message_log_input() increments/decrements by one per keypress. What changed
  // is how many rows that costs: a wrapped message can now take more than one, so
  // max_scroll is simply "don't scroll past the single oldest message" rather than a
  // precise "exactly fills the screen" count, which would depend on every message's
  // wrapped height between here and the top.
  int max_scroll = std::max(0, total - 1);
  gs.log_scroll = std::min(gs.log_scroll, max_scroll);  // clamp in case the log shrank (e.g. after a restart)

  // Oldest at top, newest at bottom, like a terminal scrollback. end_index is one past
  // the last message shown; walk backward from it, wrapped-height first, accumulating
  // until the visible_rows budget would overflow — the same idiom render_log_panel()
  // uses for its own smaller budget, just against the whole screen's height.
  int end_index = total - gs.log_scroll;
  int start_index = end_index;
  int used = 0;
  while (start_index > 0) {
    int h = tcod::get_height_rect(SCREEN_WIDTH, gs.message_log[static_cast<size_t>(start_index - 1)]);
    if (used + h > visible_rows) break;
    used += h;
    --start_index;
  }
  if (start_index == end_index && end_index > 0) start_index = end_index - 1;  // always show at least one

  int y = 1;
  int bottom = SCREEN_HEIGHT - 1;
  for (int i = start_index; i < end_index && y <= bottom; ++i) {
    int remaining = bottom - y + 1;
    y += tcod::print_rect(console, {0, y, SCREEN_WIDTH, remaining}, gs.message_log[static_cast<size_t>(i)],
                           tcod::ColorRGB{200, 200, 200}, std::nullopt);
  }
}

// Static text; reads no game state, hence no GameState parameter.
void render_help(tcod::Console& console) {
  tcod::print(console, {0, 0}, "Controls - '?' or Esc to close", tcod::ColorRGB{255, 255, 255}, std::nullopt);
  static const std::vector<std::string> kHelpLines = {
      "",
      "Arrows / hjkl / yubn (diagonals)  Move; walks into an enemy to attack, or",
      "                                  swaps places with your own minion",
      "Shift + a movement key            Travel that way until a hostile comes into",
      "                                  view or you hit something",
      ".                                 Wait a turn",
      ">  <                              Stairs down/up (must be standing on them)",
      "g                                 Pick up (menu if several items here)",
      "w  a  q                           Weapon / Armor / Potion menu (equip or drink)",
      "d                                 Drop a weapon, armor, or potion",
      "f                                 Fire the equipped ranged weapon (move to target,",
      "                                  Enter to loose it, Esc to cancel)",
      "z                                 Cast a known spell — or, while focused on a single",
      "                                  minion, open that minion's own abilities instead",
      "m                                 Command a minion or all of them (roster menu; Shift+A",
      "                                  there jumps straight to All)",
      "o  p                              Cycle command focus to the next/previous minion",
      "Shift+P                           Return focus to yourself",
      "f  g  Enter                       While focused on a minion instead: Follow / go",
      "                                  Aggressive / confirm Attack or Hold (sidebar shows",
      "                                  each minion's order as [F]ollow / [G]o aggressive /",
      "                                  [H]old / [A]ttack)",
      "x                                 Look around (move the cursor, side panel shows",
      "                                  details); x or Esc to close",
      "]                                 Message log (full scrollback)",
      "Shift+S  Shift+D  Shift+I         On level up: spend the point on STR/DEX/INT",
      "Shift+C  Shift+U  Shift+M         At Intelligence 4: choose Caster, Summoner, or",
      "                                  Combat Mage (once, permanent)",
      "?                                 This screen",
      "Esc                               Quit (or close the current menu)",
  };
  for (size_t i = 0; i < kHelpLines.size(); ++i) {
    tcod::print(console, {0, 1 + static_cast<int>(i)}, kHelpLines[i], tcod::ColorRGB{200, 200, 200},
                std::nullopt);
  }
}

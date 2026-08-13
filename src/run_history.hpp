#pragma once

// Persisted record of past runs, so the start menu's Run History screen survives
// process restarts. One flat text file (run_history.txt, in the current working
// directory — the same "run from repo root" assumption load_best_tileset() already
// makes for font loading), one line per run, appended to as runs end and read back
// oldest-first.
//
// This is a primitive, like rng.hpp: no GameState dependency, just data in and data
// out. game.cpp fills in an entry and calls append_run_history_entry() from the same
// two spots on_actor_killed() already switches to Mode::Dead/Mode::Win.

#include <string>
#include <vector>

struct RunHistoryEntry {
  bool won = false;
  int floor_reached = 1;
  int player_level = 1;
  std::string cause;         // death cause, or the final boss's name on a win
  std::string seed_display;  // the seed the run used, or "random"
};

// Every entry currently on disk, oldest first. A missing file just means no history
// yet, not an error — the Run History screen shows "No runs recorded yet." for that.
std::vector<RunHistoryEntry> load_run_history();

// Appends one entry to the on-disk history file. Best-effort: if the file can't be
// opened for some reason, the run simply won't show up in history rather than
// crashing the game over it.
void append_run_history_entry(const RunHistoryEntry& entry);

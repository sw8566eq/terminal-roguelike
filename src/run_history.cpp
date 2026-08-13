#include "run_history.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {
const char* kRunHistoryPath = "run_history.txt";
constexpr char kFieldSep = '|';
}  // namespace

std::vector<RunHistoryEntry> load_run_history() {
  std::vector<RunHistoryEntry> entries;
  std::ifstream in(kRunHistoryPath);
  if (!in) return entries;  // no history file yet

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::istringstream fields(line);
    std::string outcome, floor_str, level_str, seed_str, cause;
    if (!std::getline(fields, outcome, kFieldSep)) continue;
    if (!std::getline(fields, floor_str, kFieldSep)) continue;
    if (!std::getline(fields, level_str, kFieldSep)) continue;
    if (!std::getline(fields, seed_str, kFieldSep)) continue;
    std::getline(fields, cause);  // whatever's left on the line, cause is never re-split

    RunHistoryEntry entry;
    entry.won = (outcome == "WIN");
    entry.floor_reached = std::atoi(floor_str.c_str());
    entry.player_level = std::atoi(level_str.c_str());
    entry.seed_display = seed_str;
    entry.cause = cause;
    entries.push_back(std::move(entry));
  }
  return entries;
}

void append_run_history_entry(const RunHistoryEntry& entry) {
  std::ofstream out(kRunHistoryPath, std::ios::app);
  if (!out) return;
  out << (entry.won ? "WIN" : "DEAD") << kFieldSep << entry.floor_reached << kFieldSep << entry.player_level
      << kFieldSep << entry.seed_display << kFieldSep << entry.cause << "\n";
}

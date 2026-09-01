#pragma once

// The "test arena": headless helpers for building an exact, minimal GameState — a hand-
// placed map, a player, and exactly the monsters/minions a test needs at exactly the
// tiles it needs them — then driving turns through the real end_turn()/resolve_attack()
// pipeline and asserting on the result. No SDL, no window, no libtcod console: everything
// in this file's own dependency closure (see CMakeLists.txt's roguelike_game_tests
// target) is exactly the headless logic layer --dump-loot already proves out.
//
// Deliberately thin — these are wrappers over real production entry points
// (spawn_monster(), spawn_minion(), actor_index_by_id()), not a parallel simulation.
// A test arena that reimplemented spawning would be testing itself, not the game.

#include <string>
#include <string_view>
#include <vector>

#include "content.hpp"
#include "game.hpp"

// Row index in `table` whose .name matches `name`, or -1 if none does — every suite
// looks a content-table row up by name rather than a hardcoded index (so a table
// reorder can't quietly make a test exercise the wrong row), and this is the one
// definition of that lookup shared across all of them.
//
// `name` is a string_view (not `const std::string&`) deliberately: named() below returns
// a reference and every call site passes a string literal, and a `const std::string&`
// parameter there binds a temporary std::string that GCC's -Wdangling-reference flags as
// a possible source of the returned reference, even though what's actually returned
// aliases `table`, never `name`. Taking `name` by value as a string_view sidesteps the
// warning outright rather than fighting it, and costs nothing extra at any call site.
template <typename T>
int index_named(const std::vector<T>& table, std::string_view name) {
  for (size_t i = 0; i < table.size(); ++i) {
    if (table[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

// The same lookup as index_named(), but returning the matching row itself rather than
// its index — for a call site that only ever wanted the row (e.g. to push a copy into
// an Actor's inventory), which would otherwise re-wrap index_named()'s result in
// `table[static_cast<size_t>(...)]` at every use.
template <typename T>
const T& named(const std::vector<T>& table, std::string_view name) {
  return table[static_cast<size_t>(index_named(table, name))];
}

// Convenience wrappers over index_named() for the four content tables every suite
// actually looks up by name — kept unqualified (not under namespace arena below) since
// that's how every existing call site already spells them.
int monster_index_named(const std::string& name);
int spell_index_named(const std::string& name);
int weapon_index_named(const std::string& name);
int potion_index_named(const std::string& name);

namespace arena {

// Builds a GameState with one hand-painted floor (see Map::paint_ascii) and the player
// placed at (px, py) with sane, fixed stats (STR/DEX/INT 2, level 1, Fists, no armor —
// the same starting numbers start_new_game() gives a fresh character) rather than
// start_new_game()'s randomly-generated dungeon. No monsters, no items — callers add
// exactly what a test needs via place_monster()/place_minion().
//
// `rows` must be no larger than MAP_WIDTH x MAP_HEIGHT (game.hpp); the underlying Map is
// always allocated at that full size so every existing coordinate/bounds assumption
// elsewhere in the game logic (e.g. render/HUD math this suite doesn't exercise) still
// holds, but only the tiles `rows` actually paints are anything other than wall.
GameState make_gamestate(const std::vector<std::string>& rows, int px, int py);

// Appends a second hand-painted floor — same Level aggregate-init shape
// make_gamestate() itself uses — without touching the current floor or the player.
// Returns the new floor's index (levels.size() - 1) so a test can set gs.current_level
// to it. For a test that needs ascend()/descend() to have somewhere real to land;
// make_gamestate() alone only ever builds one floor.
int add_floor(GameState& gs, const std::vector<std::string>& rows, int entry_x, int entry_y, bool has_stairs_up);

// Places a hostile monster from kMonsterTable at an exact tile and returns its stable
// Actor::id (see actor_index_by_id()) so a test can find it again after a turn shifts
// level.monsters around. Thin wrapper over spawn_monster() + push_back.
int place_monster(GameState& gs, int table_index, int x, int y);

// The minion counterpart of place_monster() — spawn_minion() + push_back, allegiance
// already Allegiance::Player.
int place_minion(GameState& gs, const MinionTemplate& tmpl, int x, int y);

// Finds a still-alive Actor by id on the current floor, or nullptr if it died or was
// never there. Thin wrapper over actor_index_by_id() for tests that don't want to think
// about vector indices shifting after a kill.
Actor* find_actor(GameState& gs, int id);

}  // namespace arena

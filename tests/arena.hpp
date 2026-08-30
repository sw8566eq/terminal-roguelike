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
#include <vector>

#include "content.hpp"
#include "game.hpp"

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

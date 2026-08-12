#pragma once

#include <libtcod.hpp>

#include <optional>
#include <utility>
#include <vector>

// A single tile in the dungeon.
struct Tile {
  bool walkable = false;
  bool transparent = false;  // whether light/sight passes through, for FOV
  bool explored = false;     // true once the tile has ever been seen (fog of war memory)
  bool in_room = false;      // true for room floor tiles; false for corridor floor tiles and walls
  // A pit/chasm: blocks movement and pathfinding (walkable=false, like a wall) but not
  // projectiles/spells/line-of-sight (transparent=true, unlike a wall). See
  // Map::blocks_projectile() and Map::carve_hole_clusters().
  bool is_hole = false;
  // True for every tile of a room placed by Map::carve_special_room() — both its
  // interior and its own surrounding wall ring, so the renderer can give the whole
  // chamber (walls included) a distinct color. Purely cosmetic/informational; nothing
  // in Map itself treats it differently from an ordinary room or wall tile.
  bool in_special_room = false;
};

// Axis-aligned rectangle used while carving rooms, in tile coordinates.
// [x1, x2) x [y1, y2), i.e. x2/y2 are exclusive.
struct Rect {
  int x1, y1, x2, y2;

  Rect(int x, int y, int w, int h) : x1(x), y1(y), x2(x + w), y2(y + h) {}

  std::pair<int, int> center() const { return {(x1 + x2) / 2, (y1 + y2) / 2}; }

  bool intersects(const Rect& other) const {
    return x1 <= other.x2 && x2 >= other.x1 && y1 <= other.y2 && y2 >= other.y1;
  }
};

class Map {
 public:
  Map(int width, int height);

  int width() const { return width_; }
  int height() const { return height_; }

  bool in_bounds(int x, int y) const;
  bool is_walkable(int x, int y) const;
  // Whether a projectile/spell/line-of-sight is stopped at (x,y): true for a wall or
  // out-of-bounds, false for ordinary floor *and* for a Hole. The one "does a shot stop
  // here" check, kept separate from is_walkable so the two can differ.
  bool blocks_projectile(int x, int y) const;
  const Tile& at(int x, int y) const;

  // Resets the map to all-wall and carves a fresh dungeon of non-overlapping
  // rooms connected by L-shaped corridors. Returns the tile coordinates the
  // player should start in (the center of the first room placed).
  std::pair<int, int> generate(int max_rooms, int room_min_size, int room_max_size);

  // Recomputes what's visible from (x, y) out to radius tiles (0 = unlimited),
  // stopping at walls. Newly-seen tiles are marked explored, so they stay
  // dimly visible via is_explored() even after they fall out of view again.
  // Call this once after generate() and again every time the player moves.
  void update_fov(int x, int y, int radius);

  bool is_in_fov(int x, int y) const;
  bool is_explored(int x, int y) const;
  bool is_in_room(int x, int y) const;  // false for corridor tiles and walls

  // A* path from (from_x,from_y) to (to_x,to_y) over this map's walkability (the same
  // TCODMap already kept in sync for FOV — see sync_fov_map()). Returns the tiles from
  // just past the start through the destination, in order; empty if no path exists or
  // start == destination. Used by monster chase AI so they route around obstacles
  // instead of a naive greedy step toward the target.
  std::vector<std::pair<int, int>> find_path(int from_x, int from_y, int to_x, int to_y) const;

  // Carves 0-2 small random-shaped Hole patches into room interiors only (never a
  // corridor, never a room's edge/doorway), each validated by tentatively carving it
  // and confirming (entry_x,entry_y) can still reach (stairs_x,stairs_y) via
  // find_path() — reverted and retried if not. Must be called after stairs are chosen
  // (from generate_level() in level.hpp), not from inside generate() itself, since
  // stairs_down_x/y don't exist yet when generate() runs.
  void carve_hole_clusters(int entry_x, int entry_y, int stairs_x, int stairs_y);

  // Carves one additional room beyond generate()'s own `max_rooms`, sized
  // room_min_size..room_max_size (independent of generate()'s own room size range, so
  // it can be made deliberately bigger) and connected to (link_x, link_y) by the same
  // randomized-bend L-shaped corridor generate()'s own room-to-room connections use.
  // Must be called after generate() — it checks each candidate location directly
  // against the tile grid (every tile in it, plus a 1-tile border, must still be
  // untouched wall) rather than against generate()'s own room list, which isn't
  // exposed outside this class. Retried a bounded number of times; returns the placed
  // room's bounds, or std::nullopt if the map was too crowded to fit it anywhere (the
  // caller should fall back to its normal placement in that case, same as
  // carve_hole_clusters() silently placing fewer patches than requested rather than
  // failing outright).
  //
  // Map itself has no notion of what goes in this room — that's entirely the caller's
  // business (see generate_level(), which uses it for the final boss's chamber). This
  // is just "one more, bigger, guaranteed room," a generic capability.
  //
  // The room's outer ring (every interior tile touching its own wall) becomes a moat of
  // Hole tiles — walkable=false like a wall, but still shootable/castable over, per the
  // usual Hole rules. Every tile the connector corridor actually crosses is left as
  // plain floor instead, guaranteeing at least one walkable way in regardless of exactly
  // where the corridor happens to meet the room. Every tile of the room (interior, ring
  // and its own surrounding wall) is also tagged Tile::in_special_room, purely so the
  // renderer can give the whole chamber a distinct look.
  std::optional<Rect> carve_special_room(int room_min_size, int room_max_size, int link_x, int link_y);

 private:
  void dig_room(const Rect& room);
  void dig_corridor_h(int x1, int x2, int y);
  void dig_corridor_v(int y1, int y2, int x);
  void sync_fov_map();  // pushes tiles_' walkable/transparent flags into fov_map_

  int width_;
  int height_;
  std::vector<Tile> tiles_;
  TCODMap fov_map_;
};

// --- Line geometry over a map -------------------------------------------------------

// Every tile from just past (from_x,from_y) through (to_x,to_y), via libtcod's Bresenham
// line. Excludes the starting tile so a projectile doesn't "hit" its caster. libtcod
// already provides the line-tracing — don't hand-roll one.
std::vector<std::pair<int, int>> trace_path(int from_x, int from_y, int to_x, int to_y);

// Whether a straight line from (fx,fy) to (tx,ty) is unobstructed — used to let a ranged
// attacker's reach, or a spell's, be blocked by terrain instead of passing straight
// through it. Checks every tile the path crosses *except the last* (the target's own tile
// doesn't need to be clear from the shooter's perspective — the target is standing on
// it). For adjacent tiles trace_path()'s result is just the single target tile, so the
// loop never runs and this is trivially true: melee is completely unaffected.
//
// Uses blocks_projectile(), so a wall stops it but a hole does not — you can shoot, cast
// and swap across a pit you couldn't walk over.
bool line_clear(int fx, int fy, int tx, int ty, const Map& map);

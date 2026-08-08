#pragma once

#include <libtcod.hpp>

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
  // (from generate_level() in main.cpp), not from inside generate() itself, since
  // stairs_down_x/y don't exist yet when generate() runs.
  void carve_hole_clusters(int entry_x, int entry_y, int stairs_x, int stairs_y);

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

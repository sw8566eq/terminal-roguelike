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

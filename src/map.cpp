#include "map.hpp"

#include <algorithm>

#include "rng.hpp"

Map::Map(int width, int height)
    : width_(width), height_(height), tiles_(width * height), fov_map_(width, height) {}

bool Map::in_bounds(int x, int y) const { return x >= 0 && x < width_ && y >= 0 && y < height_; }

bool Map::is_walkable(int x, int y) const { return in_bounds(x, y) && tiles_[y * width_ + x].walkable; }

const Tile& Map::at(int x, int y) const { return tiles_[y * width_ + x]; }

bool Map::is_in_fov(int x, int y) const { return in_bounds(x, y) && fov_map_.isInFov(x, y); }

bool Map::is_explored(int x, int y) const { return in_bounds(x, y) && tiles_[y * width_ + x].explored; }

bool Map::is_in_room(int x, int y) const { return in_bounds(x, y) && tiles_[y * width_ + x].in_room; }

void Map::sync_fov_map() {
  for (int y = 0; y < height_; ++y) {
    for (int x = 0; x < width_; ++x) {
      const Tile& tile = tiles_[y * width_ + x];
      fov_map_.setProperties(x, y, tile.transparent, tile.walkable);
    }
  }
}

void Map::update_fov(int x, int y, int radius) {
  fov_map_.computeFov(x, y, radius, /*light_walls=*/true, FOV_SHADOW);
  for (int yy = 0; yy < height_; ++yy) {
    for (int xx = 0; xx < width_; ++xx) {
      if (fov_map_.isInFov(xx, yy)) tiles_[yy * width_ + xx].explored = true;
    }
  }
}

void Map::dig_room(const Rect& room) {
  for (int y = room.y1; y < room.y2; ++y) {
    for (int x = room.x1; x < room.x2; ++x) {
      if (!in_bounds(x, y)) continue;
      Tile& tile = tiles_[y * width_ + x];
      tile.walkable = true;
      tile.transparent = true;
      tile.in_room = true;
    }
  }
}

// Note: these mutate walkable/transparent in place rather than overwriting the whole
// Tile, so a corridor that happens to clip a room's edge doesn't stomp its in_room flag.
void Map::dig_corridor_h(int x1, int x2, int y) {
  for (int x = std::min(x1, x2); x <= std::max(x1, x2); ++x) {
    if (!in_bounds(x, y)) continue;
    Tile& tile = tiles_[y * width_ + x];
    tile.walkable = true;
    tile.transparent = true;
  }
}

void Map::dig_corridor_v(int y1, int y2, int x) {
  for (int y = std::min(y1, y2); y <= std::max(y1, y2); ++y) {
    if (!in_bounds(x, y)) continue;
    Tile& tile = tiles_[y * width_ + x];
    tile.walkable = true;
    tile.transparent = true;
  }
}

std::pair<int, int> Map::generate(int max_rooms, int room_min_size, int room_max_size) {
  std::fill(tiles_.begin(), tiles_.end(), Tile{});

  std::vector<Rect> rooms;
  std::pair<int, int> start{width_ / 2, height_ / 2};  // fallback if nothing gets placed

  for (int i = 0; i < max_rooms; ++i) {
    int w = random_int(room_min_size, room_max_size);
    int h = random_int(room_min_size, room_max_size);

    // Leave a 1-tile border so rooms never touch the map edge.
    int x_max = width_ - w - 1;
    int y_max = height_ - h - 1;
    if (x_max < 1 || y_max < 1) continue;
    int x = random_int(1, x_max);
    int y = random_int(1, y_max);

    Rect new_room(x, y, w, h);
    bool overlaps = std::any_of(
        rooms.begin(), rooms.end(), [&](const Rect& other) { return new_room.intersects(other); });
    if (overlaps) continue;

    dig_room(new_room);
    auto [new_x, new_y] = new_room.center();

    if (rooms.empty()) {
      start = {new_x, new_y};
    } else {
      auto [prev_x, prev_y] = rooms.back().center();
      // Randomize the bend direction so corridors don't all turn the same way.
      if (random_int(0, 1) == 0) {
        dig_corridor_h(prev_x, new_x, prev_y);
        dig_corridor_v(prev_y, new_y, new_x);
      } else {
        dig_corridor_v(prev_y, new_y, prev_x);
        dig_corridor_h(prev_x, new_x, new_y);
      }
    }

    rooms.push_back(new_room);
  }

  sync_fov_map();
  return start;
}

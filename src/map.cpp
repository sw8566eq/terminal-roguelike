#include "map.hpp"

#include <algorithm>

#include "rng.hpp"

Map::Map(int width, int height)
    : width_(width), height_(height), tiles_(width * height), fov_map_(width, height) {}

bool Map::in_bounds(int x, int y) const { return x >= 0 && x < width_ && y >= 0 && y < height_; }

bool Map::is_walkable(int x, int y) const { return in_bounds(x, y) && tiles_[y * width_ + x].walkable; }

bool Map::blocks_projectile(int x, int y) const {
  if (!in_bounds(x, y)) return true;
  const Tile& tile = tiles_[y * width_ + x];
  return !tile.walkable && !tile.is_hole;
}

const Tile& Map::at(int x, int y) const { return tiles_[y * width_ + x]; }

bool Map::is_in_fov(int x, int y) const { return in_bounds(x, y) && fov_map_.isInFov(x, y); }

bool Map::is_explored(int x, int y) const { return in_bounds(x, y) && tiles_[y * width_ + x].explored; }

bool Map::is_in_room(int x, int y) const { return in_bounds(x, y) && tiles_[y * width_ + x].in_room; }

std::vector<std::pair<int, int>> Map::find_path(int from_x, int from_y, int to_x, int to_y) const {
  std::vector<std::pair<int, int>> result;
  TCODPath path(&fov_map_);
  if (!path.compute(from_x, from_y, to_x, to_y)) return result;
  int x, y;
  while (path.walk(&x, &y, false)) {
    result.push_back({x, y});
  }
  return result;
}

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

namespace {
constexpr int kMaxHolePatches = 2;    // 0-2 patches per floor
constexpr int kMinHolePatchSize = 2;
constexpr int kMaxHolePatchSize = 6;
constexpr int kHolePatchRetries = 5;  // per patch, before giving up on placing it at all
}  // namespace

void Map::carve_hole_clusters(int entry_x, int entry_y, int stairs_x, int stairs_y) {
  // A tile is a safe hole candidate if it's room floor (never a corridor/wall), not the
  // entry or stairs tile, not already a hole, and every orthogonal neighbor is also room
  // floor — that last check is what keeps a patch away from a room's edge/doorway (a
  // corridor or wall neighbor means this tile is on the boundary).
  auto is_candidate = [&](int x, int y) {
    if (!in_bounds(x, y)) return false;
    if ((x == entry_x && y == entry_y) || (x == stairs_x && y == stairs_y)) return false;
    const Tile& t = tiles_[y * width_ + x];
    if (!t.in_room || !t.walkable || t.is_hole) return false;
    const int offs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& o : offs) {
      int nx = x + o[0], ny = y + o[1];
      if (!in_bounds(nx, ny) || !tiles_[ny * width_ + nx].in_room) return false;
    }
    return true;
  };

  int patch_count = random_int(0, kMaxHolePatches);
  for (int p = 0; p < patch_count; ++p) {
    for (int attempt = 0; attempt < kHolePatchRetries; ++attempt) {
      // Hunt for a random seed tile (bounded tries so a nearly-full map can't hang).
      int seed_x = -1, seed_y = -1;
      for (int tries = 0; tries < 200 && seed_x < 0; ++tries) {
        int x = random_int(0, width_ - 1);
        int y = random_int(0, height_ - 1);
        if (is_candidate(x, y)) {
          seed_x = x;
          seed_y = y;
        }
      }
      if (seed_x < 0) break;  // no valid seed anywhere; give up on this patch entirely

      // Random-walk a small blob out from the seed, staying inside candidate tiles.
      std::vector<std::pair<int, int>> patch = {{seed_x, seed_y}};
      int target_size = random_int(kMinHolePatchSize, kMaxHolePatchSize);
      while (static_cast<int>(patch.size()) < target_size) {
        auto [fx, fy] = patch[static_cast<size_t>(random_int(0, static_cast<int>(patch.size()) - 1))];
        const int offs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        std::vector<std::pair<int, int>> options;
        for (const auto& o : offs) {
          int nx = fx + o[0], ny = fy + o[1];
          if (!is_candidate(nx, ny)) continue;
          bool already = std::any_of(patch.begin(), patch.end(),
                                      [&](const auto& t) { return t.first == nx && t.second == ny; });
          if (!already) options.push_back({nx, ny});
        }
        if (options.empty()) break;  // can't grow further; take what we have
        patch.push_back(options[static_cast<size_t>(random_int(0, static_cast<int>(options.size()) - 1))]);
      }
      if (static_cast<int>(patch.size()) < kMinHolePatchSize) continue;  // too small, retry elsewhere

      // Tentatively carve, then confirm entry can still reach the stairs.
      for (const auto& [x, y] : patch) {
        Tile& t = tiles_[y * width_ + x];
        t.walkable = false;
        t.transparent = true;  // already true for any dig_room-produced tile; explicit for clarity
        t.is_hole = true;
      }
      sync_fov_map();
      // entry_x/y and stairs_x/y are always distinct tiles (stairs placement excludes
      // the entry from its occupied list), so an empty result here always means
      // "unreachable" — not find_path()'s other empty-result case (start == destination).
      bool reachable = !find_path(entry_x, entry_y, stairs_x, stairs_y).empty();
      if (reachable) break;  // patch kept; move on to the next patch

      // Revert and retry a different placement.
      for (const auto& [x, y] : patch) {
        Tile& t = tiles_[y * width_ + x];
        t.walkable = true;
        t.is_hole = false;
      }
      sync_fov_map();
    }
  }
}

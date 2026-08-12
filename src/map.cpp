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

namespace {
constexpr int kSpecialRoomRetries = 500;
}  // namespace

std::optional<Rect> Map::carve_special_room(int room_min_size, int room_max_size, int link_x, int link_y) {
  for (int attempt = 0; attempt < kSpecialRoomRetries; ++attempt) {
    int w = random_int(room_min_size, room_max_size);
    int h = random_int(room_min_size, room_max_size);

    // Same border-from-the-map-edge margin generate()'s own room placement uses.
    int x_max = width_ - w - 1;
    int y_max = height_ - h - 1;
    if (x_max < 1 || y_max < 1) continue;
    int x = random_int(1, x_max);
    int y = random_int(1, y_max);
    Rect candidate(x, y, w, h);

    // Not part of generate()'s own room list, so overlap is checked directly against
    // the tile grid instead: every tile in the candidate rect, plus a 1-tile border
    // (so it doesn't end up sharing a wall with something else already carved), must
    // still be untouched wall.
    bool clear = true;
    for (int cy = candidate.y1 - 1; clear && cy <= candidate.y2; ++cy) {
      for (int cx = candidate.x1 - 1; cx <= candidate.x2; ++cx) {
        if (is_walkable(cx, cy)) {
          clear = false;
          break;
        }
      }
    }
    if (!clear) continue;

    dig_room(candidate);

    // Tag the whole chamber — interior plus its own surrounding wall ring, the same
    // border already confirmed clear above — purely for the renderer. Nothing here
    // reads it back.
    for (int ty = candidate.y1 - 1; ty <= candidate.y2; ++ty) {
      for (int tx = candidate.x1 - 1; tx <= candidate.x2; ++tx) {
        if (in_bounds(tx, ty)) tiles_[static_cast<size_t>(ty * width_ + tx)].in_special_room = true;
      }
    }

    auto [center_x, center_y] = candidate.center();
    // Same randomized-bend L-shaped connector generate()'s own room-to-room corridors
    // use, just linking to a caller-supplied point instead of the previous room —
    // carving it directly like this guarantees the new room is connected, so unlike
    // carve_hole_clusters() there's no separate reachability check to run afterward.
    // Every tile either segment touches is tracked as it's carved, so the ring-moat
    // step just below knows exactly which ring tile(s) to leave as a walkable entrance.
    std::vector<std::pair<int, int>> corridor_tiles;
    auto track_h = [&](int xa, int xb, int ty) {
      for (int tx = std::min(xa, xb); tx <= std::max(xa, xb); ++tx) corridor_tiles.push_back({tx, ty});
    };
    auto track_v = [&](int ya, int yb, int tx) {
      for (int ty = std::min(ya, yb); ty <= std::max(ya, yb); ++ty) corridor_tiles.push_back({tx, ty});
    };
    if (random_int(0, 1) == 0) {
      dig_corridor_h(link_x, center_x, link_y);
      dig_corridor_v(link_y, center_y, center_x);
      track_h(link_x, center_x, link_y);
      track_v(link_y, center_y, center_x);
    } else {
      dig_corridor_v(link_y, center_y, link_x);
      dig_corridor_h(link_x, center_x, center_y);
      track_v(link_y, center_y, link_x);
      track_h(link_x, center_x, center_y);
    }

    // The room's outer ring becomes a moat of Hole tiles, except wherever the connector
    // corridor actually crosses it — that gap is what keeps the room reachable at all.
    // A straight line from outside the room (link_x/y) to strictly inside it
    // (center_x/y, always within the interior) is guaranteed to cross the ring at least
    // once, but this is exactly the kind of "should always hold" claim
    // carve_hole_clusters() double-checks rather than trusts — so this does too: if
    // nothing on the ring is actually part of the tracked corridor path, the moat is
    // skipped entirely (room stays fully walkable) rather than risk sealing it shut.
    bool has_entrance = std::any_of(corridor_tiles.begin(), corridor_tiles.end(), [&](const auto& t) {
      int tx = t.first, ty = t.second;
      if (tx < candidate.x1 || tx >= candidate.x2 || ty < candidate.y1 || ty >= candidate.y2) return false;
      return tx == candidate.x1 || tx == candidate.x2 - 1 || ty == candidate.y1 || ty == candidate.y2 - 1;
    });
    if (has_entrance) {
      for (int ry = candidate.y1; ry < candidate.y2; ++ry) {
        for (int rx = candidate.x1; rx < candidate.x2; ++rx) {
          bool on_ring = rx == candidate.x1 || rx == candidate.x2 - 1 || ry == candidate.y1 || ry == candidate.y2 - 1;
          if (!on_ring) continue;
          bool is_entrance = std::any_of(corridor_tiles.begin(), corridor_tiles.end(),
                                          [&](const auto& t) { return t.first == rx && t.second == ry; });
          if (is_entrance) continue;
          Tile& t = tiles_[static_cast<size_t>(ry * width_ + rx)];
          t.walkable = false;
          t.transparent = true;  // already true from dig_room; explicit for clarity, matching carve_hole_clusters()
          t.is_hole = true;
        }
      }
    }

    sync_fov_map();
    return candidate;
  }
  return std::nullopt;
}

std::vector<std::pair<int, int>> trace_path(int from_x, int from_y, int to_x, int to_y) {
  std::vector<std::pair<int, int>> path;
  for (auto [x, y] : tcod::BresenhamLine({from_x, from_y}, {to_x, to_y}).without_start()) {
    path.push_back({x, y});
  }
  return path;
}

bool line_clear(int fx, int fy, int tx, int ty, const Map& map) {
  auto path = trace_path(fx, fy, tx, ty);
  for (size_t i = 0; i + 1 < path.size(); ++i) {
    if (map.blocks_projectile(path[i].first, path[i].second)) return false;
  }
  return true;
}

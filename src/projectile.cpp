#include "projectile.hpp"

std::vector<std::pair<int, int>> trace_path(int from_x, int from_y, int to_x, int to_y) {
  std::vector<std::pair<int, int>> path;
  for (auto [x, y] : tcod::BresenhamLine({from_x, from_y}, {to_x, to_y}).without_start()) {
    path.push_back({x, y});
  }
  return path;
}

std::pair<int, int> find_impact(const std::vector<std::pair<int, int>>& path, int start_x, int start_y, const Map& map,
                                const std::vector<Actor>& monsters) {
  int prev_x = start_x, prev_y = start_y;
  for (const auto& [x, y] : path) {
    if (map.blocks_projectile(x, y)) return {prev_x, prev_y};
    if (hostile_monster_at(monsters, x, y) >= 0) return {x, y};
    prev_x = x;
    prev_y = y;
  }
  return {prev_x, prev_y};  // reached the end of the path with nothing there
}

bool line_clear(int fx, int fy, int tx, int ty, const Map& map) {
  auto path = trace_path(fx, fy, tx, ty);
  for (size_t i = 0; i + 1 < path.size(); ++i) {
    if (map.blocks_projectile(path[i].first, path[i].second)) return false;
  }
  return true;
}

std::string projectile_possessive(const Projectile& proj) {
  if (proj.owner_is_player) return "your";
  return (proj.owner_allegiance == Allegiance::Player ? "your " : "the ") + proj.owner_name + "'s";
}

std::string projectile_subject(const Projectile& proj) {
  if (proj.owner_is_player) return "Your " + proj.name;
  return (proj.owner_allegiance == Allegiance::Player ? "Your " : "The ") + proj.owner_name + "'s " + proj.name;
}

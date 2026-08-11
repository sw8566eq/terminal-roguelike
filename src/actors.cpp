#include "actors.hpp"

#include <algorithm>
#include <cstdlib>

#include "rules.hpp"

int distance_between(const Actor& a, const Actor& b) {
  return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

int monster_at(const std::vector<Actor>& monsters, int x, int y) {
  for (size_t i = 0; i < monsters.size(); ++i) {
    if (monsters[i].is_alive() && monsters[i].x == x && monsters[i].y == y) return static_cast<int>(i);
  }
  return -1;
}

int hostile_monster_at(const std::vector<Actor>& monsters, int x, int y) {
  for (size_t i = 0; i < monsters.size(); ++i) {
    if (monsters[i].allegiance == Allegiance::Hostile && monsters[i].is_alive() && monsters[i].x == x &&
        monsters[i].y == y) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int own_minion_at(const std::vector<Actor>& monsters, int x, int y) {
  for (size_t i = 0; i < monsters.size(); ++i) {
    if (monsters[i].allegiance == Allegiance::Player && monsters[i].is_alive() && monsters[i].x == x &&
        monsters[i].y == y) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int projectile_target_at(const std::vector<Actor>& monsters, int x, int y, Allegiance owner) {
  for (size_t i = 0; i < monsters.size(); ++i) {
    if (monsters[i].allegiance == owner) continue;
    if (monsters[i].is_alive() && monsters[i].x == x && monsters[i].y == y) return static_cast<int>(i);
  }
  return -1;
}

int actor_index_by_id(const std::vector<Actor>& actors, int id) {
  for (size_t i = 0; i < actors.size(); ++i) {
    if (actors[i].id == id && actors[i].is_alive()) return static_cast<int>(i);
  }
  return -1;
}

int auto_target_hostile(const std::vector<Actor>& monsters, const Actor& player, const Map& map, int last_target_id,
                        int range) {
  auto qualifies = [&](const Actor& m) {
    if (!map.is_in_fov(m.x, m.y)) return false;
    int dx = m.x - player.x;
    int dy = m.y - player.y;
    return dx * dx + dy * dy <= range * range;
  };

  int last_index = actor_index_by_id(monsters, last_target_id);
  if (last_index >= 0 && monsters[static_cast<size_t>(last_index)].allegiance == Allegiance::Hostile &&
      qualifies(monsters[static_cast<size_t>(last_index)])) {
    return last_target_id;
  }

  int best_id = -1, best_dist = -1;
  for (const auto& m : monsters) {
    if (m.allegiance != Allegiance::Hostile || !m.is_alive() || !qualifies(m)) continue;
    int dx = m.x - player.x, dy = m.y - player.y;
    int dist = dx * dx + dy * dy;
    if (best_id == -1 || dist < best_dist) {
      best_id = m.id;
      best_dist = dist;
    }
  }
  return best_id;
}

int closest_own_minion(const std::vector<Actor>& monsters, const Actor& player, const Map& map, int range) {
  int best_id = -1, best_dist = -1;
  for (const auto& m : monsters) {
    if (m.allegiance != Allegiance::Player || !m.is_alive()) continue;
    int dx = m.x - player.x, dy = m.y - player.y;
    int dist = dx * dx + dy * dy;
    if (dist > range * range) continue;
    if (!line_clear(player.x, player.y, m.x, m.y, map)) continue;
    if (best_id == -1 || dist < best_dist) {
      best_id = m.id;
      best_dist = dist;
    }
  }
  return best_id;
}

int count_minions(const std::vector<Actor>& monsters) {
  int count = 0;
  for (const auto& m : monsters) {
    if (m.allegiance == Allegiance::Player && m.is_alive()) ++count;
  }
  return count;
}

std::string describe_minion_order(const Actor& minion, const std::vector<Actor>& monsters) {
  if (minion.order == MinionOrder::Hold) return "holding position";
  if (minion.order == MinionOrder::AttackTarget) {
    int ti = actor_index_by_id(monsters, minion.attack_target_id);
    return ti >= 0 ? "attacking the " + monsters[static_cast<size_t>(ti)].name : "following you";
  }
  if (minion.order == MinionOrder::Aggressive) {
    int ti = actor_index_by_id(monsters, minion.attack_target_id);
    return ti >= 0 ? "engaging the " + monsters[static_cast<size_t>(ti)].name : "following you, watching for enemies";
  }
  return "following you";
}

std::string minion_order_flag(const Actor& minion) {
  switch (minion.order) {
    case MinionOrder::Hold:
      return "H";
    case MinionOrder::AttackTarget:
      return "A";
    case MinionOrder::Aggressive:
      return "G";
    default:
      return "F";
  }
}

std::string actor_subject(const Actor& a) {
  if (a.is_player) return "You";
  return (a.allegiance == Allegiance::Player ? "Your " : "The ") + a.name;
}

std::string actor_object(const Actor& a) {
  if (a.is_player) return "you";
  return (a.allegiance == Allegiance::Player ? "your " : "the ") + a.name;
}

std::string actor_possessive(const Actor& a) {
  if (a.is_player) return "your";
  return (a.allegiance == Allegiance::Player ? "your " : "the ") + a.name + "'s";
}

std::string actor_verb(const Actor& a, const std::string& base) { return a.is_player ? base : base + "s"; }

void equip_best_weapon_for_range(Actor& actor, int distance) {
  if (actor.weapons.empty()) return;
  auto usable = [&](const Weapon& w) {
    if (actor.melee_engaged && w.attack_range > 1) return false;
    return w.attack_range >= distance;
  };

  int best = -1;  // -1 means "nothing carried beats what's already equipped"
  double best_score = usable(actor.weapon) ? expected_damage(actor, actor.weapon) : -1.0;
  for (size_t i = 0; i < actor.weapons.size(); ++i) {
    if (!usable(actor.weapons[i])) continue;
    double score = expected_damage(actor, actor.weapons[i]);
    if (score > best_score) {
      best_score = score;
      best = static_cast<int>(i);
    }
  }
  if (best >= 0) std::swap(actor.weapon, actor.weapons[static_cast<size_t>(best)]);

  // Latched from whatever ends up equipped, whether or not a swap actually happened —
  // an Actor whose *starting* weapon is already the melee one (while it also carries a
  // longer-ranged one) reaches here with best < 0, and used to slip past this and never
  // commit to melee. No current table row is built that way, but the next one might be.
  if (actor.weapon.attack_range <= 1 && distance <= 1) actor.melee_engaged = true;
}

std::vector<ItemSlot> drop_slots(const Actor& actor) {
  std::vector<ItemSlot> slots;
  if (!actor.weapon.is_intrinsic) slots.push_back({ItemKind::Weapon, -1});
  for (size_t i = 0; i < actor.weapons.size(); ++i) slots.push_back({ItemKind::Weapon, static_cast<int>(i)});
  if (!actor.armor.is_intrinsic) slots.push_back({ItemKind::Armor, -1});
  for (size_t i = 0; i < actor.armors.size(); ++i) slots.push_back({ItemKind::Armor, static_cast<int>(i)});
  for (size_t i = 0; i < actor.potions.size(); ++i) slots.push_back({ItemKind::Potion, static_cast<int>(i)});
  return slots;
}

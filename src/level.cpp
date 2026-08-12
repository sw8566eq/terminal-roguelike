#include "level.hpp"

#include <algorithm>

#include "actors.hpp"
#include "rng.hpp"

bool g_debug_fast_monsters = false;

int allocate_actor_id() {
  static int next_id = 1;
  return next_id++;
}

Actor spawn_monster(int table_index, int x, int y) {
  const MonsterTemplate& tmpl = kMonsterTable[static_cast<size_t>(table_index)];
  Actor monster;
  monster.id = allocate_actor_id();
  monster.monster_template_index = table_index;
  monster.x = x;
  monster.y = y;
  monster.hp = monster.max_hp = tmpl.max_hp;
  monster.glyph = tmpl.glyph;
  monster.color = tmpl.color;
  monster.name = tmpl.name;
  monster.weapon = tmpl.weapon;
  monster.armor = tmpl.armor;
  monster.weapons = tmpl.extra_weapons;
  monster.potions = tmpl.potions;
  monster.xp_reward = tmpl.xp_reward;
  monster.evasion = tmpl.evasion;
  monster.dexterity = tmpl.dexterity;
  monster.strength = tmpl.strength;
  monster.hp_regen_turns = tmpl.hp_regen_turns;
  monster.extra_actions = tmpl.extra_actions + (g_debug_fast_monsters ? 1 : 0);
  monster.intelligence = tmpl.intelligence;
  monster.mana = monster.max_mana = tmpl.max_mana;  // spawns with a full pool, like max_hp
  monster.mana_regen_turns = tmpl.mana_regen_turns;
  monster.spell_index = tmpl.spell_index;
  return monster;
}

Actor spawn_minion(const MinionTemplate& tmpl, int x, int y) {
  Actor minion;
  minion.id = allocate_actor_id();
  minion.x = x;
  minion.y = y;
  minion.hp = minion.max_hp = tmpl.max_hp;
  minion.glyph = tmpl.glyph;
  minion.color = tmpl.color;
  minion.name = tmpl.name;
  minion.weapon = tmpl.weapon;
  minion.armor = tmpl.armor;
  minion.weapons = tmpl.extra_weapons;
  minion.potions = tmpl.potions;
  minion.evasion = tmpl.evasion;
  minion.dexterity = tmpl.dexterity;
  minion.strength = tmpl.strength;
  minion.hp_regen_turns = tmpl.hp_regen_turns;
  minion.extra_actions = tmpl.extra_actions;
  minion.duration_turns = tmpl.duration_turns;
  minion.abilities = tmpl.abilities;
  minion.mana = minion.max_mana = tmpl.max_mana;  // spawns with a full pool, like max_hp
  minion.mana_regen_turns = tmpl.mana_regen_turns;
  minion.allegiance = Allegiance::Player;
  return minion;
}

void drop_actor_gear(Level& level, const Actor& actor) {
  if (!actor.weapon.is_intrinsic) level.items.push_back(GroundItem{actor.x, actor.y, actor.weapon});
  for (const auto& w : actor.weapons) {
    if (!w.is_intrinsic) level.items.push_back(GroundItem{actor.x, actor.y, w});
  }
  if (!actor.armor.is_intrinsic) level.armor_items.push_back(GroundArmor{actor.x, actor.y, actor.armor});
  for (const auto& a : actor.armors) {
    if (!a.is_intrinsic) level.armor_items.push_back(GroundArmor{actor.x, actor.y, a});
  }
  for (const auto& p : actor.potions) level.potions.push_back(GroundPotion{actor.x, actor.y, p});
}

void update_monster_memory(Level& level) {
  for (const auto& monster : level.monsters) {
    if (!level.map.is_in_fov(monster.x, monster.y)) continue;
    // Your own minions are exempt: the renderer draws them live wherever they are, in
    // sight or not, because a summoner always knows where they are. Remembering one
    // would leave a stale copy at its last-seen tile *and* draw the real one at its
    // current tile — the same minion, twice. Memory is for things you might have lost
    // track of, which yours never are.
    if (monster.allegiance == Allegiance::Player) continue;

    bool updated = false;
    for (auto& remembered : level.remembered_monsters) {
      if (remembered.x == monster.x && remembered.y == monster.y) {
        remembered.glyph = monster.glyph;
        remembered.color = monster.color;
        updated = true;
        break;
      }
    }
    if (!updated) {
      level.remembered_monsters.push_back(RememberedMonster{monster.x, monster.y, monster.glyph, monster.color});
    }
  }

  level.remembered_monsters.erase(
      std::remove_if(level.remembered_monsters.begin(), level.remembered_monsters.end(),
                     [&](const RememberedMonster& remembered) {
                       if (!level.map.is_in_fov(remembered.x, remembered.y)) return false;  // out of sight, keep it
                       return !std::any_of(level.monsters.begin(), level.monsters.end(), [&](const Actor& m) {
                         return m.x == remembered.x && m.y == remembered.y;
                       });
                     }),
      level.remembered_monsters.end());
}

int monster_count_for_depth(int depth) { return std::min(NUM_MONSTERS + (depth - 1) / 2, NUM_MONSTERS + 5); }

std::pair<int, int> random_free_tile(const Map& map, const std::vector<std::pair<int, int>>& occupied,
                                     bool require_room) {
  for (;;) {
    int x = random_int(0, map.width() - 1);
    int y = random_int(0, map.height() - 1);
    if (!map.is_walkable(x, y)) continue;
    if (require_room && !map.is_in_room(x, y)) continue;
    bool taken = false;
    for (const auto& p : occupied) {
      if (p.first == x && p.second == y) {
        taken = true;
        break;
      }
    }
    if (!taken) return {x, y};
  }
}

bool free_adjacent_tile(const Map& map, const std::vector<Actor>& monsters, int x, int y, int& out_x, int& out_y) {
  const int offsets[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
  for (const auto& off : offsets) {
    int nx = x + off[0];
    int ny = y + off[1];
    if (!map.is_walkable(nx, ny)) continue;
    if (monster_at(monsters, nx, ny) >= 0) continue;
    out_x = nx;
    out_y = ny;
    return true;
  }
  return false;
}

Level generate_level(int width, int height, bool has_stairs_up, int depth) {
  Level level{Map(width, height), {}, {}, {}, {}, {}, {}, {}};
  auto [entry_x, entry_y] = level.map.generate(/*max_rooms=*/12, /*room_min_size=*/4, /*room_max_size=*/8);
  level.entry_x = entry_x;
  level.entry_y = entry_y;
  level.has_stairs_up = has_stairs_up;

  std::vector<std::pair<int, int>> occupied = {{entry_x, entry_y}};

  // Room-only so stairs never land in a corridor.
  auto [down_x, down_y] = random_free_tile(level.map, occupied, /*require_room=*/true);
  level.stairs_down_x = down_x;
  level.stairs_down_y = down_y;
  occupied.push_back({down_x, down_y});

  // Must run after stairs are placed (Map::generate() itself returns before
  // stairs_down_x/y are known) and before monsters/items are placed, so their
  // random_free_tile() calls skip Hole tiles for free via the ordinary is_walkable()
  // check, no extra bookkeeping needed.
  level.map.carve_hole_clusters(entry_x, entry_y, down_x, down_y);

  auto available_monsters = monsters_available_at_depth(depth);
  int monster_count = monster_count_for_depth(depth);
  for (int i = 0; i < monster_count; ++i) {
    auto [mx, my] = random_free_tile(level.map, occupied);
    occupied.push_back({mx, my});

    int table_index =
        available_monsters[static_cast<size_t>(random_int(0, static_cast<int>(available_monsters.size()) - 1))];
    level.monsters.push_back(spawn_monster(table_index, mx, my));
  }

  // Bosses are placed on top of that count rather than drawn from it: one guaranteed
  // spawn per boss row whose depth range covers this floor (see bosses_at_depth()).
  // Placed like any other monster otherwise — a random free tile, respecting `occupied`
  // — so it can land anywhere on the floor, not in a designated arena. Since levels
  // persist, a killed boss stays dead when you come back.
  for (int table_index : bosses_at_depth(depth)) {
    auto [bx, by] = random_free_tile(level.map, occupied);
    occupied.push_back({bx, by});
    level.monsters.push_back(spawn_monster(table_index, bx, by));
  }

  auto available_weapons = weapons_available_at_depth(depth);
  for (int i = 0; i < NUM_ITEMS; ++i) {
    auto [ix, iy] = random_free_tile(level.map, occupied);
    occupied.push_back({ix, iy});
    int table_index =
        available_weapons[static_cast<size_t>(random_int(0, static_cast<int>(available_weapons.size()) - 1))];
    level.items.push_back(GroundItem{ix, iy, kWeaponTable[static_cast<size_t>(table_index)]});
  }

  auto available_armor = armor_available_at_depth(depth);
  for (int i = 0; i < NUM_ARMOR; ++i) {
    auto [ax, ay] = random_free_tile(level.map, occupied);
    occupied.push_back({ax, ay});
    int table_index = available_armor[static_cast<size_t>(random_int(0, static_cast<int>(available_armor.size()) - 1))];
    level.armor_items.push_back(GroundArmor{ax, ay, kArmorTable[static_cast<size_t>(table_index)]});
  }

  auto available_potions = potions_available_at_depth(depth);
  for (int i = 0; i < NUM_POTIONS; ++i) {
    auto [px, py] = random_free_tile(level.map, occupied);
    occupied.push_back({px, py});
    int table_index =
        available_potions[static_cast<size_t>(random_int(0, static_cast<int>(available_potions.size()) - 1))];
    level.potions.push_back(GroundPotion{px, py, kPotionTable[static_cast<size_t>(table_index)]});
  }

  return level;
}

std::vector<ItemSlot> ground_slots_at(const Level& level, int x, int y) {
  std::vector<ItemSlot> slots;
  for (size_t i = 0; i < level.items.size(); ++i) {
    if (level.items[i].x == x && level.items[i].y == y) slots.push_back({ItemKind::Weapon, static_cast<int>(i)});
  }
  for (size_t i = 0; i < level.armor_items.size(); ++i) {
    if (level.armor_items[i].x == x && level.armor_items[i].y == y) {
      slots.push_back({ItemKind::Armor, static_cast<int>(i)});
    }
  }
  for (size_t i = 0; i < level.potions.size(); ++i) {
    if (level.potions[i].x == x && level.potions[i].y == y) slots.push_back({ItemKind::Potion, static_cast<int>(i)});
  }
  return slots;
}

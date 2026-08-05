#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <libtcod.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "entity.hpp"
#include "map.hpp"
#include "rng.hpp"

// --- Content tables (temporary; will move into a data-driven item/monster system) ---

constexpr int NUM_MONSTERS = 5;  // per floor
constexpr int NUM_ITEMS = 4;     // per floor
constexpr int NUM_ARMOR = 3;     // per floor

// Melee weapons that can be found lying on the floor.
const std::vector<Weapon> kWeaponTable = {
    {"Dagger", 1, 4, 0},
    {"Short Sword", 1, 6, 0},
    {"Mace", 1, 8, 0},
    {"Battle Axe", 2, 6, 0},
};

// Armor that can be found lying on the floor.
const std::vector<Armor> kArmorTable = {
    {"Leather Armor", 1},
    {"Chainmail", 3},
    {"Plate Armor", 5},
};

struct MonsterTemplate {
  std::string name;
  char glyph;
  tcod::ColorRGB color;
  int max_hp;
  Weapon weapon;
  int xp_reward;
  int evasion;    // percent chance to dodge the player's attack; monsters wear no armor
  int min_depth;  // first floor (1-indexed) this monster can spawn on
  int max_depth;  // last floor it can spawn on; -1 means no upper limit
};

// Roughly increasing toughness/reward with min_depth, so descending gets harder. Each
// "tier" fully replaces the previous one at its cutover rather than overlapping: Rat
// and Goblin stop past floor 4, and Skeleton/Orc pick up as the new baseline exactly
// where they leave off, same idea Troll will hand off to whatever comes after it.
const std::vector<MonsterTemplate> kMonsterTable = {
    {"Rat", 'r', tcod::ColorRGB{150, 100, 60}, 4, Weapon{"Bite", 1, 3, 0}, /*xp_reward=*/5, /*evasion=*/15,
     /*min_depth=*/1, /*max_depth=*/4},
    {"Goblin", 'g', tcod::ColorRGB{80, 180, 80}, 7, Weapon{"Claws", 1, 4, 0}, /*xp_reward=*/10, /*evasion=*/5,
     /*min_depth=*/1, /*max_depth=*/4},
    {"Skeleton", 's', tcod::ColorRGB{220, 220, 200}, 10, Weapon{"Rusty Sword", 1, 6, 0}, /*xp_reward=*/15,
     /*evasion=*/8, /*min_depth=*/5, /*max_depth=*/-1},
    {"Orc", 'o', tcod::ColorRGB{60, 120, 60}, 14, Weapon{"Orc Axe", 1, 8, 0}, /*xp_reward=*/22, /*evasion=*/5,
     /*min_depth=*/5, /*max_depth=*/-1},
    {"Troll", 'T', tcod::ColorRGB{100, 110, 80}, 22, Weapon{"Massive Club", 2, 6, 0}, /*xp_reward=*/40,
     /*evasion=*/2, /*min_depth=*/8, /*max_depth=*/-1},
};

// Indices into kMonsterTable of every monster that can spawn at the given floor depth
// (1-indexed). Kept as one function, same pattern as known_spell_indices, so the spawn
// logic can't drift from whatever the table actually says.
std::vector<int> monsters_available_at_depth(int depth) {
  std::vector<int> indices;
  for (size_t i = 0; i < kMonsterTable.size(); ++i) {
    const MonsterTemplate& tmpl = kMonsterTable[i];
    bool below_max = tmpl.max_depth < 0 || depth <= tmpl.max_depth;
    if (depth >= tmpl.min_depth && below_max) indices.push_back(static_cast<int>(i));
  }
  return indices;
}

// Max HP scales with Strength, so there's no need for a separate Vitality stat.
int max_hp_for_strength(int strength) { return 10 + strength * 5; }

// Evasion (percent chance to dodge an attack entirely) scales with Dexterity, capped
// so the player can never become effectively unhittable.
int evasion_for_dexterity(int dexterity) { return std::min(dexterity * 3, 40); }

// XP required to advance from the given level to the next one.
int xp_needed_for_level(int level) { return level * 20; }

// The player's default, always-available unarmed attack. Not a real pickup, so it's
// never added to the ground or the inventory list.
const Weapon kFists = Weapon{"Fists", 1, 2, 0, /*is_intrinsic=*/true};

// The player's default, always-available "armor" (bare skin, no defense). Not a real
// pickup, same idea as kFists.
const Armor kNoArmor = Armor{"Nothing", 0, /*is_intrinsic=*/true};

// A ranged spell: damage is dice_count dice of dice_sides each, plus floor(INT/3).
// Known automatically once the player's Intelligence reaches unlock_int — nothing to
// learn or pick up, it just unlocks. (Thresholds are placeholders until the rest of
// the spell pool is designed; there's only one spell so far.)
//
// speed is tiles traveled per player turn, not real time — there's no animation.
// "Instant" just means a speed high enough to always cross the whole map in one turn;
// slower future spells would take multiple turns to reach a distant target.
struct Spell {
  std::string name;
  int unlock_int;
  int dice_count;
  int dice_sides;
  int speed;
  int range;  // max cast distance from the caster, in tiles (straight-line)
  char glyph;
  tcod::ColorRGB color;
};

constexpr int kInstantSpellSpeed = 99;  // safely more tiles than this map's diagonal

const std::vector<Spell> kSpellTable = {
    // range happens to match the player's starting FOV radius today, but it's its own
    // fixed number — it won't change if FOV radius ever does (e.g. a future perception
    // mechanic).
    {"Magic Dart", /*unlock_int=*/3, /*dice_count=*/1, /*dice_sides=*/2, kInstantSpellSpeed, /*range=*/8, '*',
     tcod::ColorRGB{200, 100, 255}},
};

// Indices into kSpellTable of every spell the player currently knows, in display
// order. Kept as one function so the spell-menu render and input code can't drift.
std::vector<int> known_spell_indices(int intelligence) {
  std::vector<int> indices;
  for (size_t i = 0; i < kSpellTable.size(); ++i) {
    if (intelligence >= kSpellTable[i].unlock_int) indices.push_back(static_cast<int>(i));
  }
  return indices;
}

// A weapon lying on the floor, waiting to be picked up.
struct GroundItem {
  int x, y;
  Weapon weapon;
};

// A piece of armor lying on the floor, waiting to be picked up.
struct GroundArmor {
  int x, y;
  Armor armor;
};

// A spell in flight: advances along a precomputed path by `speed` tiles every player
// turn (see advance_projectiles), hitting the first wall or monster it reaches.
struct Projectile {
  std::vector<std::pair<int, int>> path;  // tiles from just past the caster through the target
  size_t path_index = 0;                  // how many tiles of the path have been consumed so far
  int speed = 1;
  int dice_count = 1;
  int dice_sides = 2;
  int bonus = 0;  // locked in at cast time (e.g. floor(INT/3)), not re-read later
  std::string name;
  char glyph = '*';
  tcod::ColorRGB color{255, 255, 255};
};

// Every tile from just past (from_x,from_y) through (to_x,to_y), via libtcod's
// Bresenham line. Excludes the starting tile so a projectile doesn't "hit" its caster.
std::vector<std::pair<int, int>> trace_path(int from_x, int from_y, int to_x, int to_y) {
  std::vector<std::pair<int, int>> path;
  for (auto [x, y] : tcod::BresenhamLine({from_x, from_y}, {to_x, to_y}).without_start()) {
    path.push_back({x, y});
  }
  return path;
}

// A monster glyph remembered at a tile after it's no longer in view — fog-of-war
// memory, but for monsters instead of terrain. Once monster AI can move them around,
// this may go stale (the monster shown there may no longer actually be there), same
// as remembering "there was a rat over there" doesn't guarantee it stayed put.
struct RememberedMonster {
  int x, y;
  char glyph;
  tcod::ColorRGB color;
};

// Darkens a color for the "remembered, but not currently visible" rendering tier.
tcod::ColorRGB dim_color(tcod::ColorRGB c) {
  return tcod::ColorRGB{static_cast<uint8_t>(c.r / 3), static_cast<uint8_t>(c.g / 3), static_cast<uint8_t>(c.b / 3)};
}

// Formats a weapon as e.g. "1d6" or "2d6+1", for the HUD.
std::string describe_weapon(const Weapon& weapon) {
  std::string desc = std::to_string(weapon.dice_count) + "d" + std::to_string(weapon.dice_sides);
  if (weapon.bonus != 0) desc += "+" + std::to_string(weapon.bonus);
  return desc;
}

// Picks a random walkable tile that isn't already in `occupied`. If require_room is
// true, corridor tiles are skipped too — used to keep stairs out of corridors.
std::pair<int, int> random_free_tile(const Map& map, const std::vector<std::pair<int, int>>& occupied,
                                      bool require_room = false) {
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

enum class ItemKind { Weapon, Armor };

// One selectable row in the equip or drop screen. index == -1 means the intrinsic
// default (Fists / Nothing) for Weapon/Armor respectively; otherwise it's an index
// into `inventory` or `armor_inventory`.
struct ItemSlot {
  ItemKind kind;
  int index;
};

// The droppable list: the currently equipped weapon/armor (omitted if intrinsic),
// followed by everything carried of each kind.
std::vector<ItemSlot> drop_slots(const Actor& player, const std::vector<Weapon>& inventory,
                                  const std::vector<Armor>& armor_inventory) {
  std::vector<ItemSlot> slots;
  if (!player.weapon.is_intrinsic) slots.push_back({ItemKind::Weapon, -1});
  for (size_t i = 0; i < inventory.size(); ++i) slots.push_back({ItemKind::Weapon, static_cast<int>(i)});
  if (!player.armor.is_intrinsic) slots.push_back({ItemKind::Armor, -1});
  for (size_t i = 0; i < armor_inventory.size(); ++i) slots.push_back({ItemKind::Armor, static_cast<int>(i)});
  return slots;
}

// Formats an armor piece as e.g. "+3", for the HUD/menus.
std::string describe_armor(const Armor& armor) { return "+" + std::to_string(armor.defense); }

// One dungeon floor: its own map, monsters, and items. Levels are generated once and
// then kept around (not regenerated) so going back upstairs returns to how you left it.
struct Level {
  Map map;
  std::vector<Actor> monsters;
  std::vector<GroundItem> items;
  std::vector<GroundArmor> armor_items;
  std::vector<RememberedMonster> remembered_monsters;  // last-seen monster sightings, may go stale
  std::vector<Projectile> projectiles;  // spells currently in flight on this floor
  int entry_x = 0;           // where the player arrives on this floor
  int entry_y = 0;
  bool has_stairs_up = false;  // whether entry_x/y doubles as a stairs-up tile (false on floor 1)
  int stairs_down_x = 0;
  int stairs_down_y = 0;
};

// Refreshes monster memory: records/updates a sighting for every monster currently in
// view, and forgets any remembered sighting whose tile we can currently see but which
// no longer has a monster on it (it moved on, died, or was never really there anymore).
void update_monster_memory(Level& level) {
  for (const auto& monster : level.monsters) {
    if (!level.map.is_in_fov(monster.x, monster.y)) continue;

    bool updated = false;
    for (auto& remembered : level.remembered_monsters) {
      if (remembered.x == monster.x && remembered.y == monster.y) {
        remembered.glyph = monster.glyph;
        remembered.color = monster.color;
        updated = true;
        break;
      }
    }
    if (!updated) level.remembered_monsters.push_back(RememberedMonster{monster.x, monster.y, monster.glyph, monster.color});
  }

  level.remembered_monsters.erase(
      std::remove_if(level.remembered_monsters.begin(), level.remembered_monsters.end(),
                      [&](const RememberedMonster& remembered) {
                        if (!level.map.is_in_fov(remembered.x, remembered.y)) return false;  // still out of sight, keep it
                        return !std::any_of(level.monsters.begin(), level.monsters.end(), [&](const Actor& m) {
                          return m.x == remembered.x && m.y == remembered.y;
                        });
                      }),
      level.remembered_monsters.end());
}

// Monster count grows gently with depth, capped so deep floors don't get absurd.
int monster_count_for_depth(int depth) { return std::min(NUM_MONSTERS + (depth - 1) / 2, NUM_MONSTERS + 5); }

// Builds and populates a fresh floor. depth is 1-indexed (matches the "Floor:N" HUD)
// and gates which monsters can spawn here, plus how many.
Level generate_level(int width, int height, bool has_stairs_up, int depth) {
  Level level{Map(width, height), {}, {}, {}, {}, {}};
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

  auto available_monsters = monsters_available_at_depth(depth);
  int monster_count = monster_count_for_depth(depth);
  for (int i = 0; i < monster_count; ++i) {
    auto [mx, my] = random_free_tile(level.map, occupied);
    occupied.push_back({mx, my});

    int table_index = available_monsters[static_cast<size_t>(random_int(0, static_cast<int>(available_monsters.size()) - 1))];
    const MonsterTemplate& tmpl = kMonsterTable[static_cast<size_t>(table_index)];
    Actor monster;
    monster.x = mx;
    monster.y = my;
    monster.hp = monster.max_hp = tmpl.max_hp;
    monster.glyph = tmpl.glyph;
    monster.color = tmpl.color;
    monster.name = tmpl.name;
    monster.weapon = tmpl.weapon;
    monster.xp_reward = tmpl.xp_reward;
    monster.evasion = tmpl.evasion;
    level.monsters.push_back(monster);
  }

  for (int i = 0; i < NUM_ITEMS; ++i) {
    auto [ix, iy] = random_free_tile(level.map, occupied);
    occupied.push_back({ix, iy});
    level.items.push_back(GroundItem{ix, iy, kWeaponTable[random_int(0, static_cast<int>(kWeaponTable.size()) - 1)]});
  }

  for (int i = 0; i < NUM_ARMOR; ++i) {
    auto [ax, ay] = random_free_tile(level.map, occupied);
    occupied.push_back({ax, ay});
    level.armor_items.push_back(GroundArmor{ax, ay, kArmorTable[random_int(0, static_cast<int>(kArmorTable.size()) - 1)]});
  }

  return level;
}

// Common monospace font paths, one per Linux distro this project's README documents
// setup for. Tried in order; the first one found is used. This approximates "use the
// font your terminal uses" without a fontconfig dependency or bundling a font file:
// on an unconfigured terminal (no custom font override), these paths ARE what
// fontconfig's "monospace" alias resolves to on each respective distro.
const std::vector<std::string> kPreferredFontPaths = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",         // Debian/Ubuntu
    "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",  // Fedora
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",                     // Arch
};

// Loads the first font from kPreferredFontPaths that exists on disk, rendered at
// tile_size x tile_size pixels per cell. Falls back to libtcod's built-in font (same
// one used before this project had any font-selection logic) if none of them exist.
tcod::TilesetPtr load_best_tileset(int tile_size) {
  for (const auto& path : kPreferredFontPaths) {
    if (!std::filesystem::exists(path)) continue;
    tcod::TilesetPtr tileset{TCOD_load_truetype_font_(path.c_str(), tile_size, tile_size)};
    if (tileset) return tileset;
  }
  return tcod::tileset::new_fallback_tileset({tile_size, tile_size});
}

int main(int argc, char* argv[]) {
  constexpr int SCREEN_WIDTH = 100;
  constexpr int SCREEN_HEIGHT = 32;
  constexpr int MESSAGE_ROWS = 3;  // how many of the most recent distinct log entries are shown at once
  constexpr int HUD_HEIGHT = 2 + MESSAGE_ROWS;  // stats, gear, then the message — not part of the map
  constexpr int MAP_WIDTH = SCREEN_WIDTH;
  constexpr int MAP_HEIGHT = SCREEN_HEIGHT - HUD_HEIGHT;
  constexpr int FOV_RADIUS = 8;  // how far the player can see; unrelated to any spell's range
  constexpr int TILE_SIZE = 18;  // pixels per cell; square, so tiles aren't stretched

  auto console = tcod::Console{SCREEN_WIDTH, SCREEN_HEIGHT};  // Main console.

  // Explicitly pick a font instead of leaving tileset null: with none set, libtcod
  // tries to load a "terminal.png" from disk (which this project doesn't ship, hence
  // the "Error loading font image" warning at startup), then silently falls back to
  // its built-in font anyway, but at whatever tiny default size it picks. Doing it
  // ourselves skips the failed disk lookup, lets us pick a real size, and tries to
  // match the font your terminal would normally show text in.
  auto tileset = load_best_tileset(TILE_SIZE);

  // Configure the context.
  auto params = TCOD_ContextParams{};
  params.console = console.get();  // Derive the window size from the console size.
  params.tileset = tileset.get();
  params.window_title = "Terminal Roguelike";
  params.sdl_window_flags = SDL_WINDOW_RESIZABLE;
  params.vsync = true;
  params.argc = argc;
  params.argv = argv;

  auto context = tcod::Context(params);

  Actor player;
  player.glyph = '@';
  player.color = tcod::ColorRGB{255, 255, 0};
  player.name = "Player";

  std::vector<Weapon> inventory;
  std::vector<Armor> armor_inventory;
  std::vector<std::string> message_log;  // full history; the HUD shows the last MESSAGE_ROWS entries
  int log_scroll = 0;  // lines scrolled up from the bottom, while Mode::MessageLog
  std::string death_cause;  // name of whatever last killed the player, for the death screen
  int pending_attribute_points = 0;  // unspent level-up points forcing a Mode::LevelUp prompt

  // Records a new, distinct message as its own log entry. Everything that happens
  // becomes its own line, even multiple things on the same turn (e.g. an attack
  // landing and the target retaliating) — nothing ever gets concatenated into one.
  auto add_message = [&](const std::string& text) {
    if (!text.empty()) message_log.push_back(text);
  };

  enum class Mode { Playing, WeaponMenu, ArmorMenu, Drop, Dead, LevelUp, SpellMenu, Targeting, MessageLog };
  int casting_spell_index = -1;  // which kSpellTable entry is being aimed, while Mode::Targeting
  int target_x = 0;              // targeting cursor position, while Mode::Targeting
  int target_y = 0;
  Mode mode = Mode::Playing;

  std::vector<Level> levels;
  int current_level = 0;

  // Grants XP and processes any level-ups it triggers (normally one, but a large XP
  // reward could trigger several), queuing a forced attribute-point prompt for each.
  auto grant_xp = [&](int amount) {
    player.xp += amount;
    while (player.xp >= xp_needed_for_level(player.level)) {
      player.xp -= xp_needed_for_level(player.level);
      player.level += 1;
      pending_attribute_points += 1;
    }
    if (pending_attribute_points > 0) mode = Mode::LevelUp;
  };

  // Advances every in-flight projectile on the current floor by its speed (in tiles),
  // checking each tile it passes through this turn for a wall or monster to hit.
  // Called once per player turn, from end_turn().
  auto advance_projectiles = [&]() {
    Level& level = levels[static_cast<size_t>(current_level)];
    for (size_t i = 0; i < level.projectiles.size();) {
      Projectile& proj = level.projectiles[i];
      bool consumed = false;

      for (int step = 0; step < proj.speed && !consumed; ++step) {
        if (proj.path_index >= proj.path.size()) {
          consumed = true;  // reached the target tile with nothing there; the spell dissipates
          break;
        }
        auto [x, y] = proj.path[proj.path_index];
        ++proj.path_index;

        if (!level.map.is_walkable(x, y)) {
          add_message("Your " + proj.name + " fizzles against a wall.");
          consumed = true;
          break;
        }

        int target_index = -1;
        for (size_t m = 0; m < level.monsters.size(); ++m) {
          if (level.monsters[m].x == x && level.monsters[m].y == y) {
            target_index = static_cast<int>(m);
            break;
          }
        }
        if (target_index >= 0) {
          Actor& target = level.monsters[static_cast<size_t>(target_index)];
          int damage = roll_dice(proj.dice_count, proj.dice_sides) + proj.bonus;
          target.hp -= damage;
          if (!target.is_alive()) {
            add_message("Your " + proj.name + " kills the " + target.name + "!");
            int xp_reward = target.xp_reward;  // read before erase invalidates `target`
            level.monsters.erase(level.monsters.begin() + target_index);
            grant_xp(xp_reward);
          } else {
            add_message("Your " + proj.name + " hits the " + target.name + " for " + std::to_string(damage) + ".");
          }
          consumed = true;
        }
      }

      if (consumed) {
        level.projectiles.erase(level.projectiles.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
  };

  // Runs after the player's turn: every living monster still adjacent to the player
  // gets to attack. (Movement/chasing AI will plug into this same turn boundary later.)
  auto end_turn = [&]() {
    advance_projectiles();

    Level& level = levels[static_cast<size_t>(current_level)];

    // Tries to step a monster by (step_dx, step_dy); does nothing and returns false if
    // that tile is a wall or already has another living monster on it.
    auto try_monster_step = [&](Actor& m, int step_dx, int step_dy) -> bool {
      if (step_dx == 0 && step_dy == 0) return false;
      int nx = m.x + step_dx;
      int ny = m.y + step_dy;
      if (!level.map.is_walkable(nx, ny)) return false;
      for (const auto& other : level.monsters) {
        if (&other != &m && other.is_alive() && other.x == nx && other.y == ny) return false;
      }
      m.x = nx;
      m.y = ny;
      return true;
    };

    for (auto& monster : level.monsters) {
      if (mode == Mode::Dead) break;  // player already died to an earlier monster this turn
      if (!monster.is_alive()) continue;

      int dx = player.x - monster.x;
      int dy = player.y - monster.y;
      int abs_dx = dx < 0 ? -dx : dx;
      int abs_dy = dy < 0 ? -dy : dy;
      bool adjacent = abs_dx <= 1 && abs_dy <= 1 && (dx != 0 || dy != 0);

      if (adjacent) {
        if (random_int(1, 100) <= player.evasion) {
          add_message("You dodge the " + monster.name + "'s attack!");
          continue;
        }

        int raw_damage = roll_damage(monster.weapon);
        int damage = std::max(raw_damage - player.armor.defense, 0);
        player.hp -= damage;
        add_message("The " + monster.name + " hits you for " + std::to_string(damage) + ".");
        if (!player.is_alive()) {
          death_cause = monster.name;
          mode = Mode::Dead;
        }
        continue;
      }

      // Not adjacent: chase if the player would currently see this tile (shadowcasting
      // FOV is reciprocal, so this doubles as "can the monster see the player" without
      // computing a separate FOV per monster); otherwise wander idly. Movement is a
      // simple greedy step toward the player, not real pathfinding — monsters can still
      // get stuck on awkward corners, but that's a fine starting point.
      int move_dx = 0;
      int move_dy = 0;
      if (level.map.is_in_fov(monster.x, monster.y)) {
        move_dx = (dx > 0) - (dx < 0);  // sign(dx): one tile toward the player
        move_dy = (dy > 0) - (dy < 0);
      } else if (random_int(0, 1) == 0) {
        // Wander: only a coin-flip chance to shuffle each turn, so it reads as idle
        // rather than frantic.
        move_dx = random_int(-1, 1);
        move_dy = random_int(-1, 1);
      }
      if (move_dx == 0 && move_dy == 0) continue;

      // Try the intended step, then fall back to a single-axis step if it's blocked
      // (e.g. a diagonal clipped by a wall corner).
      if (!try_monster_step(monster, move_dx, move_dy)) {
        if (!try_monster_step(monster, move_dx, 0)) try_monster_step(monster, 0, move_dy);
      }
    }
  };

  // (Re)generates the dungeon and populates it, for both the initial game and every
  // restart after death.
  auto start_new_game = [&]() {
    levels.clear();
    levels.push_back(generate_level(MAP_WIDTH, MAP_HEIGHT, /*has_stairs_up=*/false, /*depth=*/1));
    current_level = 0;

    Level& level = levels[static_cast<size_t>(current_level)];
    player.x = level.entry_x;
    player.y = level.entry_y;
    player.strength = 2;
    player.dexterity = 2;
    player.intelligence = 2;
    player.level = 1;
    player.xp = 0;
    player.max_hp = max_hp_for_strength(player.strength);
    player.hp = player.max_hp;
    player.evasion = evasion_for_dexterity(player.dexterity);
    player.weapon = kFists;
    player.armor = kNoArmor;
    level.map.update_fov(player.x, player.y, FOV_RADIUS);

    inventory.clear();
    armor_inventory.clear();
    pending_attribute_points = 0;
    message_log.clear();
    add_message("Walk into an enemy to attack. hjkl/yubn or arrows to move, '.' wait, ']' log.");
    add_message("'g' get, 'w' weapons, 'a' armor, 'd' drop, 'z' spells.");
    add_message("'>'/'<' for stairs, Esc to quit.");
    mode = Mode::Playing;
  };

  // Goes down the stairs the player is currently standing on, generating the floor
  // below the first time it's visited.
  auto descend = [&]() {
    current_level += 1;
    if (static_cast<size_t>(current_level) >= levels.size()) {
      levels.push_back(generate_level(MAP_WIDTH, MAP_HEIGHT, /*has_stairs_up=*/true, /*depth=*/current_level + 1));
    }
    Level& level = levels[static_cast<size_t>(current_level)];
    player.x = level.entry_x;
    player.y = level.entry_y;
    level.map.update_fov(player.x, player.y, FOV_RADIUS);
    add_message("You descend the stairs.");
  };

  // Goes back up to the floor above, landing on the stairs down that was taken from it.
  auto ascend = [&]() {
    current_level -= 1;
    Level& level = levels[static_cast<size_t>(current_level)];
    player.x = level.stairs_down_x;
    player.y = level.stairs_down_y;
    level.map.update_fov(player.x, player.y, FOV_RADIUS);
    add_message("You ascend the stairs.");
  };

  start_new_game();

  // Debug convenience: `--floor=N` jumps straight to floor N at startup, so testing
  // deep floors doesn't require a long walk down through every floor above it. Not
  // meant for normal play; a missing/malformed value is just silently ignored.
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const std::string prefix = "--floor=";
    if (arg.rfind(prefix, 0) != 0) continue;
    int target_floor = std::atoi(arg.c_str() + prefix.size());
    for (int f = 1; f < target_floor; ++f) descend();
  }

  bool running = true;

  while (running) {
    Level& level = levels[static_cast<size_t>(current_level)];

    // --- Render ---
    console.clear();

    if (mode == Mode::WeaponMenu) {
      tcod::print(console, {0, 0}, "Weapons - press a letter to equip, Esc to close", tcod::ColorRGB{255, 255, 255},
                  std::nullopt);
      tcod::print(console, {0, 1}, "Equipped: " + player.weapon.name + " (" + describe_weapon(player.weapon) + ")",
                  tcod::ColorRGB{200, 200, 100}, std::nullopt);

      // Fists is always slot 'a', so you can always bail back to unarmed; carried
      // weapons fill 'b' onward.
      std::string fists_line = "a) Fists (" + describe_weapon(kFists) + ")";
      if (player.weapon.is_intrinsic) fists_line += " [equipped]";
      tcod::print(console, {0, 3}, fists_line, tcod::ColorRGB{200, 200, 200}, std::nullopt);

      for (size_t i = 0; i < inventory.size(); ++i) {
        std::string line = std::string(1, static_cast<char>('b' + i)) + ") " + inventory[i].name + " (" +
                            describe_weapon(inventory[i]) + ")";
        tcod::print(console, {0, 4 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
      }
    } else if (mode == Mode::ArmorMenu) {
      tcod::print(console, {0, 0}, "Armor - press a letter to equip, Esc to close", tcod::ColorRGB{255, 255, 255},
                  std::nullopt);
      tcod::print(console, {0, 1}, "Equipped: " + player.armor.name + " (" + describe_armor(player.armor) + ")",
                  tcod::ColorRGB{200, 200, 100}, std::nullopt);

      // "Nothing" is always slot 'a', so you can always bail back to unarmored; carried
      // armor fills 'b' onward.
      std::string none_line = "a) " + kNoArmor.name + " (" + describe_armor(kNoArmor) + ")";
      if (player.armor.is_intrinsic) none_line += " [equipped]";
      tcod::print(console, {0, 3}, none_line, tcod::ColorRGB{200, 200, 200}, std::nullopt);

      for (size_t i = 0; i < armor_inventory.size(); ++i) {
        std::string line = std::string(1, static_cast<char>('b' + i)) + ") " + armor_inventory[i].name + " (" +
                            describe_armor(armor_inventory[i]) + ")";
        tcod::print(console, {0, 4 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
      }
    } else if (mode == Mode::SpellMenu) {
      tcod::print(console, {0, 0}, "Spells - press a letter to cast, Esc to close", tcod::ColorRGB{255, 255, 255},
                  std::nullopt);

      auto known = known_spell_indices(player.intelligence);
      if (known.empty()) {
        tcod::print(console, {0, 2}, "(no spells known yet)", tcod::ColorRGB{120, 120, 120}, std::nullopt);
      }
      for (size_t i = 0; i < known.size(); ++i) {
        const Spell& s = kSpellTable[static_cast<size_t>(known[i])];
        std::string line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (" +
                            std::to_string(s.dice_count) + "d" + std::to_string(s.dice_sides) + "+INT/3)";
        tcod::print(console, {0, 2 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
      }
    } else if (mode == Mode::Drop) {
      tcod::print(console, {0, 0}, "Drop - press a letter to drop, Esc to cancel", tcod::ColorRGB{255, 255, 255},
                  std::nullopt);

      auto slots = drop_slots(player, inventory, armor_inventory);
      if (slots.empty()) {
        tcod::print(console, {0, 2}, "(nothing to drop)", tcod::ColorRGB{120, 120, 120}, std::nullopt);
      }
      for (size_t i = 0; i < slots.size(); ++i) {
        char letter = static_cast<char>('a' + i);
        std::string line;
        if (slots[i].kind == ItemKind::Weapon) {
          const Weapon& w = (slots[i].index == -1) ? player.weapon : inventory[static_cast<size_t>(slots[i].index)];
          line = std::string(1, letter) + ") " + w.name + " (" + describe_weapon(w) + ")";
          if (slots[i].index == -1) line += " [equipped]";
        } else {
          const Armor& a = (slots[i].index == -1) ? player.armor : armor_inventory[static_cast<size_t>(slots[i].index)];
          line = std::string(1, letter) + ") " + a.name + " (" + describe_armor(a) + ")";
          if (slots[i].index == -1) line += " [equipped]";
        }
        tcod::print(console, {0, 2 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
      }
    } else if (mode == Mode::Dead) {
      tcod::print(console, {0, 0}, "You died, slain by the " + death_cause + ".", tcod::ColorRGB{255, 80, 80},
                  std::nullopt);
      tcod::print(console, {0, 2}, "Press any key to start a new game, or Esc to quit.", tcod::ColorRGB{200, 200, 200},
                  std::nullopt);
    } else if (mode == Mode::MessageLog) {
      tcod::print(console, {0, 0}, "Message Log - j/k or arrows to scroll, ']' or Esc to close",
                  tcod::ColorRGB{255, 255, 255}, std::nullopt);

      int visible_rows = SCREEN_HEIGHT - 1;
      int total = static_cast<int>(message_log.size());
      int max_scroll = std::max(0, total - visible_rows);
      log_scroll = std::min(log_scroll, max_scroll);  // clamp in case the log shrank (e.g. after a restart)

      // Oldest at top, newest at bottom, like a terminal scrollback — log_scroll is how
      // many lines scrolled up from the bottom (0 = showing the most recent messages).
      int end_index = total - log_scroll;
      int start_index = std::max(0, end_index - visible_rows);
      for (int i = start_index; i < end_index; ++i) {
        int row = 1 + (i - start_index);
        tcod::print(console, {0, row}, message_log[static_cast<size_t>(i)], tcod::ColorRGB{200, 200, 200},
                    std::nullopt);
      }
    } else {
      update_monster_memory(level);

      // Row 0: HP/level/floor. Row 1: attributes + gear. Rows 2+: the last couple of
      // distinct log messages (or the level-up/targeting prompt) — kept on their own
      // lines so a long stats prefix can't crowd them out.
      std::string status_line = "HP:" + std::to_string(player.hp) + "/" + std::to_string(player.max_hp) +
                                 " Lvl:" + std::to_string(player.level) + " Floor:" + std::to_string(current_level + 1);
      tcod::print(console, {0, 0}, status_line, tcod::ColorRGB{255, 255, 255}, std::nullopt);

      std::string gear_line = "STR:" + std::to_string(player.strength) + " DEX:" + std::to_string(player.dexterity) +
                               " INT:" + std::to_string(player.intelligence) + " Wpn:" + player.weapon.name + "(" +
                               describe_weapon(player.weapon) + ") Arm:" + player.armor.name + "(" +
                               describe_armor(player.armor) + ")";
      tcod::print(console, {0, 1}, gear_line, tcod::ColorRGB{200, 200, 200}, std::nullopt);

      if (mode == Mode::LevelUp) {
        std::string prompt = "*** LEVEL UP (now level " + std::to_string(player.level) +
                              ")! Press Shift+S/D/I to raise Strength/Dexterity/Intelligence. ***";
        tcod::print(console, {0, 2}, prompt, tcod::ColorRGB{255, 255, 100}, std::nullopt);
      } else if (mode == Mode::Targeting) {
        std::string prompt = "Casting " + kSpellTable[static_cast<size_t>(casting_spell_index)].name +
                              " - move to target, Enter to fire, Esc to cancel.";
        tcod::print(console, {0, 2}, prompt, tcod::ColorRGB{255, 255, 100}, std::nullopt);
      } else {
        // Always exactly the last MESSAGE_ROWS distinct messages, oldest on top, one per line —
        // never wrapped or combined, even if several things happened on the same turn.
        int total = static_cast<int>(message_log.size());
        for (int row = 0; row < MESSAGE_ROWS; ++row) {
          int idx = total - MESSAGE_ROWS + row;
          if (idx < 0) continue;
          tcod::print(console, {0, 2 + row}, message_log[static_cast<size_t>(idx)], tcod::ColorRGB{255, 255, 100},
                      std::nullopt);
        }
      }

      for (int y = 0; y < level.map.height(); ++y) {
        for (int x = 0; x < level.map.width(); ++x) {
          if (!level.map.is_explored(x, y)) continue;  // never seen: leave blank

          bool walkable = level.map.is_walkable(x, y);
          bool visible = level.map.is_in_fov(x, y);
          bool is_stairs_down = (x == level.stairs_down_x && y == level.stairs_down_y);
          bool is_stairs_up = level.has_stairs_up && (x == level.entry_x && y == level.entry_y);

          auto& cell = console.at(x, y + HUD_HEIGHT);
          if (is_stairs_down) {
            cell.ch = '>';
          } else if (is_stairs_up) {
            cell.ch = '<';
          } else {
            cell.ch = walkable ? '.' : '#';
          }

          if (visible) {
            if (is_stairs_down || is_stairs_up) {
              cell.fg = tcod::ColorRGB{255, 255, 150};
            } else {
              cell.fg = walkable ? tcod::ColorRGB{160, 160, 160} : tcod::ColorRGB{90, 90, 90};
            }
          } else {
            // Remembered but currently out of sight: dimmed fog-of-war shading.
            if (is_stairs_down || is_stairs_up) {
              cell.fg = tcod::ColorRGB{110, 110, 70};
            } else {
              cell.fg = walkable ? tcod::ColorRGB{60, 60, 60} : tcod::ColorRGB{35, 35, 35};
            }
          }
        }
      }

      // Remembered monster sightings: dimmed, drawn only where we can't currently see
      // (the live loop below draws anything actually visible, on top, at full brightness).
      for (const auto& remembered : level.remembered_monsters) {
        if (level.map.is_in_fov(remembered.x, remembered.y)) continue;
        auto& cell = console.at(remembered.x, remembered.y + HUD_HEIGHT);
        cell.ch = remembered.glyph;
        cell.fg = dim_color(remembered.color);
      }

      // Items/monsters only show up while actually in view, unlike remembered terrain.
      for (const auto& item : level.items) {
        if (!level.map.is_in_fov(item.x, item.y)) continue;
        auto& cell = console.at(item.x, item.y + HUD_HEIGHT);
        cell.ch = '/';
        cell.fg = tcod::ColorRGB{200, 200, 255};
      }

      for (const auto& armor_item : level.armor_items) {
        if (!level.map.is_in_fov(armor_item.x, armor_item.y)) continue;
        auto& cell = console.at(armor_item.x, armor_item.y + HUD_HEIGHT);
        cell.ch = '[';
        cell.fg = tcod::ColorRGB{180, 220, 200};
      }

      for (const auto& monster : level.monsters) {
        if (!level.map.is_in_fov(monster.x, monster.y)) continue;
        auto& cell = console.at(monster.x, monster.y + HUD_HEIGHT);
        cell.ch = monster.glyph;
        cell.fg = monster.color;
      }

      // Spells currently in flight (only visible ones matter, same as monsters/items).
      for (const auto& proj : level.projectiles) {
        if (proj.path_index == 0 || proj.path_index > proj.path.size()) continue;
        auto [px, py] = proj.path[proj.path_index - 1];
        if (!level.map.is_in_fov(px, py)) continue;
        auto& cell = console.at(px, py + HUD_HEIGHT);
        cell.ch = proj.glyph;
        cell.fg = proj.color;
      }

      console.at(player.x, player.y + HUD_HEIGHT).ch = player.glyph;
      console.at(player.x, player.y + HUD_HEIGHT).fg = player.color;

      if (mode == Mode::Targeting) {
        // Preview the shot: trace the same path a cast would take, and stop drawing at
        // the first tile that would actually stop it, so what you see is what you'd hit.
        auto preview = trace_path(player.x, player.y, target_x, target_y);
        for (size_t i = 0; i < preview.size(); ++i) {
          auto [x, y] = preview[i];
          bool blocked = !level.map.is_walkable(x, y);
          bool has_monster = std::any_of(level.monsters.begin(), level.monsters.end(),
                                          [&](const Actor& m) { return m.x == x && m.y == y; });
          bool stops_here = blocked || has_monster || i + 1 == preview.size();
          auto& cell = console.at(x, y + HUD_HEIGHT);
          cell.ch = stops_here ? 'X' : '*';
          cell.fg = stops_here ? tcod::ColorRGB{255, 60, 60} : tcod::ColorRGB{150, 60, 60};
          if (blocked || has_monster) break;
        }
      }
    }

    context.present(console);

    // --- Input / events ---
    SDL_Event event;
    SDL_WaitEvent(nullptr);  // Sleep until an event arrives (this is a turn-based game; no need to busy-loop).
    while (SDL_PollEvent(&event)) {
      context.convert_event_coordinates(event);

      if (event.type == SDL_EVENT_QUIT) {
        running = false;
        continue;
      }
      if (event.type != SDL_EVENT_KEY_DOWN) continue;

      // Re-fetched fresh for every event (not reused from the outer render-time `level`
      // above): descend() can push_back onto `levels`, which may reallocate and would
      // dangle a reference held across more than one queued event in the same batch.
      Level& level = levels[static_cast<size_t>(current_level)];

      if (mode == Mode::Dead) {
        if (event.key.key == SDLK_ESCAPE) {
          running = false;
        } else {
          start_new_game();
        }
        continue;
      }

      if (mode == Mode::MessageLog) {
        if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_RIGHTBRACKET) {
          mode = Mode::Playing;
        } else if (event.key.key == SDLK_K || event.key.key == SDLK_UP) {
          int visible_rows = SCREEN_HEIGHT - 1;
          int max_scroll = std::max(0, static_cast<int>(message_log.size()) - visible_rows);
          log_scroll = std::min(log_scroll + 1, max_scroll);
        } else if (event.key.key == SDLK_J || event.key.key == SDLK_DOWN) {
          log_scroll = std::max(log_scroll - 1, 0);
        }
        continue;
      }

      if (mode == Mode::LevelUp) {
        // No menu for this on purpose: just force S/D/I directly, one point at a time.
        // Requires actual Shift+S/D/I (not the bare lowercase letter) since 'd' and 'i'
        // already mean something in normal play — a permanent stat point shouldn't be
        // one stray unshifted keypress away from being spent on the wrong thing.
        bool shift_held = (event.key.mod & SDL_KMOD_SHIFT) != 0;
        if (event.key.key == SDLK_ESCAPE) {
          running = false;
        } else if (shift_held && event.key.key == SDLK_S) {
          player.strength += 1;
          int new_max_hp = max_hp_for_strength(player.strength);
          player.hp += new_max_hp - player.max_hp;
          player.max_hp = new_max_hp;
          add_message("Strength increased to " + std::to_string(player.strength) + "!");
          pending_attribute_points -= 1;
        } else if (shift_held && event.key.key == SDLK_D) {
          player.dexterity += 1;
          player.evasion = evasion_for_dexterity(player.dexterity);
          add_message("Dexterity increased to " + std::to_string(player.dexterity) + "!");
          pending_attribute_points -= 1;
        } else if (shift_held && event.key.key == SDLK_I) {
          auto known_before = known_spell_indices(player.intelligence);
          player.intelligence += 1;
          auto known_after = known_spell_indices(player.intelligence);
          add_message("Intelligence increased to " + std::to_string(player.intelligence) + "!");
          for (int spell_idx : known_after) {
            bool already_known = std::find(known_before.begin(), known_before.end(), spell_idx) != known_before.end();
            if (!already_known) add_message("You can now cast " + kSpellTable[static_cast<size_t>(spell_idx)].name + "!");
          }
          pending_attribute_points -= 1;
        }
        if (pending_attribute_points <= 0) mode = Mode::Playing;
        continue;
      }

      if (mode == Mode::WeaponMenu) {
        if (event.key.key == SDLK_ESCAPE) {
          mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          // Slot 'a' is always fists; carried weapons fill 'b' onward.
          Weapon chosen;
          bool valid = false;
          if (idx == 0) {
            chosen = kFists;
            valid = true;
          } else if (idx - 1 < inventory.size()) {
            chosen = inventory[idx - 1];
            inventory.erase(inventory.begin() + static_cast<long>(idx - 1));
            valid = true;
          }
          if (valid) {
            // Swap the old weapon back into the pack, unless it's an intrinsic one
            // like bare fists, which isn't a real item.
            if (!player.weapon.is_intrinsic) inventory.push_back(player.weapon);
            player.weapon = chosen;
            add_message("You equip the " + chosen.name + ".");
            mode = Mode::Playing;
            end_turn();  // fiddling with gear takes time; adjacent monsters get a free hit
          }
        }
        continue;
      }

      if (mode == Mode::ArmorMenu) {
        if (event.key.key == SDLK_ESCAPE) {
          mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          // Slot 'a' is always "Nothing"; carried armor fills 'b' onward.
          Armor chosen;
          bool valid = false;
          if (idx == 0) {
            chosen = kNoArmor;
            valid = true;
          } else if (idx - 1 < armor_inventory.size()) {
            chosen = armor_inventory[idx - 1];
            armor_inventory.erase(armor_inventory.begin() + static_cast<long>(idx - 1));
            valid = true;
          }
          if (valid) {
            if (!player.armor.is_intrinsic) armor_inventory.push_back(player.armor);
            player.armor = chosen;
            add_message("You equip the " + chosen.name + ".");
            mode = Mode::Playing;
            end_turn();  // fiddling with gear takes time; adjacent monsters get a free hit
          }
        }
        continue;
      }

      if (mode == Mode::SpellMenu) {
        if (event.key.key == SDLK_ESCAPE) {
          mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          auto known = known_spell_indices(player.intelligence);
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          if (idx < known.size()) {
            casting_spell_index = known[idx];
            target_x = player.x;
            target_y = player.y;
            mode = Mode::Targeting;
          }
        }
        continue;
      }

      if (mode == Mode::Targeting) {
        const Spell& spell = kSpellTable[static_cast<size_t>(casting_spell_index)];

        if (event.key.key == SDLK_ESCAPE) {
          mode = Mode::Playing;
          continue;
        }
        if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
          // Any tile is a legal target now: the spell travels and resolves against
          // whatever (if anything) it actually reaches, not necessarily the cursor tile.
          Projectile proj;
          proj.path = trace_path(player.x, player.y, target_x, target_y);
          proj.speed = spell.speed;
          proj.dice_count = spell.dice_count;
          proj.dice_sides = spell.dice_sides;
          proj.bonus = player.intelligence / 3;  // locked in now, not re-read when it lands
          proj.name = spell.name;
          proj.glyph = spell.glyph;
          proj.color = spell.color;
          level.projectiles.push_back(proj);

          add_message("You cast " + spell.name + ".");
          mode = Mode::Playing;
          end_turn();  // advance_projectiles() may resolve this immediately for fast spells
          continue;
        }

        // Movement keys move the targeting cursor instead of the player.
        int tdx = 0;
        int tdy = 0;
        switch (event.key.key) {
          case SDLK_UP:
          case SDLK_K:
            tdy = -1;
            break;
          case SDLK_DOWN:
          case SDLK_J:
            tdy = 1;
            break;
          case SDLK_LEFT:
          case SDLK_H:
            tdx = -1;
            break;
          case SDLK_RIGHT:
          case SDLK_L:
            tdx = 1;
            break;
          case SDLK_Y:
            tdx = -1;
            tdy = -1;
            break;
          case SDLK_U:
            tdx = 1;
            tdy = -1;
            break;
          case SDLK_B:
            tdx = -1;
            tdy = 1;
            break;
          case SDLK_N:
            tdx = 1;
            tdy = 1;
            break;
          default:
            break;
        }
        if (tdx != 0 || tdy != 0) {
          int nx = target_x + tdx;
          int ny = target_y + tdy;
          int rdx = nx - player.x;
          int rdy = ny - player.y;
          bool in_range = rdx * rdx + rdy * rdy <= spell.range * spell.range;
          if (level.map.in_bounds(nx, ny) && in_range) {
            target_x = nx;
            target_y = ny;
          }
        }
        continue;
      }

      if (mode == Mode::Drop) {
        if (event.key.key == SDLK_ESCAPE) {
          mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          auto slots = drop_slots(player, inventory, armor_inventory);
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          if (idx < slots.size()) {
            const ItemSlot& slot = slots[idx];
            std::string dropped_name;
            if (slot.kind == ItemKind::Weapon) {
              Weapon dropped;
              if (slot.index == -1) {
                dropped = player.weapon;
                player.weapon = kFists;
              } else {
                size_t inv_idx = static_cast<size_t>(slot.index);
                dropped = inventory[inv_idx];
                inventory.erase(inventory.begin() + static_cast<long>(inv_idx));
              }
              level.items.push_back(GroundItem{player.x, player.y, dropped});
              dropped_name = dropped.name;
            } else {
              Armor dropped;
              if (slot.index == -1) {
                dropped = player.armor;
                player.armor = kNoArmor;
              } else {
                size_t inv_idx = static_cast<size_t>(slot.index);
                dropped = armor_inventory[inv_idx];
                armor_inventory.erase(armor_inventory.begin() + static_cast<long>(inv_idx));
              }
              level.armor_items.push_back(GroundArmor{player.x, player.y, dropped});
              dropped_name = dropped.name;
            }
            add_message("You drop the " + dropped_name + ".");
            mode = Mode::Playing;
            end_turn();
          }
        }
        continue;
      }

      // Mode::Playing
      if (event.key.key == SDLK_ESCAPE) {
        running = false;
        continue;
      }

      if (event.key.key == SDLK_W) {
        mode = Mode::WeaponMenu;
        continue;
      }
      if (event.key.key == SDLK_A) {
        mode = Mode::ArmorMenu;
        continue;
      }
      if (event.key.key == SDLK_D) {
        mode = Mode::Drop;
        continue;
      }
      if (event.key.key == SDLK_G) {
        // Picks up whatever's on the player's current tile (no more auto-pickup on
        // step). Picks up one weapon and one armor piece if both happen to be here,
        // each as its own message rather than one combined line.
        bool picked_up_anything = false;
        for (auto it = level.items.begin(); it != level.items.end(); ++it) {
          if (it->x == player.x && it->y == player.y) {
            add_message("You pick up a " + it->weapon.name + ". Press 'w' to equip.");
            inventory.push_back(it->weapon);
            level.items.erase(it);
            picked_up_anything = true;
            break;
          }
        }
        for (auto it = level.armor_items.begin(); it != level.armor_items.end(); ++it) {
          if (it->x == player.x && it->y == player.y) {
            add_message("You pick up a " + it->armor.name + ". Press 'a' to equip.");
            armor_inventory.push_back(it->armor);
            level.armor_items.erase(it);
            picked_up_anything = true;
            break;
          }
        }
        if (picked_up_anything) {
          end_turn();
        } else {
          add_message("There's nothing here to pick up.");
        }
        continue;
      }
      if (event.key.key == SDLK_Z) {
        mode = Mode::SpellMenu;
        continue;
      }
      if (event.key.key == SDLK_RIGHTBRACKET) {
        mode = Mode::MessageLog;
        log_scroll = 0;  // always open showing the most recent messages
        continue;
      }
      // SDL reports keycodes for the *unshifted* key on a US layout, so Shift+Period
      // arrives as SDLK_PERIOD with the shift modifier set, not SDLK_GREATER — check
      // both forms so '>' / '<' work regardless of how the layout reports it.
      bool pressed_stairs_down =
          event.key.key == SDLK_GREATER || (event.key.key == SDLK_PERIOD && (event.key.mod & SDL_KMOD_SHIFT));
      bool pressed_stairs_up =
          event.key.key == SDLK_LESS || (event.key.key == SDLK_COMMA && (event.key.mod & SDL_KMOD_SHIFT));

      if (pressed_stairs_down) {
        if (player.x == level.stairs_down_x && player.y == level.stairs_down_y) {
          descend();
        } else {
          add_message("There are no stairs down here.");
        }
        continue;
      }
      if (pressed_stairs_up) {
        if (level.has_stairs_up && player.x == level.entry_x && player.y == level.entry_y) {
          ascend();
        } else {
          add_message("There are no stairs up here.");
        }
        continue;
      }
      // Plain '.' (no shift, which is claimed above for '>') passes the turn without
      // moving or attacking — handy for watching what monsters do on their own.
      if (event.key.key == SDLK_PERIOD && !(event.key.mod & SDL_KMOD_SHIFT)) {
        add_message("You wait.");
        end_turn();
        continue;
      }

      int dx = 0;
      int dy = 0;
      switch (event.key.key) {
        case SDLK_UP:
        case SDLK_K:
          dy = -1;
          break;
        case SDLK_DOWN:
        case SDLK_J:
          dy = 1;
          break;
        case SDLK_LEFT:
        case SDLK_H:
          dx = -1;
          break;
        case SDLK_RIGHT:
        case SDLK_L:
          dx = 1;
          break;
        // Vim-style diagonals: y/u/b/n for up-left/up-right/down-left/down-right.
        case SDLK_Y:
          dx = -1;
          dy = -1;
          break;
        case SDLK_U:
          dx = 1;
          dy = -1;
          break;
        case SDLK_B:
          dx = -1;
          dy = 1;
          break;
        case SDLK_N:
          dx = 1;
          dy = 1;
          break;
        default:
          break;
      }
      if (dx == 0 && dy == 0) continue;

      int new_x = player.x + dx;
      int new_y = player.y + dy;

      int target_index = -1;
      for (size_t i = 0; i < level.monsters.size(); ++i) {
        if (level.monsters[i].x == new_x && level.monsters[i].y == new_y) {
          target_index = static_cast<int>(i);
          break;
        }
      }

      if (target_index >= 0) {
        // Bump attack: walking into a monster attacks it instead of moving.
        Actor& target = level.monsters[static_cast<size_t>(target_index)];

        if (random_int(1, 100) <= target.evasion) {
          add_message("The " + target.name + " dodges your attack!");
        } else {
          int damage = roll_damage(player.weapon) + player.strength;
          target.hp -= damage;

          if (!target.is_alive()) {
            add_message("You slay the " + target.name + " with your " + player.weapon.name + "!");
            int xp_reward = target.xp_reward;  // read before erase invalidates `target`
            level.monsters.erase(level.monsters.begin() + target_index);
            grant_xp(xp_reward);
          } else {
            add_message("You hit the " + target.name + " for " + std::to_string(damage) + ".");
          }
        }
        end_turn();  // any monster(s) still adjacent (including the one just hit) get to act
      } else if (level.map.is_walkable(new_x, new_y)) {
        player.x = new_x;
        player.y = new_y;
        level.map.update_fov(player.x, player.y, FOV_RADIUS);
        end_turn();
      }
    }
  }

  return 0;
}

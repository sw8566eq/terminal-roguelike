#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <libtcod.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "entity.hpp"
#include "map.hpp"
#include "rng.hpp"

// --- Content tables (temporary; will move into a data-driven item/monster system) ---

constexpr int NUM_MONSTERS = 5;  // per floor
constexpr int NUM_ITEMS = 4;     // per floor

// Melee weapons that can be found lying on the floor.
const std::vector<Weapon> kWeaponTable = {
    {"Dagger", 1, 4, 0},
    {"Short Sword", 1, 6, 0},
    {"Mace", 1, 8, 0},
    {"Battle Axe", 2, 6, 0},
};

struct MonsterTemplate {
  std::string name;
  char glyph;
  tcod::ColorRGB color;
  int max_hp;
  Weapon weapon;
  int xp_reward;
};

const std::vector<MonsterTemplate> kMonsterTable = {
    {"Rat", 'r', tcod::ColorRGB{150, 100, 60}, 4, Weapon{"Bite", 1, 3, 0}, /*xp_reward=*/5},
    {"Goblin", 'g', tcod::ColorRGB{80, 180, 80}, 7, Weapon{"Claws", 1, 4, 0}, /*xp_reward=*/10},
};

// Max HP scales with Strength, so there's no need for a separate Vitality stat.
int max_hp_for_strength(int strength) { return 10 + strength * 5; }

// XP required to advance from the given level to the next one.
int xp_needed_for_level(int level) { return level * 20; }

// The player's default, always-available unarmed attack. Not a real pickup, so it's
// never added to the ground or the inventory list.
const Weapon kFists = Weapon{"Fists", 1, 2, 0, /*is_intrinsic=*/true};

// A weapon lying on the floor, waiting to be picked up.
struct GroundItem {
  int x, y;
  Weapon weapon;
};

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

// The items selectable in the "drop" screen, in display order: -1 means the currently
// equipped weapon (omitted if it's fists/intrinsic), otherwise it's an index into
// `inventory`. Kept as one function so the render and input-handling code that both
// need this list can't drift apart.
std::vector<int> droppable_slots(const Actor& player, const std::vector<Weapon>& inventory) {
  std::vector<int> slots;
  if (!player.weapon.is_intrinsic) slots.push_back(-1);
  for (size_t i = 0; i < inventory.size(); ++i) slots.push_back(static_cast<int>(i));
  return slots;
}

// One dungeon floor: its own map, monsters, and items. Levels are generated once and
// then kept around (not regenerated) so going back upstairs returns to how you left it.
struct Level {
  Map map;
  std::vector<Actor> monsters;
  std::vector<GroundItem> items;
  std::vector<RememberedMonster> remembered_monsters;  // last-seen monster sightings, may go stale
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

// Builds and populates a fresh floor.
Level generate_level(int width, int height, bool has_stairs_up) {
  Level level{Map(width, height), {}, {}, {}};
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

  for (int i = 0; i < NUM_MONSTERS; ++i) {
    auto [mx, my] = random_free_tile(level.map, occupied);
    occupied.push_back({mx, my});

    const MonsterTemplate& tmpl = kMonsterTable[random_int(0, static_cast<int>(kMonsterTable.size()) - 1)];
    Actor monster;
    monster.x = mx;
    monster.y = my;
    monster.hp = monster.max_hp = tmpl.max_hp;
    monster.glyph = tmpl.glyph;
    monster.color = tmpl.color;
    monster.name = tmpl.name;
    monster.weapon = tmpl.weapon;
    monster.xp_reward = tmpl.xp_reward;
    level.monsters.push_back(monster);
  }

  for (int i = 0; i < NUM_ITEMS; ++i) {
    auto [ix, iy] = random_free_tile(level.map, occupied);
    occupied.push_back({ix, iy});
    level.items.push_back(GroundItem{ix, iy, kWeaponTable[random_int(0, static_cast<int>(kWeaponTable.size()) - 1)]});
  }

  return level;
}

int main(int argc, char* argv[]) {
  constexpr int SCREEN_WIDTH = 80;
  constexpr int SCREEN_HEIGHT = 25;
  constexpr int HUD_HEIGHT = 2;  // top rows reserved for stats + message, not part of the map
  constexpr int MAP_WIDTH = SCREEN_WIDTH;
  constexpr int MAP_HEIGHT = SCREEN_HEIGHT - HUD_HEIGHT;
  constexpr int FOV_RADIUS = 8;

  auto console = tcod::Console{SCREEN_WIDTH, SCREEN_HEIGHT};  // Main console.

  // Configure the context.
  auto params = TCOD_ContextParams{};
  params.console = console.get();  // Derive the window size from the console size.
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
  std::string message;
  std::string death_cause;  // name of whatever last killed the player, for the death screen
  int pending_attribute_points = 0;  // unspent level-up points forcing a Mode::LevelUp prompt

  enum class Mode { Playing, Inventory, Drop, Dead, LevelUp };
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

  // Runs after the player's turn: every living monster still adjacent to the player
  // gets to attack. (Movement/chasing AI will plug into this same turn boundary later.)
  auto end_turn = [&]() {
    Level& level = levels[static_cast<size_t>(current_level)];
    for (auto& monster : level.monsters) {
      if (mode == Mode::Dead) break;  // player already died to an earlier monster this turn
      if (!monster.is_alive()) continue;

      int dx = monster.x - player.x;
      int dy = monster.y - player.y;
      if (dx < 0) dx = -dx;
      if (dy < 0) dy = -dy;
      bool adjacent = dx <= 1 && dy <= 1 && (dx != 0 || dy != 0);
      if (!adjacent) continue;

      int damage = roll_damage(monster.weapon);
      player.hp -= damage;
      message += " The " + monster.name + " hits you for " + std::to_string(damage) + ".";
      if (!player.is_alive()) {
        death_cause = monster.name;
        mode = Mode::Dead;
      }
    }
  };

  // (Re)generates the dungeon and populates it, for both the initial game and every
  // restart after death.
  auto start_new_game = [&]() {
    levels.clear();
    levels.push_back(generate_level(MAP_WIDTH, MAP_HEIGHT, /*has_stairs_up=*/false));
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
    player.weapon = kFists;
    level.map.update_fov(player.x, player.y, FOV_RADIUS);

    inventory.clear();
    pending_attribute_points = 0;
    message = "Walk into an enemy to attack. hjkl/yubn or arrows to move, '>'/'<' for stairs, 'i' inventory, 'd' drop, Esc to quit.";
    mode = Mode::Playing;
  };

  // Goes down the stairs the player is currently standing on, generating the floor
  // below the first time it's visited.
  auto descend = [&]() {
    current_level += 1;
    if (static_cast<size_t>(current_level) >= levels.size()) {
      levels.push_back(generate_level(MAP_WIDTH, MAP_HEIGHT, /*has_stairs_up=*/true));
    }
    Level& level = levels[static_cast<size_t>(current_level)];
    player.x = level.entry_x;
    player.y = level.entry_y;
    level.map.update_fov(player.x, player.y, FOV_RADIUS);
    message = "You descend the stairs.";
  };

  // Goes back up to the floor above, landing on the stairs down that was taken from it.
  auto ascend = [&]() {
    current_level -= 1;
    Level& level = levels[static_cast<size_t>(current_level)];
    player.x = level.stairs_down_x;
    player.y = level.stairs_down_y;
    level.map.update_fov(player.x, player.y, FOV_RADIUS);
    message = "You ascend the stairs.";
  };

  start_new_game();

  bool running = true;

  while (running) {
    Level& level = levels[static_cast<size_t>(current_level)];

    // --- Render ---
    console.clear();

    if (mode == Mode::Inventory) {
      tcod::print(console, {0, 0}, "Inventory - press a letter to equip, Esc to close",
                  tcod::ColorRGB{255, 255, 255}, std::nullopt);
      tcod::print(console, {0, 1}, "Equipped: " + player.weapon.name + " (" + describe_weapon(player.weapon) + ")",
                  tcod::ColorRGB{200, 200, 100}, std::nullopt);

      // Fists are always slot 'a', so you can always bail back to unarmed; carried
      // items fill 'b' onward.
      std::string fists_line = "a) Fists (" + describe_weapon(kFists) + ")";
      if (player.weapon.is_intrinsic) fists_line += " [equipped]";
      tcod::print(console, {0, 3}, fists_line, tcod::ColorRGB{200, 200, 200}, std::nullopt);

      for (size_t i = 0; i < inventory.size(); ++i) {
        std::string line = std::string(1, static_cast<char>('b' + i)) + ") " + inventory[i].name + " (" +
                            describe_weapon(inventory[i]) + ")";
        tcod::print(console, {0, 4 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
      }
    } else if (mode == Mode::Drop) {
      tcod::print(console, {0, 0}, "Drop - press a letter to drop, Esc to cancel", tcod::ColorRGB{255, 255, 255},
                  std::nullopt);

      auto slots = droppable_slots(player, inventory);
      if (slots.empty()) {
        tcod::print(console, {0, 2}, "(nothing to drop)", tcod::ColorRGB{120, 120, 120}, std::nullopt);
      }
      for (size_t i = 0; i < slots.size(); ++i) {
        const Weapon& w = (slots[i] == -1) ? player.weapon : inventory[static_cast<size_t>(slots[i])];
        std::string line = std::string(1, static_cast<char>('a' + i)) + ") " + w.name + " (" + describe_weapon(w) + ")";
        if (slots[i] == -1) line += " [equipped]";
        tcod::print(console, {0, 2 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
      }
    } else if (mode == Mode::Dead) {
      tcod::print(console, {0, 0}, "You died, slain by the " + death_cause + ".", tcod::ColorRGB{255, 80, 80},
                  std::nullopt);
      tcod::print(console, {0, 2}, "Press any key to start a new game, or Esc to quit.", tcod::ColorRGB{200, 200, 200},
                  std::nullopt);
    } else {
      update_monster_memory(level);

      // Row 0: persistent stats. Row 1: the rolling event message (or the level-up
      // prompt) — kept on its own line so a long stats prefix can't crowd it out.
      std::string stats_line = "HP:" + std::to_string(player.hp) + "/" + std::to_string(player.max_hp) + " Lvl:" +
                                std::to_string(player.level) + " STR:" + std::to_string(player.strength) +
                                " DEX:" + std::to_string(player.dexterity) + " INT:" +
                                std::to_string(player.intelligence) + " Floor:" + std::to_string(current_level + 1) +
                                " Wpn:" + player.weapon.name + "(" + describe_weapon(player.weapon) + ")";
      tcod::print(console, {0, 0}, stats_line, tcod::ColorRGB{255, 255, 255}, std::nullopt);

      std::string message_line = message;
      if (mode == Mode::LevelUp) {
        message_line = "*** LEVEL UP (now level " + std::to_string(player.level) +
                        ")! Press Shift+S/D/I to raise Strength/Dexterity/Intelligence. ***";
      }
      tcod::print(console, {0, 1}, message_line, tcod::ColorRGB{255, 255, 100}, std::nullopt);

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

      for (const auto& monster : level.monsters) {
        if (!level.map.is_in_fov(monster.x, monster.y)) continue;
        auto& cell = console.at(monster.x, monster.y + HUD_HEIGHT);
        cell.ch = monster.glyph;
        cell.fg = monster.color;
      }

      console.at(player.x, player.y + HUD_HEIGHT).ch = player.glyph;
      console.at(player.x, player.y + HUD_HEIGHT).fg = player.color;
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
          message = "Strength increased to " + std::to_string(player.strength) + "!";
          pending_attribute_points -= 1;
        } else if (shift_held && event.key.key == SDLK_D) {
          player.dexterity += 1;
          message = "Dexterity increased to " + std::to_string(player.dexterity) + "!";
          pending_attribute_points -= 1;
        } else if (shift_held && event.key.key == SDLK_I) {
          player.intelligence += 1;
          message = "Intelligence increased to " + std::to_string(player.intelligence) + "!";
          pending_attribute_points -= 1;
        }
        if (pending_attribute_points <= 0) mode = Mode::Playing;
        continue;
      }

      if (mode == Mode::Inventory) {
        if (event.key.key == SDLK_ESCAPE) {
          mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          // Slot 'a' is always fists; carried items fill 'b' onward.
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
            message = "You equip the " + chosen.name + ".";
            mode = Mode::Playing;
            end_turn();  // fiddling with gear takes time; adjacent monsters get a free hit
          }
        }
        continue;
      }

      if (mode == Mode::Drop) {
        if (event.key.key == SDLK_ESCAPE) {
          mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          auto slots = droppable_slots(player, inventory);
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          if (idx < slots.size()) {
            Weapon dropped;
            if (slots[idx] == -1) {
              dropped = player.weapon;
              player.weapon = kFists;
            } else {
              size_t inv_idx = static_cast<size_t>(slots[idx]);
              dropped = inventory[inv_idx];
              inventory.erase(inventory.begin() + static_cast<long>(inv_idx));
            }
            level.items.push_back(GroundItem{player.x, player.y, dropped});
            message = "You drop the " + dropped.name + ".";
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

      if (event.key.key == SDLK_I) {
        mode = Mode::Inventory;
        continue;
      }
      if (event.key.key == SDLK_D) {
        mode = Mode::Drop;
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
          message = "There are no stairs down here.";
        }
        continue;
      }
      if (pressed_stairs_up) {
        if (level.has_stairs_up && player.x == level.entry_x && player.y == level.entry_y) {
          ascend();
        } else {
          message = "There are no stairs up here.";
        }
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
        int damage = roll_damage(player.weapon) + player.strength;
        target.hp -= damage;

        if (!target.is_alive()) {
          message = "You slay the " + target.name + " with your " + player.weapon.name + "!";
          int xp_reward = target.xp_reward;  // read before erase invalidates `target`
          level.monsters.erase(level.monsters.begin() + target_index);
          grant_xp(xp_reward);
        } else {
          message = "You hit the " + target.name + " for " + std::to_string(damage) + ".";
        }
        end_turn();  // any monster(s) still adjacent (including the one just hit) get to act
      } else if (level.map.is_walkable(new_x, new_y)) {
        player.x = new_x;
        player.y = new_y;
        level.map.update_fov(player.x, player.y, FOV_RADIUS);

        for (auto it = level.items.begin(); it != level.items.end(); ++it) {
          if (it->x == player.x && it->y == player.y) {
            message = "You pick up a " + it->weapon.name + ". Press 'i' to equip it.";
            inventory.push_back(it->weapon);
            level.items.erase(it);
            break;
          }
        }
        end_turn();
      }
    }
  }

  return 0;
}

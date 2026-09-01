#include "arena.hpp"

#include "actors.hpp"
#include "level.hpp"
#include "rules.hpp"

int monster_index_named(const std::string& name) { return index_named(kMonsterTable, name); }
int spell_index_named(const std::string& name) { return index_named(kSpellTable, name); }
int weapon_index_named(const std::string& name) { return index_named(kWeaponTable, name); }
int potion_index_named(const std::string& name) { return index_named(kPotionTable, name); }

namespace arena {

GameState make_gamestate(const std::vector<std::string>& rows, int px, int py) {
  GameState gs;

  // Same shape generate_level() itself constructs a Level with (see level.cpp) — an
  // aggregate-init listing every vector member, since Map has no default constructor.
  gs.levels.push_back(Level{Map(MAP_WIDTH, MAP_HEIGHT), {}, {}, {}, {}, {}, {}, {}});
  gs.current_level = 0;
  gs.level().map.paint_ascii(rows);
  gs.level().entry_x = px;
  gs.level().entry_y = py;

  // The same fixed starting numbers start_new_game() gives a fresh character (see
  // game.cpp) — deliberately not randomly generated, so a test's expected outcome is a
  // fact about the formula/AI being tested, not about whatever the RNG happened to do.
  Actor& player = gs.player;
  player.is_player = true;
  player.name = "you";
  player.x = px;
  player.y = py;
  player.strength = 2;
  player.dexterity = 2;
  player.intelligence = 2;
  player.level = 1;
  player.xp = 0;
  player.max_hp = max_hp_for_level_and_strength(player.level, player.strength);
  player.hp = player.max_hp;
  player.evasion = evasion_for_dexterity(player.dexterity);
  player.hp_regen_turns = kHpRegenTurns;
  player.mana_regen_turns = kManaRegenTurns;
  player.max_mana = max_mana_for_intelligence(player.intelligence);
  player.mana = player.max_mana;
  player.weapon = kFists;
  player.armor = kNoArmor;

  gs.level().map.update_fov(px, py, FOV_RADIUS);
  return gs;
}

int add_floor(GameState& gs, const std::vector<std::string>& rows, int entry_x, int entry_y, bool has_stairs_up) {
  gs.levels.push_back(Level{Map(MAP_WIDTH, MAP_HEIGHT), {}, {}, {}, {}, {}, {}, {}});
  int index = static_cast<int>(gs.levels.size()) - 1;
  gs.levels[static_cast<size_t>(index)].map.paint_ascii(rows);
  gs.levels[static_cast<size_t>(index)].has_stairs_up = has_stairs_up;
  gs.levels[static_cast<size_t>(index)].entry_x = entry_x;
  gs.levels[static_cast<size_t>(index)].entry_y = entry_y;
  return index;
}

int place_monster(GameState& gs, int table_index, int x, int y) {
  Actor monster = spawn_monster(table_index, x, y);
  int id = monster.id;
  gs.level().monsters.push_back(monster);
  return id;
}

int place_minion(GameState& gs, const MinionTemplate& tmpl, int x, int y) {
  Actor minion = spawn_minion(tmpl, x, y);
  int id = minion.id;
  gs.level().monsters.push_back(minion);
  return id;
}

Actor* find_actor(GameState& gs, int id) {
  int index = actor_index_by_id(gs.level().monsters, id);
  if (index < 0) return nullptr;
  return &gs.level().monsters[static_cast<size_t>(index)];
}

}  // namespace arena

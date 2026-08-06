#pragma once

#include <libtcod.hpp>
#include <string>

// A melee weapon: damage is dice_count dice of dice_sides sides each, plus a
// flat bonus. E.g. {"Short Sword", 1, 6, 0} is a 1d6.
struct Weapon {
  std::string name;
  int dice_count = 1;
  int dice_sides = 2;
  int bonus = 0;
  // True for weapons that aren't real pickups (e.g. bare fists, monster bite/claws) —
  // these don't get returned to the player's inventory when swapped out.
  bool is_intrinsic = false;
  // Which floors (1-indexed) this weapon can spawn on, same min/max_depth shape as
  // MonsterTemplate — irrelevant once it's actually picked up (a floor-10 weapon in
  // your pack doesn't vanish if you walk back to floor 1). max_depth -1 means no cap.
  int min_depth = 1;
  int max_depth = -1;
  // This weapon's accuracy against a monster's evasion (see monster_dodge_chance() in
  // main.cpp) — rolled and subtracted from the target's evasion, so a bigger hit-dice
  // roll makes the attack harder to dodge. Only meaningful for the player's own
  // weapon; monster weapons (Bite, Claws, ...) don't use this — monster attacks
  // against the player go through the unrelated dodge_chance_vs() Dexterity contest —
  // so they're left at these defaults rather than given real values. Trailing fields
  // with defaults so existing positional literals (monster weapons, kFists) that don't
  // mention them keep compiling unchanged.
  int hit_dice_count = 1;
  int hit_dice_sides = 4;
};

// Rolls this weapon's damage for one attack.
int roll_damage(const Weapon& weapon);

// Worn armor: a flat reduction applied to damage from any attack that actually lands
// (evasion is checked separately, before this). E.g. {"Chainmail", 3} soaks 3 damage
// per hit.
struct Armor {
  std::string name;
  int defense = 0;
  // True for armor that isn't a real pickup (bare skin) — doesn't get returned to the
  // player's inventory when swapped out.
  bool is_intrinsic = false;
  // Which floors (1-indexed) this armor can spawn on; see Weapon::min_depth above.
  int min_depth = 1;
  int max_depth = -1;
};

// Which attribute a temporary-buff potion raises. None means the potion doesn't buff a
// stat at all (e.g. Heal Potion).
enum class StatKind { None, Strength, Dexterity, Intelligence };

// A consumable potion: drinking it applies its effect immediately and uses it up —
// unlike Weapon/Armor, there's nothing to equip or swap back out. A potion is a heal
// (heal_percent > 0), a temporary stat buff (buff_stat != StatKind::None), or a
// teleport (teleports); the table never mixes more than one of these on one entry.
struct Potion {
  std::string name;
  int heal_percent = 0;  // percent of max HP restored, instantly, when drunk
  StatKind buff_stat = StatKind::None;  // which stat this potion temporarily raises
  int buff_amount = 0;                  // how much (e.g. +5)
  int buff_turns = 0;                   // how long, in turns, before it wears off
  char glyph = '!';
  tcod::ColorRGB color{255, 255, 255};
  // Which floors (1-indexed) this potion can spawn on; see Weapon::min_depth above.
  int min_depth = 1;
  int max_depth = -1;
  // Drinking it moves the player to a random walkable tile on the current floor
  // instead of healing/buffing — see the Mode::PotionMenu drink handler in main.cpp.
  bool teleports = false;
};

// A living thing on the map: the player or a monster. Combat is symmetric
// (same stats, same roll_damage call on both sides), so both share this one
// representation for now rather than separate Player/Monster types.
struct Actor {
  int x = 0;
  int y = 0;
  int hp = 1;
  int max_hp = 1;
  char glyph = '?';
  tcod::ColorRGB color{255, 255, 255};
  std::string name;
  Weapon weapon;
  Armor armor;

  // Player-progression stats; monsters set dexterity from their template (see below)
  // and leave strength/intelligence at their defaults. Strength drives both max HP and
  // melee damage (no separate Vitality stat). Dexterity drives dodge chance on both
  // sides of a fight (see dodge_chance_vs() in main.cpp) — on a monster it doubles as
  // its "accuracy": a low-Dexterity monster is easy for the player to dodge, a
  // high-Dexterity one much less so. Intelligence drives spell unlocks/damage and max
  // mana (max_mana_for_intelligence() in main.cpp, mirroring max_hp_for_strength()).
  int strength = 1;
  int dexterity = 1;
  int intelligence = 1;
  int level = 1;
  int xp = 0;

  int xp_reward = 0;  // monsters only: XP granted to the player on kill

  // Monsters only: fixed percent chance (0-100) to dodge the player's attack, set from
  // its template at spawn (monsters don't wear armor, so this is their only defense).
  // The player has no equivalent flat value — their dodge chance against an incoming
  // attack is computed live from both sides' Dexterity (dodge_chance_vs()), since it
  // depends on which monster is swinging, not just the player's own stats.
  int evasion = 0;

  // Monsters only: how many tiles away (Chebyshev distance) this monster can attack
  // from, set from its template at spawn. 1 = adjacency-only (melee), same as every
  // monster before ranged attackers existed. See MonsterTemplate::attack_range and the
  // in_range/can_attack check in end_turn() (main.cpp).
  int attack_range = 1;

  // Monsters only: optional fallback weapon+accuracy used instead of weapon/dexterity
  // once this monster is actually adjacent to the player, set from
  // MonsterTemplate::melee_weapon/melee_accuracy at spawn. Empty name (the default) =
  // no separate melee weapon, just always use weapon/dexterity as normal.
  Weapon melee_weapon;
  int melee_dexterity = 0;

  // Monsters only: one-way flip, set the first time this monster ever lands (or takes
  // its turn) adjacent to the player while it has a melee_weapon. A ranged monster
  // (e.g. Goblin Slinger) snipes from attack_range tiles away and never has to
  // approach — right up until the player actually reaches it, at which point it
  // commits to melee for good: from then on it behaves exactly like an ordinary
  // melee-only monster (chasing when not adjacent, attacking with melee_weapon when it
  // is), never going back to sniping. See the in_range/can_attack check in end_turn().
  bool melee_engaged = false;

  // Player only: fractional HP banked toward the next point of passive regen (HP/turn
  // is usually not a whole number, so this carries the remainder between turns).
  float hp_regen_accumulator = 0.0f;

  // Player only: mana, spent to cast spells (see Spell::mana_cost in main.cpp) and
  // regenerated passively the same way HP is. max_mana comes from
  // max_mana_for_intelligence(); mana_regen_accumulator is the same fractional-banking
  // trick as hp_regen_accumulator above.
  int mana = 0;
  int max_mana = 0;
  float mana_regen_accumulator = 0.0f;

  // Player only: temporary stat bonuses from stat potions (Potion of Strength/
  // Dexterity/Intelligence), and turns remaining before each reverts. Ticked down once
  // per turn in end_turn(); drinking another potion of the same stat while one is
  // already active just refreshes the timer rather than stacking the bonus. Strength's
  // bonus feeds max_hp_for_strength() and melee damage the same as the permanent stat —
  // but unlike leveling up, gaining or losing it never changes current HP, only the
  // ceiling. Intelligence's bonus feeds spell damage, but deliberately NOT
  // known_spell_indices() — only permanent, unmodified intelligence unlocks new spells.
  int temp_str_bonus = 0;
  int temp_str_turns = 0;
  int temp_dex_bonus = 0;
  int temp_dex_turns = 0;
  int temp_int_bonus = 0;
  int temp_int_turns = 0;

  // Monsters only: the last tile this monster actually saw the player standing on,
  // or (-1, -1) if it's never seen them (or already reached that tile without finding
  // them there). Lets a monster keep heading for where the player was after losing
  // line of sight, instead of immediately reverting to idle wandering — see the
  // chase/investigate/wander logic in end_turn().
  int last_seen_player_x = -1;
  int last_seen_player_y = -1;

  bool is_alive() const { return hp > 0; }
};

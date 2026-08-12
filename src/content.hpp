#pragma once

// Every item, monster and minion the game can produce, plus the depth filter that
// decides what a given floor is allowed to draw from.
//
// This file is all *data* — no game state, no logic beyond filtering the tables. Adding
// a new weapon, monster or summon should be a new row in one of these tables and nothing
// else; if a change needs more than that, the mechanic it needs probably belongs on
// Actor (see entity.hpp) so everyone gets it, not on a template.

#include <libtcod.hpp>

#include <string>
#include <vector>

#include "entity.hpp"

// How many of each thing generate_level() scatters on a floor. Monster count is a
// baseline that grows with depth (see monster_count_for_depth() in level.hpp).
constexpr int NUM_MONSTERS = 5;
constexpr int NUM_ITEMS = 4;
constexpr int NUM_ARMOR = 3;
constexpr int NUM_POTIONS = 2;

// How many minions the player can have active at once, checked when casting a summon
// spell. Deliberately conservative to start (roadmap targets a 1-7 range once the
// pack-order UI has actually been played) — raising this later is a one-constant
// change, not a redesign, since nothing else here assumes a specific pack size.
constexpr int kMaxMinions = 3;

// The default, always-available unarmed attack. Not a real pickup, so it's never added
// to the ground or to an inventory — an Actor whose weapon is_intrinsic simply has
// nothing to drop from that slot (see drop_actor_gear()). Monsters' natural weapons
// (Bite, Claws, ...) are marked intrinsic for exactly the same reason.
extern const Weapon kFists;

// The default, always-available "armor" (bare skin, no defense). Not a real pickup,
// same idea as kFists — and the default every monster wears unless its table row says
// otherwise.
extern const Armor kNoArmor;

// Weapons, armor and potions that can be found lying on the floor, all depth-gated
// through the same min_depth/max_depth shape (see available_at_depth() below).
extern const std::vector<Weapon> kWeaponTable;
extern const std::vector<Armor> kArmorTable;
extern const std::vector<Potion> kPotionTable;

// One monster species. Every field here maps onto a plain Actor field of the same name
// — there is no monster-specific mechanic left, only monster-specific *numbers*. A row
// is just "an Actor, pre-filled": spawn_monster() copies it across and the result
// fights, regenerates, swaps gear, and drinks potions through the exact same code the
// player does.
struct MonsterTemplate {
  std::string name;
  char glyph;
  tcod::ColorRGB color;
  int max_hp;     // authored rather than derived from strength — see the Actor comment
  Weapon weapon;  // starting equipped weapon; its attack_range is this monster's reach
  int xp_reward;
  int evasion;    // authored dodge rating (the player's comes from evasion_for_dexterity)
  int dexterity;  // accuracy this monster brings to every swing, same stat the player uses
  int strength;   // flat melee damage on top of the weapon's dice, same as the player's
  int min_depth;  // first floor (1-indexed) this monster can spawn on
  int max_depth;  // last floor it can spawn on; -1 means no upper limit
  // Carried gear, all optional and all trailing-with-defaults so a plain monster's row
  // just omits them. armor is worn from spawn (flat damage reduction against anything
  // that hits it, the same Armor the player wears); extra_weapons are alternatives it
  // switches between by range (see equip_best_weapon_for_range() — this is what replaced
  // the old bespoke melee_weapon/melee_accuracy pair). Anything non-intrinsic here drops
  // as loot when the monster dies.
  //
  // `potions` is empty on every row but the Orc Warlord's: monsters drinking their own
  // potions worked, but handing them out widely flooded the player with consumable
  // drops. The machinery is intact and unconditional (try_actor_use_potion() runs for
  // every monster and minion, through the same apply_potion() the player's q menu
  // calls), so re-enabling it for a species is just putting a kPotionTable entry back in
  // its row — but weigh the loot economy first, since a carried potion the monster never
  // drinks becomes floor loot.
  Armor armor = kNoArmor;
  std::vector<Weapon> extra_weapons = {};
  std::vector<Potion> potions = {};
  // Turns to regenerate from 0 HP to full, or 0 for a monster that doesn't heal at all
  // — see Actor::hp_regen_turns. Non-zero only on the Orc Warlord today; it's the knob
  // for boss/elite rows that should shrug off chip damage.
  int hp_regen_turns = 0;
  // Extra actions per world turn beyond the first — 0 on every row today, the second
  // boss/elite knob alongside hp_regen_turns. A row with extra_actions=1 moves/attacks
  // twice for each of the player's turns. See Actor::extra_actions.
  int extra_actions = 0;
  // A boss spawns exactly once on every floor its min_depth/max_depth range covers,
  // instead of being drawn at random like an ordinary monster — see bosses_at_depth()
  // and generate_level(). Boss rows are excluded from the ordinary random pool
  // (monsters_available_at_depth()), so a floor gets exactly one, never zero and never
  // five. Nothing else about a boss is special: it's an Actor with bigger numbers and
  // whatever combination of hp_regen_turns/extra_actions/gear its row asks for.
  bool is_boss = false;
  // Spellcasting, for a monster whose row wants it (the Goblin Shaman). All four are
  // trailing rather than sitting next to strength/dexterity where `intelligence` really
  // belongs, because every row in this table is positional aggregate init and these are
  // all ints — inserting mid-struct would silently misalign nine existing rows rather
  // than fail to compile.
  //   intelligence     -> spell damage bonus (INT/3), exactly as the player's does
  //   max_mana         -> authored, not derived via max_mana_for_intelligence() — same
  //                       authored-vs-derived split as max_hp and evasion
  //   mana_regen_turns -> 0 means the pool is a one-time budget (see Actor)
  //   spell_index      -> row into kSpellTable, -1 for a non-caster
  int intelligence = 0;
  int max_mana = 0;
  int mana_regen_turns = 0;
  int spell_index = -1;
};

extern const std::vector<MonsterTemplate> kMonsterTable;

// A player-summoned minion (Allegiance::Player) — same shape as MonsterTemplate where
// they overlap (name/glyph/color/max_hp/weapon/evasion/dexterity/strength, plus the same
// optional armor/extra_weapons/potions), but a deliberately separate table: which row is
// available is gated by which summon spell unlocked it (Spell::summon_template_index),
// not by depth, so it doesn't share kMonsterTable's min_depth/max_depth/
// available_at_depth() machinery. No xp_reward either — the player doesn't earn XP for
// a minion dying.
struct MinionTemplate {
  std::string name;
  char glyph;
  tcod::ColorRGB color;
  int max_hp;
  Weapon weapon;
  int evasion;
  int dexterity;  // accuracy, exactly as on MonsterTemplate and the player
  int strength;   // flat melee damage, exactly as on MonsterTemplate and the player
  // Turns until this minion expires on its own, or -1 for permanent (only dies in
  // combat). See Actor::duration_turns.
  int duration_turns = -1;
  // Same optional carried gear as MonsterTemplate — a minion is an Actor too, so it
  // wears armor, switches weapons by range, and drinks potions through the same code.
  Armor armor = kNoArmor;
  std::vector<Weapon> extra_weapons = {};
  std::vector<Potion> potions = {};
  // Same regeneration toggle as MonsterTemplate — 0 (today's only row) means a minion
  // doesn't heal either. See Actor::hp_regen_turns.
  int hp_regen_turns = 0;
  // Same extra-actions knob as MonsterTemplate, 0 on every row today. A summon fast
  // enough to act twice a turn would just set this. See Actor::extra_actions.
  int extra_actions = 0;
  // Abilities the *player* can order this minion to use, as indices into kSpellTable
  // (see Actor::abilities). Distinct from MonsterTemplate::spell_index, which is what a
  // monster's own AI throws unprompted — these are fired deliberately, by focusing the
  // minion and pressing 'z'. A row that leaves this empty simply has no 'z' menu.
  std::vector<int> abilities = {};
  // The pool those abilities spend, authored exactly like MonsterTemplate's — same
  // derived-vs-authored split as max_hp. mana_regen_turns 0 means the pool is a one-time
  // budget for the minion's whole life (the Goblin Shaman's shape), so a summon's
  // abilities are a resource you spend rather than something to lean on every turn.
  int max_mana = 0;
  int mana_regen_turns = 0;
};

extern const std::vector<MinionTemplate> kMinionTable;

// Indices into `table` of every entry whose min_depth/max_depth range includes `depth`
// (1-indexed; max_depth < 0 means no upper limit). Shared by monsters, weapons, armor,
// and potions — every one of those tables uses this identical min/max_depth shape, so
// the spawn logic for any of them can't drift from what its table actually says.
template <typename T>
std::vector<int> available_at_depth(const std::vector<T>& table, int depth) {
  std::vector<int> indices;
  for (size_t i = 0; i < table.size(); ++i) {
    const T& entry = table[i];
    bool below_max = entry.max_depth < 0 || depth <= entry.max_depth;
    if (depth >= entry.min_depth && below_max) indices.push_back(static_cast<int>(i));
  }
  return indices;
}

// The ordinary random spawn pool for a floor: everything available_at_depth() allows,
// minus boss rows. A boss is placed separately and exactly once (see bosses_at_depth()),
// so leaving it in here too would let a floor roll several of them — or none.
std::vector<int> monsters_available_at_depth(int depth);

// The mirror image: only the boss rows whose depth range covers this floor. Same
// min_depth/max_depth gating every other table uses — a boss that should appear on
// exactly one floor just sets both to the same number.
std::vector<int> bosses_at_depth(int depth);

std::vector<int> weapons_available_at_depth(int depth);
std::vector<int> armor_available_at_depth(int depth);
std::vector<int> potions_available_at_depth(int depth);

// Short "what does this do" strings for the HUD and menus, e.g. "+50% HP",
// "+5 STR (15 turns)", "1d6, range 8", "+3".
std::string describe_potion(const Potion& potion);
std::string describe_weapon(const Weapon& weapon);
std::string describe_armor(const Armor& armor);

// Debug convenience for the --give= startup flag: looks up `name` across kWeaponTable,
// kArmorTable and kPotionTable (first exact match wins — no name collides across the
// three today) and adds a copy to `actor`'s carried inventory, unequipped. Returns false
// if nothing matched, so the caller can say so out loud rather than silently no-opping
// the way the other debug flags do.
bool give_starting_item(const std::string& name, Actor& actor);

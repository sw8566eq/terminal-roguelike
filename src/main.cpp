#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <libtcod.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "entity.hpp"
#include "map.hpp"
#include "rng.hpp"

// --- Content tables (temporary; will move into a data-driven item/monster system) ---

constexpr int NUM_MONSTERS = 5;  // per floor
constexpr int NUM_ITEMS = 4;     // per floor
constexpr int NUM_ARMOR = 3;     // per floor
constexpr int NUM_POTIONS = 2;   // per floor

// Melee weapons that can be found lying on the floor. Depth-gated the same shape as
// kMonsterTable: rough tiers, not a strict per-floor curve. Placeholder ranges — the
// actual balance (exactly which floor a Mace should start showing up on, etc.) is a
// follow-up pass; this just makes sure a Dagger stops being possible loot on floor 10.
//
// hit_dice trails off as the weapons get heavier: a fast/light Dagger is hardest to
// dodge, a massive Battle Axe easiest — a tradeoff against the heavier weapons' bigger
// damage dice, not a strict upgrade path (see monster_dodge_chance()).
const std::vector<Weapon> kWeaponTable = {
    {"Dagger", 1, 4, 0, /*is_intrinsic=*/false, /*min_depth=*/1, /*max_depth=*/3,
     /*hit_dice_count=*/3, /*hit_dice_sides=*/4},
    {"Short Sword", 1, 6, 0, /*is_intrinsic=*/false, /*min_depth=*/1, /*max_depth=*/6,
     /*hit_dice_count=*/2, /*hit_dice_sides=*/4},
    {"Mace", 1, 8, 0, /*is_intrinsic=*/false, /*min_depth=*/3, /*max_depth=*/-1,
     /*hit_dice_count=*/1, /*hit_dice_sides=*/4},
    {"Battle Axe", 2, 6, 0, /*is_intrinsic=*/false, /*min_depth=*/5, /*max_depth=*/-1,
     /*hit_dice_count=*/1, /*hit_dice_sides=*/3},
    // The player's first ranged weapon (see Weapon::attack_range) — fired via
    // Mode::RangedAttack ('f'), not bump-to-attack. Deliberately modest dice (matching
    // Short Sword) since, unlike melee, its damage AND accuracy also get a flat
    // Dexterity-derived bonus on top (see the 'f' handler) — plus the range and safety
    // of not needing to close to melee. Ammo is unlimited for now; flagged as a
    // balance question to revisit once it's actually been played with.
    {"Bow", 1, 6, 0, /*is_intrinsic=*/false, /*min_depth=*/1, /*max_depth=*/-1,
     /*hit_dice_count=*/2, /*hit_dice_sides=*/4, /*attack_range=*/8},
};

// Armor that can be found lying on the floor. Same depth-gating shape as kWeaponTable.
const std::vector<Armor> kArmorTable = {
    {"Leather Armor", 1, /*is_intrinsic=*/false, /*min_depth=*/1, /*max_depth=*/5},
    {"Chainmail", 3, /*is_intrinsic=*/false, /*min_depth=*/3, /*max_depth=*/-1},
    {"Plate Armor", 5, /*is_intrinsic=*/false, /*min_depth=*/6, /*max_depth=*/-1},
};

// Potions that can be found lying on the floor. Stat potions all use the same +5/15-turn
// shape for now; only Strength has a described mechanical effect today (max HP, regen,
// melee damage) — Dexterity (evasion) and Intelligence (spell damage) piggyback on the
// same existing formulas that already read those stats. All are left ungated
// (default min_depth=1, max_depth=-1, same as Weapon/Armor) since there's no real tiering
// rationale among them yet — unlike kWeaponTable/kArmorTable, this table doesn't
// exercise the depth-filter below, but the fields are there once it needs to.
const std::vector<Potion> kPotionTable = {
    {"Heal Potion", /*heal_percent=*/50, StatKind::None, 0, 0, '!', tcod::ColorRGB{255, 100, 150}},
    {"Potion of Strength", 0, StatKind::Strength, /*buff_amount=*/5, /*buff_turns=*/15, '!',
     tcod::ColorRGB{200, 60, 60}},
    {"Potion of Dexterity", 0, StatKind::Dexterity, /*buff_amount=*/5, /*buff_turns=*/15, '!',
     tcod::ColorRGB{60, 200, 120}},
    {"Potion of Intelligence", 0, StatKind::Intelligence, /*buff_amount=*/5, /*buff_turns=*/15, '!',
     tcod::ColorRGB{80, 120, 220}},
    {"Potion of Teleportation", 0, StatKind::None, 0, 0, '!', tcod::ColorRGB{180, 80, 220}, /*min_depth=*/1,
     /*max_depth=*/-1, /*teleports=*/true},
};

// Formats a potion as e.g. "+50% HP" or "+5 STR (15 turns)", for the HUD/menus.
std::string describe_potion(const Potion& potion) {
  if (potion.teleports) return "Random teleport";
  if (potion.heal_percent > 0) return "+" + std::to_string(potion.heal_percent) + "% HP";
  const char* stat_name = potion.buff_stat == StatKind::Strength
                               ? "STR"
                               : potion.buff_stat == StatKind::Dexterity ? "DEX" : "INT";
  return "+" + std::to_string(potion.buff_amount) + " " + stat_name + " (" + std::to_string(potion.buff_turns) +
         " turns)";
}

struct MonsterTemplate {
  std::string name;
  char glyph;
  tcod::ColorRGB color;
  int max_hp;
  Weapon weapon;
  int xp_reward;
  int evasion;    // percent chance to dodge the player's attack; monsters wear no armor
  int accuracy;   // Dexterity-equivalent used against the player's own dodge chance (see dodge_chance_vs)
  int min_depth;  // first floor (1-indexed) this monster can spawn on
  int max_depth;  // last floor it can spawn on; -1 means no upper limit
  // How many tiles away (Chebyshev distance) this monster can attack from — see the
  // in_range/can_attack check in end_turn(). 1 means adjacency-only (melee), same as
  // every monster before this field existed; a bigger value plus a real weapon lets a
  // monster attack from a distance (e.g. Goblin Slinger) using the exact same
  // dodge_chance_vs()/roll_damage() combat code melee already uses, just from farther
  // away — still blocked by walls (line_clear()).
  int attack_range;
  // Optional fallback weapon+accuracy used instead of weapon/accuracy above whenever
  // this monster is actually adjacent to the player, regardless of attack_range — e.g.
  // Goblin Slinger closes to a more-accurate Dagger instead of its Rock once you're
  // right next to it. Left at these defaults (empty name = "no separate melee
  // weapon") for every monster that doesn't need the distinction, which just always
  // uses weapon/accuracy the way every monster did before this field existed.
  Weapon melee_weapon = Weapon{};
  int melee_accuracy = 0;
};

// Roughly increasing toughness/reward with min_depth, so descending gets harder. Each
// "tier" fully replaces the previous one at its cutover rather than overlapping: Rat
// and Goblin stop past floor 4, and Skeleton/Orc pick up as the new baseline exactly
// where they leave off, same idea Troll will hand off to whatever comes after it.
// `accuracy` climbs with the same tiers as everything else, so a weak early monster is
// genuinely easy to dodge (especially for a Dexterity-buffed player) and a deep one is
// genuinely hard to.
const std::vector<MonsterTemplate> kMonsterTable = {
    {"Rat", 'r', tcod::ColorRGB{150, 100, 60}, 4, Weapon{"Bite", 1, 3, 0}, /*xp_reward=*/5, /*evasion=*/15,
     /*accuracy=*/1, /*min_depth=*/1, /*max_depth=*/4, /*attack_range=*/1},
    {"Goblin", 'g', tcod::ColorRGB{80, 180, 80}, 7, Weapon{"Claws", 1, 4, 0}, /*xp_reward=*/10, /*evasion=*/5,
     /*accuracy=*/2, /*min_depth=*/1, /*max_depth=*/4, /*attack_range=*/1},
    // A squishier, ranged relative of the melee Goblin above — glass cannon: less HP,
    // same damage tier, but can fight from range instead of closing to melee. Reuses
    // the exact same attack code as every other monster (see the
    // MonsterTemplate::attack_range comment). accuracy=1 (down from an initial 2)
    // since getting pelted for real damage this early stung more than intended.
    // attack_range=5 so it snipes with its Rock from well across a room without
    // needing to approach at all — but only until the player actually reaches it: the
    // first time it's adjacent it draws its more-accurate Dagger (melee_weapon/
    // melee_accuracy) and permanently commits to melee from then on
    // (Actor::melee_engaged), behaving exactly like an ordinary chasing Goblin for the
    // rest of the fight rather than backing off to snipe again. The hit message names
    // whichever weapon actually landed.
    {"Goblin Slinger", 'G', tcod::ColorRGB{150, 150, 70}, 5, Weapon{"Rock", 1, 4, 0}, /*xp_reward=*/8,
     /*evasion=*/8, /*accuracy=*/1, /*min_depth=*/1, /*max_depth=*/4, /*attack_range=*/5,
     /*melee_weapon=*/Weapon{"Dagger", 1, 4, 0}, /*melee_accuracy=*/3},
    {"Skeleton", 's', tcod::ColorRGB{220, 220, 200}, 10, Weapon{"Rusty Sword", 1, 6, 0}, /*xp_reward=*/15,
     /*evasion=*/8, /*accuracy=*/3, /*min_depth=*/5, /*max_depth=*/-1, /*attack_range=*/1},
    {"Orc", 'o', tcod::ColorRGB{60, 120, 60}, 14, Weapon{"Orc Axe", 1, 8, 0}, /*xp_reward=*/22, /*evasion=*/5,
     /*accuracy=*/4, /*min_depth=*/5, /*max_depth=*/-1, /*attack_range=*/1},
    // Orc Archer: the same ranged-then-permanently-melee behavior as Goblin Slinger
    // (attack_range/melee_weapon/melee_accuracy, see those comments and
    // Actor::melee_engaged), just with every stat scaled up to match this floor-5+
    // tier — same relationship Orc already has to Goblin.
    {"Orc Archer", 'O', tcod::ColorRGB{110, 130, 60}, 10, Weapon{"Bow", 1, 8, 0}, /*xp_reward=*/18,
     /*evasion=*/6, /*accuracy=*/2, /*min_depth=*/5, /*max_depth=*/-1, /*attack_range=*/5,
     /*melee_weapon=*/Weapon{"Short Sword", 1, 8, 0}, /*melee_accuracy=*/5},
    {"Troll", 'T', tcod::ColorRGB{100, 110, 80}, 22, Weapon{"Massive Club", 2, 6, 0}, /*xp_reward=*/40,
     /*evasion=*/2, /*accuracy=*/5, /*min_depth=*/8, /*max_depth=*/-1, /*attack_range=*/1},
};

// A player-summoned minion (Allegiance::Player) — same shape as MonsterTemplate where
// they overlap (name/glyph/color/max_hp/weapon/evasion/accuracy/attack_range,
// optional melee_weapon/melee_accuracy for a ranged-then-engage minion later), but a
// deliberately separate table: which row is available is gated by which summon spell
// unlocked it (Spell::summon_template_index), not by depth, so it doesn't share
// kMonsterTable's min_depth/max_depth/available_at_depth() machinery. No xp_reward
// either — the player doesn't earn XP for a minion dying (see the hostile-monster
// target-death handling in end_turn()).
struct MinionTemplate {
  std::string name;
  char glyph;
  tcod::ColorRGB color;
  int max_hp;
  Weapon weapon;
  int evasion;
  int accuracy;  // dexterity-equivalent, same as MonsterTemplate::accuracy — combat
                 // between any two Actors (minion-vs-hostile or hostile-vs-minion)
                 // reuses dodge_chance_vs(), never the player's own hit-dice system.
  int attack_range;
  Weapon melee_weapon = Weapon{};
  int melee_accuracy = 0;
  // Turns until this minion expires on its own, or -1 for permanent (only dies in
  // combat). See Actor::duration_turns.
  int duration_turns = -1;
};

const std::vector<MinionTemplate> kMinionTable = {
    // A basic, temporary conscript — glyph/color deliberately distinct from the
    // hostile Skeleton ('s', white) so friend and foe never look alike at a glance.
    // Weaker than a real (hostile) Skeleton and time-limited, reflecting that this is
    // an early, low-commitment summon rather than true necromancy (see the roadmap's
    // Phase 3 for permanently reanimating a specific slain monster).
    {"Skeletal Minion", 'u', tcod::ColorRGB{100, 200, 220}, /*max_hp=*/8, Weapon{"Bone Claws", 1, 5, 0},
     /*evasion=*/6, /*accuracy=*/3, /*attack_range=*/1, /*melee_weapon=*/Weapon{}, /*melee_accuracy=*/0,
     /*duration_turns=*/40},
};

// How many minions the player can have active at once, checked when casting a summon
// spell. Deliberately conservative to start (roadmap targets a 1-7 range once the
// pack-order UI has actually been played) — raising this later is a one-constant
// change, not a redesign, since nothing else here assumes a specific pack size.
constexpr int kMaxMinions = 3;

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

std::vector<int> monsters_available_at_depth(int depth) { return available_at_depth(kMonsterTable, depth); }
std::vector<int> weapons_available_at_depth(int depth) { return available_at_depth(kWeaponTable, depth); }
std::vector<int> armor_available_at_depth(int depth) { return available_at_depth(kArmorTable, depth); }
std::vector<int> potions_available_at_depth(int depth) { return available_at_depth(kPotionTable, depth); }

// Max HP scales with both level and Strength — no separate Vitality stat. Leveling up
// alone (regardless of which attribute the point actually goes to) grants a flat
// kHpPerLevel per level past the first, so HP always grows with depth/XP even across a
// run that never touches Strength; Strength then adds even more on top per point
// invested (kHpPerStrength > kHpPerLevel), since it's the stat actually themed around
// survivability. (level - 1) so a fresh level-1 character's starting HP is unaffected —
// the level term only kicks in once they've actually leveled up at least once.
constexpr int kHpPerLevel = 3;
constexpr int kHpPerStrength = 7;
int max_hp_for_level_and_strength(int level, int strength) {
  return 10 + (level - 1) * kHpPerLevel + strength * kHpPerStrength;
}

// Max mana scales with Intelligence only, not level, unlike
// max_hp_for_level_and_strength above — mana costs are small (1-3 per cast, see
// kSpellTable) so the pool only needs to be a handful of casts deep, not hundreds of
// points.
int max_mana_for_intelligence(int intelligence) { return 4 + static_cast<int>(std::ceil(1.8 * intelligence)); }

// Turns for passive HP regen to heal from 0 to full. Regen scales with max HP (see
// end_turn()) so this stays constant regardless of build — a tankier character heals
// more HP per turn, but takes the same number of turns to fully recover.
constexpr int kHpRegenTurns = 150;

// Same idea as kHpRegenTurns, but for mana (see end_turn()). Independent constant so
// the two regen rates can be tuned separately later.
constexpr int kManaRegenTurns = 150;

// Percent chance to dodge an attack entirely, as a straight Dexterity contest between
// defender and attacker (on a monster, its Dexterity doubles as "accuracy" — see
// MonsterTemplate::accuracy). Every point of relative Dexterity is worth
// kDodgePerDexPoint%, on top of kDodgeBaseline for an even match; the min/max keep
// either side from ever hitting a guaranteed hit or a guaranteed miss, even against a
// wildly mismatched Dexterity (e.g. a Dexterity-buffed player vs. a Rat).
constexpr int kDodgePerDexPoint = 8;
constexpr int kDodgeBaseline = 10;
constexpr int kDodgeFloor = 5;
constexpr int kDodgeCeiling = 85;
int dodge_chance_vs(int defender_dex, int attacker_dex) {
  int chance = (defender_dex - attacker_dex) * kDodgePerDexPoint + kDodgeBaseline;
  return std::clamp(chance, kDodgeFloor, kDodgeCeiling);
}

// The player-attacks-monster equivalent of dodge_chance_vs() above, for the other
// direction of combat: the player has no symmetric "evasion" stat of their own to
// contest against (monsters wear no armor, so their flat evasion is their only
// defense), so instead of a live Dexterity contest, the attack's own accuracy —
// hit-dice, on Weapon/Spell — is rolled and subtracted from the target's evasion. A
// bigger roll (a more accurate weapon/spell — a fast dagger, an AoE Fireball) cuts
// further into evasion, making the attack harder to dodge. Reuses the same
// kDodgeFloor/kDodgeCeiling clamp as dodge_chance_vs since "always some chance, never
// a certainty" is a property of any dodge roll in this game, not just the Dexterity
// contest. accuracy_bonus is a flat addition to the roll, defaulting to 0 for every
// existing caller (melee, spells) — only a ranged weapon shot (Mode::RangedAttack)
// sets it, to a Dexterity-derived value, so "hit chance scales with Dexterity" is
// specific to ranged weapons rather than changing how melee/spell accuracy works.
int monster_dodge_chance(int target_evasion, int hit_dice_count, int hit_dice_sides, int accuracy_bonus = 0) {
  int chance = target_evasion - (roll_dice(hit_dice_count, hit_dice_sides) + accuracy_bonus);
  return std::clamp(chance, kDodgeFloor, kDodgeCeiling);
}

// XP required to advance from the given level to the next one.
int xp_needed_for_level(int level) { return level * 20; }

// The player's default, always-available unarmed attack. Not a real pickup, so it's
// never added to the ground or the inventory list.
const Weapon kFists = Weapon{"Fists",       1,  2,  0, /*is_intrinsic=*/true, /*min_depth=*/1, /*max_depth=*/-1,
                              /*hit_dice_count=*/2, /*hit_dice_sides=*/4};

// The player's default, always-available "armor" (bare skin, no defense). Not a real
// pickup, same idea as kFists.
const Armor kNoArmor = Armor{"Nothing", 0, /*is_intrinsic=*/true};

// A ranged spell: damage is dice_count dice of dice_sides each, plus floor(INT/3).
// Known automatically once the player's Intelligence reaches unlock_int — nothing to
// learn or pick up, it just unlocks. (Thresholds are placeholders until the rest of
// the spell pool is designed.)
//
// speed is tiles traveled per player turn, not real time — there's no animation.
// "Instant" just means a speed high enough to always cross the whole map in one turn;
// slower spells (e.g. Fireball) take multiple turns to reach a distant target.
//
// aoe_radius: 0 means the spell only ever hits the single tile/monster it collides
// with (Magic Dart). A positive radius means it explodes on impact into a square blast
// of that Chebyshev radius (1 = 3x3) centered on wherever it stopped, damaging every
// monster caught inside — see advance_projectiles() for how "wherever it stopped" is
// determined (wall/monster/max-range all count). For a toggle spell (below), aoe_radius
// instead sizes the aura around the player — radius R covers a (2R+1)x(2R+1) box —
// recentered every turn.
//
// is_toggle marks a persistent aura spell (e.g. Sandstorm) instead of a fired
// Projectile: selecting it in the spell menu turns it on/off directly, no Targeting
// step (it's always centered on the player). dice_count/dice_sides/speed/range/glyph/
// color are unused for these — mana_cost instead becomes the flat one-time cost to
// turn it on (see the SpellMenu toggle handler), and tick_damage/tick_mana_cost are the
// flat (no dice, no INT bonus) per-turn cost/effect while it stays active, applied in
// end_turn(). See known_spell_indices below for what "known" means either way.
//
// hit_dice: this spell's accuracy against a monster's evasion, same shape and same
// monster_dodge_chance() formula as Weapon::hit_dice_count/sides — a bigger roll is
// harder to dodge. An AoE spell should generally roll much higher than a precise
// single-target one (see kSpellTable below): "wide" is its own kind of hard-to-dodge,
// same as "fast" is for a weapon.
//
// is_summon marks a third spell "kind" alongside a fired Projectile and a toggle:
// selecting it in the spell menu immediately spawns a copy of
// kMinionTable[summon_template_index] as a new Allegiance::Player Actor next to the
// player (see the SpellMenu handler) — no Targeting step, same "resolves right from
// the menu" shape as a toggle spell's on/off, just a one-time effect instead of a
// persistent aura. dice_count/dice_sides/speed/range/aoe_radius/hit_dice/glyph/color
// are unused for these; mana_cost is what it costs to cast, same field every other
// spell already uses.
struct Spell {
  std::string name;
  int unlock_int;
  int dice_count;
  int dice_sides;
  int speed;
  int range;  // max cast distance from the caster, in tiles (straight-line)
  int mana_cost;
  int aoe_radius = 0;
  bool is_toggle = false;
  int tick_damage = 0;     // toggle spells only: flat damage/turn to everything in range
  int tick_mana_cost = 0;  // toggle spells only: flat mana drained/turn while active
  int hit_dice_count;
  int hit_dice_sides;
  char glyph;
  tcod::ColorRGB color;
  // Trailing (with defaults) so the three existing rows above don't need updating —
  // same convention as Weapon::hit_dice_count/sides.
  bool is_summon = false;
  int summon_template_index = 0;  // summon spells only: row into kMinionTable
};

constexpr int kInstantSpellSpeed = 99;  // safely more tiles than this map's diagonal

const std::vector<Spell> kSpellTable = {
    // range happens to match the player's starting FOV radius today, but it's its own
    // fixed number — it won't change if FOV radius ever does (e.g. a future perception
    // mechanic).
    {"Magic Dart", /*unlock_int=*/3, /*dice_count=*/1, /*dice_sides=*/2, kInstantSpellSpeed, /*range=*/8,
     /*mana_cost=*/1, /*aoe_radius=*/0, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/1, /*hit_dice_sides=*/4, '*', tcod::ColorRGB{200, 100, 255}},
    // Slow-moving orb (visibly crosses several turns instead of resolving instantly) that
    // explodes into a 3x3 blast wherever it stops, rather than just hitting one target.
    // Its high hit-dice (vs. Magic Dart's low one) is the AoE-is-hard-to-dodge case.
    {"Fireball", /*unlock_int=*/6, /*dice_count=*/1, /*dice_sides=*/6, /*speed=*/2, /*range=*/8,
     /*mana_cost=*/3, /*aoe_radius=*/1, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/3, /*hit_dice_sides=*/6, 'o', tcod::ColorRGB{255, 120, 40}},
    // Toggled aura, not a fired spell: 7x7 around the player (aoe_radius=3), 2 flat
    // damage/turn to every monster caught in it, drains 2 mana/turn while active.
    // Turning it on costs a flat 3 mana for that turn instead of the per-turn drain
    // (see the SpellMenu toggle handler) — steep enough that flicking it on and off
    // every turn to save mana isn't actually cheaper than just leaving it running.
    {"Sandstorm", /*unlock_int=*/9, /*dice_count=*/0, /*dice_sides=*/0, /*speed=*/0, /*range=*/0,
     /*mana_cost=*/3, /*aoe_radius=*/3, /*is_toggle=*/true, /*tick_damage=*/2, /*tick_mana_cost=*/2,
     /*hit_dice_count=*/2, /*hit_dice_sides=*/6, 's', tcod::ColorRGB{230, 190, 90}},
    // First summon spell: raises kMinionTable[0] (Skeletal Minion) next to the player.
    // unlock_int sits between Magic Dart and Fireball — an early, low-commitment taste
    // of the summoner playstyle before anything heavier. See the SpellMenu handler for
    // how casting a summon spell differs from firing/toggling.
    {"Raise Skeleton", /*unlock_int=*/5, /*dice_count=*/0, /*dice_sides=*/0, /*speed=*/0, /*range=*/0,
     /*mana_cost=*/4, /*aoe_radius=*/0, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/0, /*hit_dice_sides=*/0, 'u', tcod::ColorRGB{100, 200, 220}, /*is_summon=*/true,
     /*summon_template_index=*/0},
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

// A potion lying on the floor, waiting to be picked up.
struct GroundPotion {
  int x, y;
  Potion potion;
};

// A spell (or, via Mode::RangedAttack, a fired weapon shot — same struct, same
// resolution code, just sourced from a Weapon instead of a Spell at fire time) in
// flight: advances along a precomputed path by `speed` tiles every player turn (see
// advance_projectiles), hitting the first wall or monster it reaches.
struct Projectile {
  std::vector<std::pair<int, int>> path;  // tiles from just past the caster through the target
  size_t path_index = 0;                  // how many tiles of the path have been consumed so far
  int speed = 1;
  int dice_count = 1;
  int dice_sides = 2;
  int bonus = 0;  // locked in at cast/fire time (e.g. floor(INT/3), floor(DEX/3)), not re-read later
  int hit_dice_count = 1;  // locked in from the spell/weapon at cast/fire time, see monster_dodge_chance()
  int hit_dice_sides = 4;
  // Flat addition to the accuracy roll (see monster_dodge_chance()) — 0 for every
  // spell (their accuracy is purely hit-dice, unaffected by INT), set to a
  // Dexterity-derived value only for a ranged weapon shot (Mode::RangedAttack).
  int accuracy_bonus = 0;
  int aoe_radius = 0;  // 0 = single-target hit only; >0 = explode in a square blast on impact
  int prev_x = 0;  // last tile actually entered so far (seeded at the caster's tile at cast
  int prev_y = 0;  // time) — where an aoe_radius>0 spell explodes if the next tile is a wall
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

// Index into `monsters` of the living monster standing at (x,y), or -1 if none. Shared
// by advance_projectiles() (resolving a spell hit) and find_impact() below (predicting
// where a shot would land) so both agree on what counts as "occupied."
int monster_at(const std::vector<Actor>& monsters, int x, int y) {
  for (size_t i = 0; i < monsters.size(); ++i) {
    if (monsters[i].x == x && monsters[i].y == y) return static_cast<int>(i);
  }
  return -1;
}

// Same as monster_at(), but only ever matches a hostile monster — a minion at (x,y)
// is invisible to this query. Used everywhere a player-cast spell decides what
// blocks/stops it (advance_projectiles(), find_impact(), the live aim-preview line),
// so the player's own minions are fully transparent to their spells: never targeted,
// never blocking the shot, never accidentally the thing a Magic Dart fizzles against.
int hostile_monster_at(const std::vector<Actor>& monsters, int x, int y) {
  for (size_t i = 0; i < monsters.size(); ++i) {
    if (monsters[i].allegiance == Allegiance::Hostile && monsters[i].x == x && monsters[i].y == y) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// Index into `actors` of the living Actor with the given id, or -1 if it's dead/gone.
// Used to resolve MinionOrder::AttackTarget's attack_target_id back to an actual
// Actor each turn, rather than holding a raw index (unsafe across a vector that
// erases-in-place on death — see Actor::id).
int actor_index_by_id(const std::vector<Actor>& actors, int id) {
  for (size_t i = 0; i < actors.size(); ++i) {
    if (actors[i].id == id && actors[i].is_alive()) return static_cast<int>(i);
  }
  return -1;
}

// How many of the player's minions are currently alive on this floor (minions always
// live on whichever floor the player is currently on — see the cross-floor handling
// in descend()/ascend()). Checked against kMaxMinions when casting a summon spell.
int count_minions(const std::vector<Actor>& monsters) {
  int count = 0;
  for (const auto& m : monsters) {
    if (m.allegiance == Allegiance::Player && m.is_alive()) ++count;
  }
  return count;
}

// One-line status for a minion, for the roster menu (Mode::MinionRoster) — "attacking"
// names the target if it can still be resolved (actor_index_by_id), same fallback
// wording as what the minion itself falls back to (Follow) if it can't. Full console
// width there, so this sentence-length form is safe; the sidebar's Minions list uses
// the single-letter minion_order_flag() below instead — a target's name (e.g.
// "attacking the Goblin Slinger") could otherwise run past that panel's edge.
std::string describe_minion_order(const Actor& minion, const std::vector<Actor>& monsters) {
  if (minion.order == MinionOrder::Hold) return "holding position";
  if (minion.order == MinionOrder::AttackTarget) {
    int ti = actor_index_by_id(monsters, minion.attack_target_id);
    return ti >= 0 ? "attacking the " + monsters[static_cast<size_t>(ti)].name : "following you";
  }
  return "following you";
}

// Compact single-letter order indicator for the sidebar's Minions list (see above).
std::string minion_order_flag(const Actor& minion) {
  switch (minion.order) {
    case MinionOrder::Hold:
      return "H";
    case MinionOrder::AttackTarget:
      return "A";
    default:
      return "F";
  }
}

// Where a shot fired along `path` from (start_x,start_y) would come to rest, applying
// the same three rules advance_projectiles() applies turn-by-turn: a wall stops it on
// the tile just before the wall, a monster stops it on the monster's own tile, and
// running off the end of the path (nothing there) stops it on the final tile. Used by
// the Targeting aim preview to show where an AoE spell would actually explode before
// the player commits to firing — keep this in sync with advance_projectiles() if the
// stopping rules ever change.
std::pair<int, int> find_impact(const std::vector<std::pair<int, int>>& path, int start_x, int start_y,
                                 const Map& map, const std::vector<Actor>& monsters) {
  int prev_x = start_x, prev_y = start_y;
  for (const auto& [x, y] : path) {
    if (!map.is_walkable(x, y)) return {prev_x, prev_y};
    if (hostile_monster_at(monsters, x, y) >= 0) return {x, y};
    prev_x = x;
    prev_y = y;
  }
  return {prev_x, prev_y};  // reached the end of the path with nothing there
}

// Whether a straight line from (fx,fy) to (tx,ty) is unobstructed by walls — used to
// let a ranged monster's attack_range (see MonsterTemplate) be blocked by terrain
// instead of firing straight through it. Checks every tile the path crosses *except
// the last* (the target's own tile doesn't need to be walkable from the shooter's
// perspective — the target is standing on it). For adjacent tiles trace_path()'s
// result is just the single target tile, so the loop below never runs and this is
// trivially true — melee (attack_range=1) is completely unaffected by this check.
bool line_clear(int fx, int fy, int tx, int ty, const Map& map) {
  auto path = trace_path(fx, fy, tx, ty);
  for (size_t i = 0; i + 1 < path.size(); ++i) {
    if (!map.is_walkable(path[i].first, path[i].second)) return false;
  }
  return true;
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
  if (weapon.attack_range > 1) desc += ", range " + std::to_string(weapon.attack_range);
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

// Finds a free (walkable, unoccupied) tile adjacent to (x,y) — the 8 neighbors,
// checked in a fixed order. Used to place a newly summoned minion next to the caster.
// Returns false (out_x/out_y untouched) if every neighbor is blocked.
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

enum class ItemKind { Weapon, Armor, Potion };

// One selectable row in the equip or drop screen. index == -1 means the intrinsic
// default (Fists / Nothing) for Weapon/Armor respectively; otherwise it's an index
// into `inventory`, `armor_inventory`, or `potion_inventory`. Potions have no
// intrinsic/equipped state, so -1 never appears for ItemKind::Potion.
struct ItemSlot {
  ItemKind kind;
  int index;
};

// The droppable list: the currently equipped weapon/armor (omitted if intrinsic),
// followed by everything carried of each kind, including potions.
std::vector<ItemSlot> drop_slots(const Actor& player, const std::vector<Weapon>& inventory,
                                  const std::vector<Armor>& armor_inventory,
                                  const std::vector<Potion>& potion_inventory) {
  std::vector<ItemSlot> slots;
  if (!player.weapon.is_intrinsic) slots.push_back({ItemKind::Weapon, -1});
  for (size_t i = 0; i < inventory.size(); ++i) slots.push_back({ItemKind::Weapon, static_cast<int>(i)});
  if (!player.armor.is_intrinsic) slots.push_back({ItemKind::Armor, -1});
  for (size_t i = 0; i < armor_inventory.size(); ++i) slots.push_back({ItemKind::Armor, static_cast<int>(i)});
  for (size_t i = 0; i < potion_inventory.size(); ++i) slots.push_back({ItemKind::Potion, static_cast<int>(i)});
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
  std::vector<GroundPotion> potions;
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

// Assigns a stable id (see Actor::id) to a newly spawned monster or minion — a plain
// incrementing counter, unique for the life of the process. Ids are only ever compared
// for equality (MinionOrder::AttackTarget matching a specific enemy across turns), so
// there's no need to reset this on a new game/restart; nothing outlives the Level that
// held the old ids anyway.
int allocate_actor_id() {
  static int next_id = 1;
  return next_id++;
}

// Builds and populates a fresh floor. depth is 1-indexed (matches the "Floor:N" HUD)
// and gates which monsters can spawn here, plus how many.
Level generate_level(int width, int height, bool has_stairs_up, int depth) {
  Level level{Map(width, height), {}, {}, {}, {}, {}, {}};
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
    monster.id = allocate_actor_id();
    monster.x = mx;
    monster.y = my;
    monster.hp = monster.max_hp = tmpl.max_hp;
    monster.glyph = tmpl.glyph;
    monster.color = tmpl.color;
    monster.name = tmpl.name;
    monster.weapon = tmpl.weapon;
    monster.xp_reward = tmpl.xp_reward;
    monster.evasion = tmpl.evasion;
    monster.dexterity = tmpl.accuracy;  // read by dodge_chance_vs() when this monster attacks the player
    monster.attack_range = tmpl.attack_range;
    monster.melee_weapon = tmpl.melee_weapon;
    monster.melee_dexterity = tmpl.melee_accuracy;
    level.monsters.push_back(monster);
  }

  auto available_weapons = weapons_available_at_depth(depth);
  for (int i = 0; i < NUM_ITEMS; ++i) {
    auto [ix, iy] = random_free_tile(level.map, occupied);
    occupied.push_back({ix, iy});
    int table_index = available_weapons[static_cast<size_t>(random_int(0, static_cast<int>(available_weapons.size()) - 1))];
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
    int table_index = available_potions[static_cast<size_t>(random_int(0, static_cast<int>(available_potions.size()) - 1))];
    level.potions.push_back(GroundPotion{px, py, kPotionTable[static_cast<size_t>(table_index)]});
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

// Debug convenience for the --give= startup flag (see main()): looks up `name` across
// the weapon/armor/potion content tables, in that order (no name collisions exist
// across the three today), and pushes a copy onto whichever inventory it matched.
// Returns whether anything matched, so the caller can flag a typo instead of silently
// doing nothing — unlike the other debug flags, a silent no-op here would be
// confusing for the one thing this flag exists to do (set up a specific test
// scenario).
bool give_starting_item(const std::string& name, std::vector<Weapon>& inventory, std::vector<Armor>& armor_inventory,
                         std::vector<Potion>& potion_inventory) {
  for (const auto& w : kWeaponTable) {
    if (w.name == name) {
      inventory.push_back(w);
      return true;
    }
  }
  for (const auto& a : kArmorTable) {
    if (a.name == name) {
      armor_inventory.push_back(a);
      return true;
    }
  }
  for (const auto& p : kPotionTable) {
    if (p.name == name) {
      potion_inventory.push_back(p);
      return true;
    }
  }
  return false;
}

// Hand-drawn ASCII box: corners '+', horizontal '-', vertical '|', with an optional
// title embedded in the top border (" Title "). libtcod's own tcod::print_frame() is
// deprecated upstream in favor of exactly this ("print your own banners for frames"),
// so this is the recommended shape, not a workaround. (x, y) is the box's top-left
// corner in console cells; the box is w x h cells including the border, so a panel's
// drawable interior is (x+1, y+1) through (x+w-2, y+h-2).
void draw_panel(tcod::Console& console, int x, int y, int w, int h, const std::string& title,
                 tcod::ColorRGB color = tcod::ColorRGB{120, 120, 120}) {
  for (int i = x + 1; i < x + w - 1; ++i) {
    console.at(i, y).ch = '-';
    console.at(i, y).fg = color;
    console.at(i, y + h - 1).ch = '-';
    console.at(i, y + h - 1).fg = color;
  }
  for (int j = y + 1; j < y + h - 1; ++j) {
    console.at(x, j).ch = '|';
    console.at(x, j).fg = color;
    console.at(x + w - 1, j).ch = '|';
    console.at(x + w - 1, j).fg = color;
  }
  console.at(x, y).ch = '+';
  console.at(x, y).fg = color;
  console.at(x + w - 1, y).ch = '+';
  console.at(x + w - 1, y).fg = color;
  console.at(x, y + h - 1).ch = '+';
  console.at(x, y + h - 1).fg = color;
  console.at(x + w - 1, y + h - 1).ch = '+';
  console.at(x + w - 1, y + h - 1).fg = color;
  if (!title.empty()) {
    tcod::print(console, {x + 2, y}, " " + title + " ", tcod::ColorRGB{255, 255, 255}, std::nullopt);
  }
}

int main(int argc, char* argv[]) {
  // Sectioned HUD layout: a context-prompt row, then a map panel (left) and a
  // stats/enemies/minions "at a glance" sidebar (right) side by side, then a
  // message-log panel spanning the full width along the bottom. Each panel is a
  // hand-drawn ASCII box (see draw_panel()) — libtcod's own tcod::print_frame() is
  // deprecated upstream ("print your own banners for frames"), so this matches the
  // rest of the file's manual console.at()-based drawing rather than pulling in a
  // discouraged helper. MAP_ORIGIN_X/Y is where the *camera's* top-left corner lands
  // on screen; every map-drawing call site offsets by these minus the camera position
  // (see camera_x/camera_y below) instead of the old HUD_HEIGHT.
  //
  // The dungeon itself (MAP_WIDTH x MAP_HEIGHT, what generate_level() builds) is
  // bigger than what's ever shown at once — MAP_VIEW_W x MAP_VIEW_H is the map panel's
  // actual on-screen viewport, a scrolling window that follows the player (or the
  // aim cursor while targeting/commanding — see camera_x/camera_y) so the whole window
  // fits on a normal screen instead of rendering the entire floor at 1:1.
  constexpr int MAP_WIDTH = 100;
  constexpr int MAP_HEIGHT = 27;
  constexpr int MAP_VIEW_W = 50;
  constexpr int MAP_VIEW_H = 24;
  constexpr int MESSAGE_ROWS = 5;  // message-log panel's visible rows, oldest on top

  constexpr int CONTEXT_ROW = 0;  // transient LevelUp/Targeting/MinionFocus prompt

  constexpr int MAP_PANEL_X = 0;
  constexpr int MAP_PANEL_Y = CONTEXT_ROW + 1;
  constexpr int MAP_PANEL_W = MAP_VIEW_W + 2;   // +2 for left/right border
  constexpr int MAP_PANEL_H = MAP_VIEW_H + 2;   // +2 for top/bottom border
  constexpr int MAP_ORIGIN_X = MAP_PANEL_X + 1;
  constexpr int MAP_ORIGIN_Y = MAP_PANEL_Y + 1;

  constexpr int SIDEBAR_X = MAP_PANEL_X + MAP_PANEL_W;
  constexpr int SIDEBAR_Y = MAP_PANEL_Y;
  constexpr int SIDEBAR_W = 28;
  constexpr int SIDEBAR_H = MAP_PANEL_H;

  constexpr int LOG_PANEL_X = MAP_PANEL_X;
  constexpr int LOG_PANEL_Y = MAP_PANEL_Y + MAP_PANEL_H;
  constexpr int LOG_PANEL_W = MAP_PANEL_W + SIDEBAR_W;
  constexpr int LOG_PANEL_H = MESSAGE_ROWS + 2;  // +2 for top/bottom border

  constexpr int SCREEN_WIDTH = LOG_PANEL_W;
  constexpr int SCREEN_HEIGHT = LOG_PANEL_Y + LOG_PANEL_H;

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
  std::vector<Potion> potion_inventory;
  std::vector<std::string> message_log;  // full history; the HUD shows the last MESSAGE_ROWS entries
  int log_scroll = 0;  // lines scrolled up from the bottom, while Mode::MessageLog
  std::string death_cause;  // name of whatever last killed the player, for the death screen
  int pending_attribute_points = 0;  // unspent level-up points forcing a Mode::LevelUp prompt

  // Records a new, distinct message as its own log entry. Everything that happens
  // becomes its own line, even multiple things on the same turn (e.g. an attack
  // landing and the target retaliating) — nothing ever gets concatenated into one.
  // Exception: if this is the exact same text as the last entry (e.g. waiting several
  // turns in a row), it's coalesced into that entry with a "xN" counter instead of
  // spamming a new identical line every time.
  auto add_message = [&](const std::string& text) {
    if (text.empty()) return;
    if (!message_log.empty()) {
      std::string& last = message_log.back();
      std::string last_base = last;
      int count = 1;
      size_t suffix_pos = last.rfind(" x");
      if (suffix_pos != std::string::npos) {
        std::string suffix = last.substr(suffix_pos + 2);
        bool all_digits = !suffix.empty();
        for (char c : suffix) {
          if (c < '0' || c > '9') {
            all_digits = false;
            break;
          }
        }
        if (all_digits) {
          last_base = last.substr(0, suffix_pos);
          count = std::stoi(suffix);
        }
      }
      if (last_base == text) {
        last = text + " x" + std::to_string(count + 1);
        return;
      }
    }
    message_log.push_back(text);
  };

  enum class Mode {
    Playing,
    WeaponMenu,
    ArmorMenu,
    PotionMenu,
    Drop,
    Dead,
    LevelUp,
    SpellMenu,
    Targeting,
    MessageLog,
    Help,
    MinionRoster,
    MinionFocus,
    Look,
    RangedAttack
  };
  int casting_spell_index = -1;  // which kSpellTable entry is being aimed, while Mode::Targeting
  int target_x = 0;  // cursor position, while Mode::Targeting, MinionFocus, Look, or RangedAttack
  int target_y = 0;
  // Index into kSpellTable of the currently-running toggle spell (e.g. Sandstorm), or
  // -1 if none is active. Only one toggle spell can run at a time — simple on purpose,
  // since there's only one so far; a second would need its own slot or a small vector.
  int active_toggle_spell = -1;
  // Which minion currently has command focus while cycling with o/p (see Mode::Playing's
  // key handling) — persists across focus sessions so repeated o/p presses keep moving
  // through the roster in order, not reset to the first minion every time. -1 = no
  // minion individually focused (either never cycled, or Shift+P reset it).
  int focused_minion_id = -1;
  // True only during a Mode::MinionFocus session opened via the roster's "All" option
  // (or the analogous pack-wide path) — the resulting order applies to every living
  // minion instead of just the one named by focused_minion_id. Doesn't touch
  // focused_minion_id itself, so cycling position is preserved across an "All" session.
  bool commanding_all_minions = false;
  Mode mode = Mode::Playing;

  std::vector<Level> levels;
  int current_level = 0;

  // The one place the player's level actually advances: bumps `level` and queues one
  // more forced attribute-point prompt (Mode::LevelUp), same as every other level-up.
  // Deliberately doesn't touch XP itself — grant_xp (below) spends XP as it loops
  // through however many thresholds one reward crosses; the --level= debug startup
  // flag calls this directly to spawn pre-leveled without needing to fake XP. Either
  // caller can call this more than once in a row (a big XP reward killing several
  // monsters at once, or a debug spawn several levels up) — pending_attribute_points
  // just keeps accumulating and the LevelUp prompt loops until it's spent, so double
  // (or more) level-ups are handled by this one core path, not a special case anywhere.
  auto level_up_once = [&]() {
    player.level += 1;
    pending_attribute_points += 1;
    // The level-up itself grants HP (see max_hp_for_level_and_strength) regardless of
    // which attribute the point later gets spent on — previously a run that never
    // chose Strength never gained any max HP at all past character creation. Same
    // "current-jump" convention every other permanent max_hp increase already uses
    // (e.g. the LevelUp Shift+S handler below): current HP rises by the same delta,
    // not just the ceiling.
    int new_max_hp = max_hp_for_level_and_strength(player.level, player.strength);
    player.hp += new_max_hp - player.max_hp;
    player.max_hp = new_max_hp;
  };

  // Grants XP and processes any level-ups it triggers (normally one, but a large XP
  // reward could trigger several), queuing a forced attribute-point prompt for each.
  auto grant_xp = [&](int amount) {
    player.xp += amount;
    while (player.xp >= xp_needed_for_level(player.level)) {
      player.xp -= xp_needed_for_level(player.level);
      level_up_once();
    }
    if (pending_attribute_points > 0) mode = Mode::LevelUp;
  };

  // Advances every in-flight projectile on the current floor by its speed (in tiles),
  // checking each tile it passes through this turn for a wall or monster to hit.
  // Called once per player turn, from end_turn().
  auto advance_projectiles = [&]() {
    Level& level = levels[static_cast<size_t>(current_level)];

    // Deals independently-rolled damage to every living monster within aoe_radius tiles
    // (Chebyshev distance, so a radius of 1 is a 3x3 box) of (cx,cy) — same per-target
    // math as the single-target hit below, just applied to more than one target. Used
    // for spells with aoe_radius > 0 (e.g. Fireball); see find_impact() for how (cx,cy)
    // is chosen to match what the Targeting preview showed the player.
    //
    // The caster isn't exempt: if the player is within radius of the blast too (a point-
    // blank cast, or a wall/monster close enough that the impact lands next to them),
    // they take the same roll. This is the actual deterrent against casting an AoE on
    // something adjacent — unlike the monster hits above, armor still reduces it, same
    // as any other landed hit (see the player's melee-damage-taken code above).
    auto explode = [&](Projectile& proj, int cx, int cy) {
      add_message("Your " + proj.name + " explodes!");
      for (size_t m = 0; m < level.monsters.size();) {
        Actor& target = level.monsters[m];
        // Minions are immune to the player's own AoE splash — only hostile monsters
        // are ever caught in it. (The player themself is still at risk, below.)
        if (target.allegiance == Allegiance::Player) {
          ++m;
          continue;
        }
        if (std::abs(target.x - cx) <= proj.aoe_radius && std::abs(target.y - cy) <= proj.aoe_radius) {
          int dodge_chance = monster_dodge_chance(target.evasion, proj.hit_dice_count, proj.hit_dice_sides);
          if (random_int(1, 100) <= dodge_chance) {
            add_message("The " + target.name + " dodges the blast!");
            ++m;
            continue;
          }
          int damage = roll_dice(proj.dice_count, proj.dice_sides) + proj.bonus;
          target.hp -= damage;
          if (!target.is_alive()) {
            add_message("The blast kills the " + target.name + "!");
            int xp_reward = target.xp_reward;  // read before erase invalidates `target`
            level.monsters.erase(level.monsters.begin() + static_cast<long>(m));
            grant_xp(xp_reward);
            continue;  // next monster has shifted down into slot m; don't advance past it
          }
          add_message("The blast hits the " + target.name + " for " + std::to_string(damage) + ".");
        }
        ++m;
      }

      if (player.is_alive() && std::abs(player.x - cx) <= proj.aoe_radius && std::abs(player.y - cy) <= proj.aoe_radius) {
        int raw_damage = roll_dice(proj.dice_count, proj.dice_sides) + proj.bonus;
        int damage = std::max(raw_damage - player.armor.defense, 0);
        player.hp -= damage;
        add_message("You're caught in your own " + proj.name + " for " + std::to_string(damage) + "!");
        if (!player.is_alive()) {
          death_cause = proj.name + " you cast";
          mode = Mode::Dead;
        }
      }
    };

    for (size_t i = 0; i < level.projectiles.size();) {
      Projectile& proj = level.projectiles[i];
      bool consumed = false;

      for (int step = 0; step < proj.speed && !consumed; ++step) {
        if (proj.path_index >= proj.path.size()) {
          // Reached the end of its range with nothing there. A single-target spell just
          // dissipates, as before; an AoE spell still goes off at its destination, since
          // that's the "reaches its destination" case (may still catch nearby monsters).
          if (proj.aoe_radius > 0) explode(proj, proj.prev_x, proj.prev_y);
          consumed = true;
          break;
        }
        auto [x, y] = proj.path[proj.path_index];
        ++proj.path_index;

        if (!level.map.is_walkable(x, y)) {
          if (proj.aoe_radius > 0) {
            explode(proj, proj.prev_x, proj.prev_y);  // last open tile before the wall
          } else {
            add_message("Your " + proj.name + " fizzles against a wall.");
          }
          consumed = true;
          break;
        }

        int target_index = hostile_monster_at(level.monsters, x, y);
        if (target_index >= 0) {
          if (proj.aoe_radius > 0) {
            explode(proj, x, y);  // the monster's own tile is a valid, walkable center
          } else {
            Actor& target = level.monsters[static_cast<size_t>(target_index)];
            int dodge_chance =
                monster_dodge_chance(target.evasion, proj.hit_dice_count, proj.hit_dice_sides, proj.accuracy_bonus);
            if (random_int(1, 100) <= dodge_chance) {
              add_message("The " + target.name + " dodges your " + proj.name + "!");
            } else {
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
            }
          }
          consumed = true;
          break;
        }

        // Empty, walkable tile: the spell keeps going. Remember it as the last open tile
        // in case the very next one stops it (the aoe_radius wall-hit case above).
        proj.prev_x = x;
        proj.prev_y = y;
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
    // Passive HP regen: scales with max HP (see kHpRegenTurns) so a full heal takes
    // roughly the same number of turns regardless of build. Silent — no log message —
    // since it ticks often enough that logging it would just spam the message log.
    if (player.is_alive() && player.hp < player.max_hp) {
      player.hp_regen_accumulator += static_cast<float>(player.max_hp) / static_cast<float>(kHpRegenTurns);
      while (player.hp_regen_accumulator >= 1.0f && player.hp < player.max_hp) {
        player.hp_regen_accumulator -= 1.0f;
        player.hp += 1;
      }
    }

    // Passive mana regen: identical shape to the HP regen above (see kManaRegenTurns).
    if (player.is_alive() && player.mana < player.max_mana) {
      player.mana_regen_accumulator += static_cast<float>(player.max_mana) / static_cast<float>(kManaRegenTurns);
      while (player.mana_regen_accumulator >= 1.0f && player.mana < player.max_mana) {
        player.mana_regen_accumulator -= 1.0f;
        player.mana += 1;
      }
    }

    // Temporary stat buffs (Potion of Strength/Dexterity/Intelligence) count down every
    // turn and revert automatically the instant they expire.
    if (player.temp_str_turns > 0) {
      player.temp_str_turns -= 1;
      if (player.temp_str_turns == 0) {
        player.temp_str_bonus = 0;
        player.max_hp = max_hp_for_level_and_strength(player.level, player.strength);
        player.hp = std::min(player.hp, player.max_hp);  // clamp in case regen filled past the new, lower ceiling
        add_message("Your surge of strength fades.");
      }
    }
    if (player.temp_dex_turns > 0) {
      player.temp_dex_turns -= 1;
      if (player.temp_dex_turns == 0) {
        player.temp_dex_bonus = 0;
        add_message("Your surge of agility fades.");
      }
    }
    if (player.temp_int_turns > 0) {
      player.temp_int_turns -= 1;
      if (player.temp_int_turns == 0) {
        player.temp_int_bonus = 0;
        player.max_mana = max_mana_for_intelligence(player.intelligence);
        player.mana = std::min(player.mana, player.max_mana);  // clamp past the new, lower ceiling
        add_message("Your surge of insight fades.");
      }
    }

    advance_projectiles();

    Level& level = levels[static_cast<size_t>(current_level)];

    // Minion duration: a timed minion (duration_turns > 0, see MinionTemplate) expires
    // on its own once it hits 0, same "tick down, then resolve" shape as the temp stat
    // buffs above. A permanent minion (duration_turns <= 0, the default) never enters
    // this countdown at all. Marked via hp = 0 rather than erased here — same deferred-
    // sweep reasoning as the AI loops below, and it also means an expiring minion
    // doesn't get to act this same turn (is_alive() already gates both AI loops).
    for (auto& m : level.monsters) {
      if (m.allegiance != Allegiance::Player || !m.is_alive() || m.duration_turns <= 0) continue;
      m.duration_turns -= 1;
      if (m.duration_turns == 0) {
        add_message("Your " + m.name + " collapses into dust.");
        m.hp = 0;
      }
    }

    // Toggled aura spells (e.g. Sandstorm): while active, drains tick_mana_cost every
    // turn and deals tick_damage to every monster within aoe_radius tiles (Chebyshev
    // distance) of the player's *current* position — recentered each turn, since the
    // aura follows the player rather than sitting where it was cast. Flat damage, no
    // dice roll or INT bonus, unlike the Projectile spells above. Shuts itself off if
    // the player can no longer afford the drain. The turn a storm is first turned on
    // pays a flat activation cost instead of this tick — active_toggle_spell isn't set
    // until after that turn's end_turn() call (see the SpellMenu toggle handler), so
    // this only starts draining/damaging on the turn after.
    if (active_toggle_spell >= 0) {
      const Spell& storm = kSpellTable[static_cast<size_t>(active_toggle_spell)];
      if (player.mana < storm.tick_mana_cost) {
        add_message("Your " + storm.name + " dies down - out of mana.");
        active_toggle_spell = -1;
      } else {
        player.mana -= storm.tick_mana_cost;
        for (size_t m = 0; m < level.monsters.size();) {
          Actor& target = level.monsters[m];
          // Minions stand in the storm untouched — see explode()'s identical
          // exemption for Fireball's blast.
          if (target.allegiance == Allegiance::Player) {
            ++m;
            continue;
          }
          if (std::abs(target.x - player.x) <= storm.aoe_radius && std::abs(target.y - player.y) <= storm.aoe_radius) {
            int dodge_chance = monster_dodge_chance(target.evasion, storm.hit_dice_count, storm.hit_dice_sides);
            if (random_int(1, 100) <= dodge_chance) {
              add_message("The " + target.name + " dodges the " + storm.name + "!");
              ++m;
              continue;
            }
            target.hp -= storm.tick_damage;
            if (!target.is_alive()) {
              add_message("Your " + storm.name + " kills the " + target.name + "!");
              int xp_reward = target.xp_reward;  // read before erase invalidates `target`
              level.monsters.erase(level.monsters.begin() + static_cast<long>(m));
              grant_xp(xp_reward);
              continue;  // next monster has shifted down into slot m; don't advance past it
            }
            add_message("Your " + storm.name + " hits the " + target.name + " for " +
                        std::to_string(storm.tick_damage) + ".");
          }
          ++m;
        }
      }
    }

    // Tries to step a monster by (step_dx, step_dy); does nothing and returns false if
    // that tile is a wall, already has another living monster on it, or is the
    // player's own tile (the player isn't in level.monsters, so that needs its own
    // check — no monster/minion ever displaces the player by walking into them; the
    // player initiates all bump-to-attack contact, never the other way around).
    auto try_monster_step = [&](Actor& m, int step_dx, int step_dy) -> bool {
      if (step_dx == 0 && step_dy == 0) return false;
      int nx = m.x + step_dx;
      int ny = m.y + step_dy;
      if (!level.map.is_walkable(nx, ny)) return false;
      if (nx == player.x && ny == player.y) return false;
      for (const auto& other : level.monsters) {
        if (&other != &m && other.is_alive() && other.x == nx && other.y == ny) return false;
      }
      m.x = nx;
      m.y = ny;
      return true;
    };

    // Resolves `attacker` attacking `defender`, where defender is anything other than
    // the player (the player has armor and a death screen — handled inline in the
    // hostile-monster loop below instead). Works symmetrically whichever direction:
    // a hostile monster attacking a minion, or a minion attacking a hostile monster —
    // both are monster-shaped Actors with the same dexterity/weapon/optional
    // melee_weapon fields, so this is the exact combat math every monster-vs-player
    // attack already used, just re-pointed at another Actor and phrased for whichever
    // side is "yours". No armor reduction (only the player wears armor). Grants XP if
    // the attacker is one of the player's minions and the kill is a hostile monster —
    // a minion's kill still furthers the player — never for a hostile monster's kill
    // (killing a minion isn't a player achievement). Doesn't erase a dead defender;
    // see the deferred sweep after both AI loops below for why.
    auto resolve_actor_attack = [&](Actor& attacker, Actor& defender) {
      bool adjacent = std::abs(defender.x - attacker.x) <= 1 && std::abs(defender.y - attacker.y) <= 1;
      bool use_melee_weapon = adjacent && !attacker.melee_weapon.name.empty();
      if (use_melee_weapon) attacker.melee_engaged = true;
      const Weapon& weapon_used = use_melee_weapon ? attacker.melee_weapon : attacker.weapon;
      int attacker_dex = use_melee_weapon ? attacker.melee_dexterity : attacker.dexterity;

      std::string attacker_subject = (attacker.allegiance == Allegiance::Player ? "Your " : "The ") + attacker.name;
      std::string attacker_lower = (attacker.allegiance == Allegiance::Player ? "your " : "the ") + attacker.name;
      std::string defender_subject = (defender.allegiance == Allegiance::Player ? "Your " : "The ") + defender.name;
      std::string defender_lower = (defender.allegiance == Allegiance::Player ? "your " : "the ") + defender.name;

      if (random_int(1, 100) <= dodge_chance_vs(defender.dexterity, attacker_dex)) {
        add_message(defender_subject + " dodges " + attacker_lower + "'s attack!");
        return;
      }
      int damage = roll_damage(weapon_used);
      defender.hp -= damage;
      if (!defender.is_alive()) {
        add_message(attacker_subject + " kills " + defender_lower + "!");
        if (attacker.allegiance == Allegiance::Player) grant_xp(defender.xp_reward);
      } else {
        add_message(attacker_subject + " hits " + defender_lower + " with its " + weapon_used.name + " for " +
                    std::to_string(damage) + ".");
      }
    };

    for (auto& monster : level.monsters) {
      if (mode == Mode::Dead) break;  // player already died to an earlier monster this turn
      if (!monster.is_alive() || monster.allegiance != Allegiance::Hostile) continue;

      // Picks this monster's target for the turn: the player, or the closest living
      // minion whose tile is currently in the player's FOV (there's no separate
      // per-monster FOV; "lit right now" — the same is_in_fov() check already used to
      // decide whether to even render a minion — stands in for "this monster would
      // notice it"). Ties favor the player. Minions don't get their own "remembered
      // last position" the way the player does (last_seen_player_x/y below is still
      // player-specific) — a Phase 1 simplification. With zero minions on the floor
      // this always resolves to the player, so solo-player behavior is unchanged.
      bool target_is_player = true;
      int target_minion_index = -1;
      int target_x = player.x;
      int target_y = player.y;
      int best_dist = std::max(std::abs(player.x - monster.x), std::abs(player.y - monster.y));
      for (size_t ti = 0; ti < level.monsters.size(); ++ti) {
        const Actor& candidate = level.monsters[ti];
        if (candidate.allegiance != Allegiance::Player || !candidate.is_alive()) continue;
        if (!level.map.is_in_fov(candidate.x, candidate.y)) continue;
        int dist = std::max(std::abs(candidate.x - monster.x), std::abs(candidate.y - monster.y));
        if (dist < best_dist) {
          best_dist = dist;
          target_is_player = false;
          target_minion_index = static_cast<int>(ti);
          target_x = candidate.x;
          target_y = candidate.y;
        }
      }

      int dx = target_x - monster.x;
      int dy = target_y - monster.y;
      int abs_dx = dx < 0 ? -dx : dx;
      int abs_dy = dy < 0 ? -dy : dy;
      // Generalizes "adjacent" (melee) into "within this monster's attack_range" —
      // attack_range=1 (every monster before ranged attackers existed) makes this
      // exactly today's adjacency check. A wall between the two still blocks it
      // (line_clear()), which for attack_range=1 is always trivially true (see its
      // comment) — melee behavior is completely unchanged.
      //
      // Once melee_engaged (see Actor::melee_engaged), a ranged monster permanently
      // stops using its full attack_range and behaves as if it were 1 — it snipes from
      // range right up until it reaches its target, and from then on it's just a
      // melee-only monster (chasing when out of reach, same as any other), never going
      // back to sniping.
      int effective_range = monster.melee_engaged ? 1 : monster.attack_range;
      bool in_range = std::max(abs_dx, abs_dy) <= effective_range && (dx != 0 || dy != 0);
      bool can_attack = in_range && line_clear(monster.x, monster.y, target_x, target_y, level.map);

      if (can_attack) {
        if (!target_is_player) {
          resolve_actor_attack(monster, level.monsters[static_cast<size_t>(target_minion_index)]);
          continue;
        }
        // Some monsters (e.g. Goblin Slinger) fall back to a separate, more accurate
        // melee weapon once you're actually adjacent, rather than using their ranged
        // one at point-blank range — see MonsterTemplate::melee_weapon. Every other
        // monster's melee_weapon is left empty, so this is always false for them and
        // they just always use weapon/dexterity, same as before this existed. This is
        // also the trigger that flips melee_engaged for good, above.
        bool in_melee = abs_dx <= 1 && abs_dy <= 1;
        bool use_melee_weapon = in_melee && !monster.melee_weapon.name.empty();
        if (use_melee_weapon) monster.melee_engaged = true;
        const Weapon& weapon_used = use_melee_weapon ? monster.melee_weapon : monster.weapon;
        int attacker_dex = use_melee_weapon ? monster.melee_dexterity : monster.dexterity;

        int player_dex = player.dexterity + player.temp_dex_bonus;
        if (random_int(1, 100) <= dodge_chance_vs(player_dex, attacker_dex)) {
          add_message("You dodge the " + monster.name + "'s attack!");
          continue;
        }

        int raw_damage = roll_damage(weapon_used);
        int damage = std::max(raw_damage - player.armor.defense, 0);
        player.hp -= damage;
        add_message("The " + monster.name + " hits you with its " + weapon_used.name + " for " +
                    std::to_string(damage) + ".");
        if (!player.is_alive()) {
          death_cause = monster.name;
          mode = Mode::Dead;
        }
        continue;
      }

      // Out of range (or no line of sight): chase toward the chosen target if it's
      // currently visible — for the player specifically, "visible" still means the
      // FOV-reciprocity check below (can_see_player), same as always; a minion target
      // was already required to be in_fov to be picked as the target at all, above.
      // Otherwise, if the monster still remembers where it last saw the player
      // specifically, head there instead of immediately giving up — once it arrives
      // and the player isn't there, the memory clears and it falls back to idle
      // wandering. Movement follows a real A* path (Map::find_path(), libtcod's
      // TCODPath) recomputed fresh every turn — cheap enough at this map size that
      // there's no need to cache it turn-to-turn — so a monster routes around a wall
      // segment instead of pacing against it.
      bool can_see_player = level.map.is_in_fov(monster.x, monster.y);
      if (can_see_player) {
        monster.last_seen_player_x = player.x;
        monster.last_seen_player_y = player.y;
      }

      int move_dx = 0;
      int move_dy = 0;
      bool can_see_target = target_is_player ? can_see_player : true;  // minion targets are always is_in_fov, see above
      bool has_chase_target = can_see_target || monster.last_seen_player_x >= 0;
      if (has_chase_target) {
        int chase_x = can_see_target ? target_x : monster.last_seen_player_x;
        int chase_y = can_see_target ? target_y : monster.last_seen_player_y;
        if (chase_x == monster.x && chase_y == monster.y) {
          // Arrived at the last-known spot and nothing's here: give up the chase.
          // Falls through to a wander roll below this turn, same as if there'd never
          // been anything to chase.
          monster.last_seen_player_x = -1;
          monster.last_seen_player_y = -1;
        } else {
          auto path = level.map.find_path(monster.x, monster.y, chase_x, chase_y);
          // Empty means no route exists at all — falls through to the wander roll
          // below, same shape as today's "stuck" case, just genuinely no path instead
          // of one greedy step happening to be blocked.
          if (!path.empty()) {
            move_dx = path[0].first - monster.x;
            move_dy = path[0].second - monster.y;
          }
        }
      }
      if (move_dx == 0 && move_dy == 0 && monster.last_seen_player_x < 0 && random_int(0, 1) == 0) {
        // Wander: only a coin-flip chance to shuffle each turn, so it reads as idle
        // rather than frantic. Only reachable with no memory to chase — see above.
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

    // Attacks the closest hostile within minion.attack_range (line_clear()'d), if any
    // — the "still defend yourself" half of Follow and Hold, so a minion doing either
    // isn't a free hit for anything that wanders adjacent. Returns whether it attacked
    // (the caller should skip movement for the turn if so).
    auto try_minion_auto_defend = [&](Actor& minion) -> bool {
      int best_hostile = -1;
      int best_dist = 0;
      for (size_t hi = 0; hi < level.monsters.size(); ++hi) {
        const Actor& hostile = level.monsters[hi];
        if (hostile.allegiance != Allegiance::Hostile || !hostile.is_alive()) continue;
        int hdx = std::abs(hostile.x - minion.x);
        int hdy = std::abs(hostile.y - minion.y);
        int dist = std::max(hdx, hdy);
        if (dist > minion.attack_range) continue;
        if (!line_clear(minion.x, minion.y, hostile.x, hostile.y, level.map)) continue;
        if (best_hostile < 0 || dist < best_dist) {
          best_hostile = static_cast<int>(hi);
          best_dist = dist;
        }
      }
      if (best_hostile < 0) return false;
      resolve_actor_attack(minion, level.monsters[static_cast<size_t>(best_hostile)]);
      return true;
    };

    // The player's minions act after every hostile monster has had its turn. Each one
    // is Following (path toward the player, ignoring FOV — a summoned minion always
    // knows where its own summoner is, unlike a hostile monster tracking the player),
    // Holding (path toward and then stand at a specific tile — "guard this spot"), or
    // AttackTarget (path toward/attack one specific enemy, by id). Follow and Hold both
    // defend themselves via try_minion_auto_defend() instead of moving if a hostile is
    // already in range. An AttackTarget minion whose target has died or otherwise
    // disappeared (actor_index_by_id returns -1) reverts to Follow and just holds
    // position for the rest of this turn, picking up the chase next turn.
    for (auto& minion : level.monsters) {
      if (mode == Mode::Dead) break;
      if (!minion.is_alive() || minion.allegiance != Allegiance::Player) continue;

      if (minion.order == MinionOrder::AttackTarget) {
        int ti = actor_index_by_id(level.monsters, minion.attack_target_id);
        if (ti < 0) {
          minion.order = MinionOrder::Follow;
          continue;
        }
        Actor& target = level.monsters[static_cast<size_t>(ti)];
        int tdx = std::abs(target.x - minion.x);
        int tdy = std::abs(target.y - minion.y);
        bool in_range = std::max(tdx, tdy) <= minion.attack_range;
        if (in_range && line_clear(minion.x, minion.y, target.x, target.y, level.map)) {
          resolve_actor_attack(minion, target);
          continue;
        }
        auto path = level.map.find_path(minion.x, minion.y, target.x, target.y);
        if (!path.empty()) {
          int move_dx = path[0].first - minion.x;
          int move_dy = path[0].second - minion.y;
          if (!try_monster_step(minion, move_dx, move_dy)) {
            if (!try_monster_step(minion, move_dx, 0)) try_monster_step(minion, 0, move_dy);
          }
        }
        continue;
      }

      if (minion.order == MinionOrder::Hold) {
        if (try_minion_auto_defend(minion)) continue;
        if (minion.x == minion.hold_x && minion.y == minion.hold_y) continue;  // already there
        auto path = level.map.find_path(minion.x, minion.y, minion.hold_x, minion.hold_y);
        if (!path.empty()) {
          int move_dx = path[0].first - minion.x;
          int move_dy = path[0].second - minion.y;
          if (!try_monster_step(minion, move_dx, move_dy)) {
            if (!try_monster_step(minion, move_dx, 0)) try_monster_step(minion, 0, move_dy);
          }
        }
        continue;
      }

      // Follow.
      if (try_minion_auto_defend(minion)) continue;
      // ...otherwise close the distance to the player. try_monster_step already
      // refuses to step onto the player's own tile, so this naturally stops once
      // adjacent rather than trying to stack on them.
      auto path = level.map.find_path(minion.x, minion.y, player.x, player.y);
      if (!path.empty()) {
        int move_dx = path[0].first - minion.x;
        int move_dy = path[0].second - minion.y;
        try_monster_step(minion, move_dx, move_dy);
      }
    }

    // Deferred cleanup: remove anything that died during either AI loop above (a
    // minion killed by a hostile monster, or a hostile monster killed by a minion).
    // Deferred rather than erased in-place during either loop above, since both loops
    // iterate over this same vector and can each kill something the *other* loop
    // still needs to reference later in the same turn (e.g. monster #2 kills a minion
    // that monster #5 is also currently targeting) — erasing immediately would shift
    // indices out from under whichever loop hadn't gotten there yet. Contrast with
    // explode()/advance_projectiles(), which erase immediately — those are
    // self-contained single-purpose loops, not two loops that can each kill something
    // the other is also indexing into.
    for (size_t i = 0; i < level.monsters.size();) {
      if (!level.monsters[i].is_alive()) {
        level.monsters.erase(level.monsters.begin() + static_cast<long>(i));
      } else {
        ++i;
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
    player.max_hp = max_hp_for_level_and_strength(player.level, player.strength);
    player.hp = player.max_hp;
    player.hp_regen_accumulator = 0.0f;
    player.max_mana = max_mana_for_intelligence(player.intelligence);
    player.mana = player.max_mana;
    player.mana_regen_accumulator = 0.0f;
    player.weapon = kFists;
    player.armor = kNoArmor;
    player.temp_str_bonus = 0;
    player.temp_str_turns = 0;
    player.temp_dex_bonus = 0;
    player.temp_dex_turns = 0;
    player.temp_int_bonus = 0;
    player.temp_int_turns = 0;
    level.map.update_fov(player.x, player.y, FOV_RADIUS);

    inventory.clear();
    armor_inventory.clear();
    potion_inventory.clear();
    pending_attribute_points = 0;
    active_toggle_spell = -1;
    message_log.clear();
    add_message("Welcome to the dungeon. Press '?' for controls.");
    mode = Mode::Playing;
  };

  // Goes down the stairs the player is currently standing on, generating the floor
  // below the first time it's visited.
  // Moves every one of the player's minions from `from_level` to `to_level`,
  // positioned near the player's new spot — called by descend()/ascend() so minions
  // follow the player between floors instead of being left behind (floors are
  // otherwise fully independent/persistent — see the Level comment). Falls back to
  // any free tile on the new floor if the immediate area around the player is too
  // crowded to fit everyone adjacent.
  auto move_minions_to_new_floor = [&](Level& from_level, Level& to_level) {
    for (size_t i = 0; i < from_level.monsters.size();) {
      if (from_level.monsters[i].allegiance != Allegiance::Player) {
        ++i;
        continue;
      }
      Actor minion = from_level.monsters[i];
      from_level.monsters.erase(from_level.monsters.begin() + static_cast<long>(i));
      int nx, ny;
      if (free_adjacent_tile(to_level.map, to_level.monsters, player.x, player.y, nx, ny)) {
        minion.x = nx;
        minion.y = ny;
      } else {
        std::vector<std::pair<int, int>> occupied = {{player.x, player.y}};
        for (const auto& m : to_level.monsters) occupied.push_back({m.x, m.y});
        auto [rx, ry] = random_free_tile(to_level.map, occupied);
        minion.x = rx;
        minion.y = ry;
      }
      to_level.monsters.push_back(minion);
      // Don't advance i — the erase above shifted the next element into slot i.
    }
  };

  auto descend = [&]() {
    int old_index = current_level;
    current_level += 1;
    if (static_cast<size_t>(current_level) >= levels.size()) {
      levels.push_back(generate_level(MAP_WIDTH, MAP_HEIGHT, /*has_stairs_up=*/true, /*depth=*/current_level + 1));
    }
    // Both re-fetched fresh, after the possible push_back above — which can reallocate
    // `levels` and would dangle a reference taken any earlier (same hazard noted where
    // the render loop re-fetches `level` per event).
    Level& old_level = levels[static_cast<size_t>(old_index)];
    Level& level = levels[static_cast<size_t>(current_level)];
    player.x = level.entry_x;
    player.y = level.entry_y;
    move_minions_to_new_floor(old_level, level);
    level.map.update_fov(player.x, player.y, FOV_RADIUS);
    add_message("You descend the stairs.");
  };

  // Goes back up to the floor above, landing on the stairs down that was taken from it.
  auto ascend = [&]() {
    int old_index = current_level;
    current_level -= 1;
    Level& old_level = levels[static_cast<size_t>(old_index)];
    Level& level = levels[static_cast<size_t>(current_level)];
    player.x = level.stairs_down_x;
    player.y = level.stairs_down_y;
    move_minions_to_new_floor(old_level, level);
    level.map.update_fov(player.x, player.y, FOV_RADIUS);
    add_message("You ascend the stairs.");
  };

  start_new_game();

  // Debug convenience: `--floor=N` jumps straight to floor N at startup, so testing
  // deep floors doesn't require a long walk down through every floor above it. Not
  // meant for normal play; a missing/malformed value is just silently ignored.
  //
  // `--level=N` spawns the player already at level N, forcing the same Mode::LevelUp
  // prompt (N-1) times in a row so you allocate every point yourself, same as leveling
  // up for real — it calls level_up_once() directly rather than faking XP, so it's the
  // exact core path a big XP reward would also drive if it crossed several thresholds
  // at once (see level_up_once's comment). Combinable with --floor=N.
  //
  // `--reveal` shows every tile/monster/item on the current floor regardless of
  // exploration or FOV (still dimmed if not actually in current sight, matching the
  // remembered-terrain/monster look) — for eyeballing spawns and loot without having
  // to walk the whole floor first. Also debug-only, off by default.
  //
  // `--dump-loot` prints every weapon/armor/potion/monster on the floor reached via
  // --floor=N (floor 1 if that flag's absent) to stdout, then exits before the window
  // ever opens — a scriptable alternative to eyeballing --reveal in the live window,
  // for checking depth-gating (kWeaponTable etc.) actually filters as intended.
  //
  // `--give=<name>[,<name>...]` adds items straight to the carried inventory at
  // startup (not auto-equipped — same "found it, now equip it" flow as picking one up,
  // just skipping the walk), for testing a specific item without grinding to find one.
  // Comma-separated, e.g. --give="Dagger,Potion of Teleportation"; see
  // give_starting_item() above.
  bool reveal_mode = false;
  bool dump_loot = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--reveal") {
      reveal_mode = true;
      continue;
    }
    if (arg == "--dump-loot") {
      dump_loot = true;
      continue;
    }
    const std::string floor_prefix = "--floor=";
    if (arg.rfind(floor_prefix, 0) == 0) {
      int target_floor = std::atoi(arg.c_str() + floor_prefix.size());
      for (int f = 1; f < target_floor; ++f) descend();
      continue;
    }
    const std::string level_prefix = "--level=";
    if (arg.rfind(level_prefix, 0) == 0) {
      int target_level = std::atoi(arg.c_str() + level_prefix.size());
      for (int lv = player.level; lv < target_level; ++lv) level_up_once();
      continue;
    }
    const std::string give_prefix = "--give=";
    if (arg.rfind(give_prefix, 0) == 0) {
      std::string names = arg.substr(give_prefix.size());
      size_t pos = 0;
      while (pos <= names.size()) {
        size_t comma = names.find(',', pos);
        std::string name = names.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!name.empty()) {
          if (give_starting_item(name, inventory, armor_inventory, potion_inventory)) {
            add_message("Debug: added " + name + " to inventory.");
          } else {
            add_message("Debug: no item named \"" + name + "\" found.");
          }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
      continue;
    }
  }
  // Surfaces the LevelUp prompt if --level= queued any points, same as grant_xp does.
  if (pending_attribute_points > 0) mode = Mode::LevelUp;

  if (dump_loot) {
    Level& level = levels[static_cast<size_t>(current_level)];
    std::cout << "Floor " << (current_level + 1) << " loot:\n";
    for (const auto& item : level.items) {
      std::cout << "  weapon: " << item.weapon.name << " (" << describe_weapon(item.weapon) << ")\n";
    }
    for (const auto& armor_item : level.armor_items) {
      std::cout << "  armor: " << armor_item.armor.name << " (" << describe_armor(armor_item.armor) << ")\n";
    }
    for (const auto& ground_potion : level.potions) {
      std::cout << "  potion: " << ground_potion.potion.name << " (" << describe_potion(ground_potion.potion)
                 << ")\n";
    }
    std::cout << "Floor " << (current_level + 1) << " monsters:\n";
    for (const auto& monster : level.monsters) {
      std::cout << "  " << monster.name << "\n";
    }
    return 0;
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
    } else if (mode == Mode::PotionMenu) {
      tcod::print(console, {0, 0}, "Potions - press a letter to drink, Esc to close", tcod::ColorRGB{255, 255, 255},
                  std::nullopt);

      if (potion_inventory.empty()) {
        tcod::print(console, {0, 2}, "(no potions carried)", tcod::ColorRGB{120, 120, 120}, std::nullopt);
      }
      for (size_t i = 0; i < potion_inventory.size(); ++i) {
        std::string line = std::string(1, static_cast<char>('a' + i)) + ") " + potion_inventory[i].name + " (" +
                            describe_potion(potion_inventory[i]) + ")";
        tcod::print(console, {0, 2 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
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
        bool is_active = active_toggle_spell == known[i];
        std::string line;
        bool at_minion_cap = false;
        if (s.is_toggle) {
          line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (" +
                 std::to_string(s.tick_damage) + " dmg/turn in " + std::to_string(2 * s.aoe_radius + 1) + "x" +
                 std::to_string(2 * s.aoe_radius + 1) + ", " + std::to_string(s.tick_mana_cost) + " MP/turn) - " +
                 std::to_string(s.mana_cost) + " MP to toggle" + (is_active ? " [ACTIVE]" : "");
        } else if (s.is_summon) {
          const MinionTemplate& tmpl = kMinionTable[static_cast<size_t>(s.summon_template_index)];
          std::string duration_str =
              tmpl.duration_turns > 0 ? std::to_string(tmpl.duration_turns) + " turns" : "permanent";
          at_minion_cap = count_minions(level.monsters) >= kMaxMinions;
          line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (summons a " + tmpl.name + ", " +
                 duration_str + ") - " + std::to_string(s.mana_cost) + " MP" + (at_minion_cap ? " [AT CAP]" : "");
        } else {
          line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (" + std::to_string(s.dice_count) +
                 "d" + std::to_string(s.dice_sides) + "+INT/3) - " + std::to_string(s.mana_cost) + " MP";
        }
        // Dimmed red instead of the usual grey once you can't actually afford it — a
        // currently-active toggle is always "affordable" to select again (turning it
        // off is always free) so it doesn't get the red treatment.
        bool affordable = is_active || (player.mana >= s.mana_cost && !at_minion_cap);
        tcod::print(console, {0, 2 + static_cast<int>(i)}, line,
                    affordable ? tcod::ColorRGB{200, 200, 200} : tcod::ColorRGB{150, 80, 80}, std::nullopt);
      }
    } else if (mode == Mode::Drop) {
      tcod::print(console, {0, 0}, "Drop - press a letter to drop, Esc to cancel", tcod::ColorRGB{255, 255, 255},
                  std::nullopt);

      auto slots = drop_slots(player, inventory, armor_inventory, potion_inventory);
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
        } else if (slots[i].kind == ItemKind::Armor) {
          const Armor& a = (slots[i].index == -1) ? player.armor : armor_inventory[static_cast<size_t>(slots[i].index)];
          line = std::string(1, letter) + ") " + a.name + " (" + describe_armor(a) + ")";
          if (slots[i].index == -1) line += " [equipped]";
        } else {
          const Potion& p = potion_inventory[static_cast<size_t>(slots[i].index)];
          line = std::string(1, letter) + ") " + p.name + " (" + describe_potion(p) + ")";
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
    } else if (mode == Mode::Help) {
      tcod::print(console, {0, 0}, "Controls - '?' or Esc to close", tcod::ColorRGB{255, 255, 255}, std::nullopt);
      static const std::vector<std::string> kHelpLines = {
          "",
          "Arrows / hjkl / yubn (diagonals)  Move; walks into an enemy to attack, or",
          "                                  swaps places with your own minion",
          ".                                 Wait a turn",
          ">  <                              Stairs down/up (must be standing on them)",
          "g                                 Pick up everything on your tile",
          "w  a  q                           Weapon / Armor / Potion menu (equip or drink)",
          "d                                 Drop a weapon, armor, or potion",
          "f                                 Fire the equipped ranged weapon (move to target,",
          "                                  Enter to loose it, Esc to cancel)",
          "z                                 Cast a known spell",
          "m                                 Command a minion or all of them (roster menu; Shift+A",
          "                                  there jumps straight to All)",
          "o  p                              Cycle command focus to the next/previous minion",
          "Shift+P                           Return focus to yourself",
          "f  Enter                          While focused on a minion instead: Follow / confirm",
          "                                  Attack or Hold (sidebar shows each minion's order as",
          "                                  [F]ollow / [H]old / [A]ttack)",
          "x                                 Look around (move the cursor, side panel shows",
          "                                  details); x or Esc to close",
          "]                                 Message log (full scrollback)",
          "Shift+S  Shift+D  Shift+I         On level up: spend the point on STR/DEX/INT",
          "?                                 This screen",
          "Esc                               Quit (or close the current menu)",
      };
      for (size_t i = 0; i < kHelpLines.size(); ++i) {
        tcod::print(console, {0, 1 + static_cast<int>(i)}, kHelpLines[i], tcod::ColorRGB{200, 200, 200},
                    std::nullopt);
      }
    } else if (mode == Mode::MinionRoster) {
      tcod::print(console, {0, 0}, "Command a minion - press a letter, Esc to close", tcod::ColorRGB{255, 255, 255},
                  std::nullopt);
      // "All" is a fixed hotkey (Shift+A) pinned above the roster rather than a letter
      // tacked onto the end of it — a trailing letter shifts around as the pack's size
      // changes (and got long enough with a real attack target named to run off the
      // sidebar in the equivalent per-minion list, see minion_order_flag() above), so
      // anchoring it first keeps the ordering predictable regardless of pack size.
      tcod::print(console, {0, 2}, "Shift+A) All minions at once", tcod::ColorRGB{200, 200, 200}, std::nullopt);
      // Each living minion then gets its own letter, in level.monsters order (stable
      // turn to turn barring a death).
      int row = 4;
      char letter = 'a';
      for (const auto& m : level.monsters) {
        if (m.allegiance != Allegiance::Player || !m.is_alive()) continue;
        std::string line =
            std::string(1, letter) + ") " + m.name + " (" + describe_minion_order(m, level.monsters) + ")";
        tcod::print(console, {0, row}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
        ++row;
        ++letter;
      }
    } else {
      update_monster_memory(level);

      // Row CONTEXT_ROW: a transient action prompt (level-up / spell targeting /
      // minion command) when one of those modes is active. Deliberately separate from
      // the message-log panel below — a long-running prompt shouldn't crowd out or get
      // crowded out by ordinary log messages.
      if (mode == Mode::LevelUp) {
        std::string prompt = "*** LEVEL UP (now level " + std::to_string(player.level) +
                              ")! Press Shift+S/D/I to raise Strength/Dexterity/Intelligence. ***";
        tcod::print(console, {0, CONTEXT_ROW}, prompt, tcod::ColorRGB{255, 255, 100}, std::nullopt);
      } else if (mode == Mode::Targeting) {
        const Spell& casting_spell = kSpellTable[static_cast<size_t>(casting_spell_index)];
        std::string prompt = "Casting " + casting_spell.name + " (" + std::to_string(casting_spell.mana_cost) +
                              " MP) - move to target, Enter to fire, Esc to cancel.";
        tcod::print(console, {0, CONTEXT_ROW}, prompt, tcod::ColorRGB{255, 255, 100}, std::nullopt);
      } else if (mode == Mode::MinionFocus) {
        std::string who = "your minion";
        if (commanding_all_minions) {
          who = "all minions";
        } else {
          int fi = actor_index_by_id(level.monsters, focused_minion_id);
          if (fi >= 0) who = level.monsters[static_cast<size_t>(fi)].name;
        }
        std::string prompt = "Commanding " + who +
                              " - move to a monster (attack), your own tile (follow), or elsewhere "
                              "(hold), Enter to confirm, F to follow, Esc to cancel.";
        tcod::print(console, {0, CONTEXT_ROW}, prompt, tcod::ColorRGB{255, 255, 100}, std::nullopt);
      } else if (mode == Mode::Look) {
        tcod::print(console, {0, CONTEXT_ROW}, "Looking around - move the cursor to inspect, Esc to close.",
                    tcod::ColorRGB{255, 255, 100}, std::nullopt);
      } else if (mode == Mode::RangedAttack) {
        std::string prompt = "Firing your " + player.weapon.name + " - move to target, Enter to fire, Esc to cancel.";
        tcod::print(console, {0, CONTEXT_ROW}, prompt, tcod::ColorRGB{255, 255, 100}, std::nullopt);
      }

      // --- Sidebar: character stats, then who's around ("at a glance") ---
      draw_panel(console, SIDEBAR_X, SIDEBAR_Y, SIDEBAR_W, SIDEBAR_H, "Status");
      int sb_x = SIDEBAR_X + 1;
      int sb_row = SIDEBAR_Y + 1;
      int sb_bottom = SIDEBAR_Y + SIDEBAR_H - 2;  // last usable interior row
      // Appends one line and advances sb_row, silently dropping anything past the
      // panel's interior instead of overflowing into its border. Also clips the text
      // itself to the panel's interior width — the sidebar sits flush against the
      // console's right edge with nothing beyond it, so an unclipped long line (e.g. a
      // minion's status naming a long monster name) would run right off the window
      // instead of just looking crowded.
      int sb_max_width = SIDEBAR_W - 2;
      auto sb_print = [&](const std::string& text, tcod::ColorRGB color) {
        if (sb_row > sb_bottom) return;
        std::string clipped = text;
        if (static_cast<int>(clipped.size()) > sb_max_width) {
          clipped = clipped.substr(0, static_cast<size_t>(std::max(0, sb_max_width - 3))) + "...";
        }
        tcod::print(console, {sb_x, sb_row}, clipped, color, std::nullopt);
        ++sb_row;
      };

      sb_print("HP: " + std::to_string(player.hp) + "/" + std::to_string(player.max_hp),
               tcod::ColorRGB{255, 255, 255});
      sb_print("MP: " + std::to_string(player.mana) + "/" + std::to_string(player.max_mana),
               tcod::ColorRGB{255, 255, 255});
      sb_print("Lvl: " + std::to_string(player.level) + "  Floor: " + std::to_string(current_level + 1),
               tcod::ColorRGB{255, 255, 255});

      // Appends "+N" to a stat only while its temp buff is active, so the HUD reflects
      // Potion of Strength/Dexterity/Intelligence without a separate buff tracker.
      auto stat_str = [](int base, int bonus) {
        return std::to_string(base) + (bonus > 0 ? "+" + std::to_string(bonus) : "");
      };
      sb_print("STR: " + stat_str(player.strength, player.temp_str_bonus), tcod::ColorRGB{200, 200, 200});
      sb_print("DEX: " + stat_str(player.dexterity, player.temp_dex_bonus), tcod::ColorRGB{200, 200, 200});
      sb_print("INT: " + stat_str(player.intelligence, player.temp_int_bonus), tcod::ColorRGB{200, 200, 200});
      sb_print("Wpn: " + player.weapon.name, tcod::ColorRGB{200, 200, 200});
      sb_print("  (" + describe_weapon(player.weapon) + ")", tcod::ColorRGB{150, 150, 150});
      sb_print("Arm: " + player.armor.name, tcod::ColorRGB{200, 200, 200});
      sb_print("  (" + describe_armor(player.armor) + ")", tcod::ColorRGB{150, 150, 150});
      // A running toggle spell (e.g. Sandstorm) has no other on-screen presence besides
      // its aura tile overlay — this tag is the only text indicator it's still active.
      if (active_toggle_spell >= 0) {
        sb_print("[" + kSpellTable[static_cast<size_t>(active_toggle_spell)].name + "]",
                 tcod::ColorRGB{255, 255, 100});
      }

      // Placed ahead of Enemies/Minions (rather than after) so it can never get
      // silently dropped by sb_print's bottom-of-panel clamp when the pack/enemy
      // lists below run long — the thing you're actively examining is the more
      // important thing to keep on screen right now.
      if (mode == Mode::Look) {
        ++sb_row;
        sb_print("Looking:", tcod::ColorRGB{200, 200, 255});
        bool explored = level.map.is_explored(target_x, target_y);
        if (!explored && !reveal_mode) {
          sb_print("  (unexplored)", tcod::ColorRGB{120, 120, 120});
        } else {
          bool tile_in_fov = level.map.is_in_fov(target_x, target_y);
          bool is_stairs_down = (target_x == level.stairs_down_x && target_y == level.stairs_down_y);
          bool is_stairs_up = level.has_stairs_up && (target_x == level.entry_x && target_y == level.entry_y);
          std::string terrain = is_stairs_down    ? "Stairs down"
                                 : is_stairs_up    ? "Stairs up"
                                 : level.map.is_walkable(target_x, target_y) ? "Floor"
                                                                              : "Wall";
          sb_print("  " + terrain, tcod::ColorRGB{200, 200, 200});

          if (!tile_in_fov && !reveal_mode) {
            // Remembered terrain layout is fine to show, but not live occupant
            // details — same rule the map rendering itself already follows (items/
            // monsters only ever show up while actually in view).
            sb_print("  (out of view)", tcod::ColorRGB{120, 120, 120});
          } else {
            bool found_anything = false;
            int mi = monster_at(level.monsters, target_x, target_y);
            if (mi >= 0) {
              const Actor& m = level.monsters[static_cast<size_t>(mi)];
              sb_print("  " + m.name, m.color);
              sb_print("    HP: " + std::to_string(m.hp) + "/" + std::to_string(m.max_hp),
                       tcod::ColorRGB{200, 200, 200});
              sb_print("    Wpn: " + m.weapon.name + " (" + describe_weapon(m.weapon) + ")",
                       tcod::ColorRGB{200, 200, 200});
              if (m.allegiance == Allegiance::Player) {
                sb_print("    " + describe_minion_order(m, level.monsters), tcod::ColorRGB{200, 200, 200});
              }
              found_anything = true;
            }
            for (const auto& gi : level.items) {
              if (gi.x != target_x || gi.y != target_y) continue;
              sb_print("  " + gi.weapon.name + " (" + describe_weapon(gi.weapon) + ")",
                       tcod::ColorRGB{200, 200, 255});
              found_anything = true;
            }
            for (const auto& ga : level.armor_items) {
              if (ga.x != target_x || ga.y != target_y) continue;
              sb_print("  " + ga.armor.name + " (" + describe_armor(ga.armor) + ")", tcod::ColorRGB{180, 220, 200});
              found_anything = true;
            }
            for (const auto& gp : level.potions) {
              if (gp.x != target_x || gp.y != target_y) continue;
              sb_print("  " + gp.potion.name + " (" + describe_potion(gp.potion) + ")", gp.potion.color);
              found_anything = true;
            }
            if (!found_anything) sb_print("  (nothing else here)", tcod::ColorRGB{120, 120, 120});
          }
        }
        ++sb_row;
      }

      sb_print("Enemies:", tcod::ColorRGB{255, 150, 150});
      bool any_enemy = false;
      for (const auto& m : level.monsters) {
        if (m.allegiance != Allegiance::Hostile || !m.is_alive()) continue;
        if (!level.map.is_in_fov(m.x, m.y)) continue;
        sb_print("  " + m.name + " (" + std::to_string(m.hp) + "/" + std::to_string(m.max_hp) + ")", m.color);
        any_enemy = true;
      }
      if (!any_enemy) sb_print("  (none in view)", tcod::ColorRGB{120, 120, 120});

      ++sb_row;
      sb_print("Minions:", tcod::ColorRGB{150, 220, 255});
      bool any_minion = false;
      for (const auto& m : level.monsters) {
        if (m.allegiance != Allegiance::Player || !m.is_alive()) continue;
        bool focused = !commanding_all_minions && m.id == focused_minion_id;
        tcod::ColorRGB color = focused ? tcod::ColorRGB{255, 255, 100} : m.color;
        // [F]ollow / [H]old / [A]ttack — a flag instead of describe_minion_order()'s
        // full sentence, which could run past the sidebar's width once it named an
        // attack target (e.g. "attacking the Goblin Slinger").
        sb_print("  " + std::string(focused ? "*" : " ") + m.name + " [" + minion_order_flag(m) + "]", color);
        any_minion = true;
      }
      if (!any_minion) sb_print("  (none)", tcod::ColorRGB{120, 120, 120});

      // --- Message log panel: always exactly the last MESSAGE_ROWS distinct
      // messages, oldest on top, one per line — never wrapped or combined, even if
      // several things happened on the same turn. (']' opens full scrollback.)
      draw_panel(console, LOG_PANEL_X, LOG_PANEL_Y, LOG_PANEL_W, LOG_PANEL_H, "Log");
      int log_total = static_cast<int>(message_log.size());
      for (int row = 0; row < MESSAGE_ROWS; ++row) {
        int idx = log_total - MESSAGE_ROWS + row;
        if (idx < 0) continue;
        tcod::print(console, {LOG_PANEL_X + 1, LOG_PANEL_Y + 1 + row}, message_log[static_cast<size_t>(idx)],
                    tcod::ColorRGB{255, 255, 100}, std::nullopt);
      }

      // --- Map panel ---
      // Camera: centers on the player, except while aiming or looking around
      // (Targeting/RangedAttack/MinionFocus/Look), where it centers on the cursor
      // instead so a cursor that has wandered away from the player (MinionFocus and
      // Look have no range limit, unlike a spell's Targeting or a weapon's
      // RangedAttack) never drifts off-screen. Clamped so the viewport never scrolls
      // past the map's edge — same clamp-to-bounds shape regardless of which point
      // it's following.
      int camera_focus_x = player.x;
      int camera_focus_y = player.y;
      if (mode == Mode::Targeting || mode == Mode::MinionFocus || mode == Mode::Look ||
          mode == Mode::RangedAttack) {
        camera_focus_x = target_x;
        camera_focus_y = target_y;
      }
      int camera_x = std::clamp(camera_focus_x - MAP_VIEW_W / 2, 0, std::max(0, level.map.width() - MAP_VIEW_W));
      int camera_y = std::clamp(camera_focus_y - MAP_VIEW_H / 2, 0, std::max(0, level.map.height() - MAP_VIEW_H));
      // True for any dungeon tile currently inside the scrolled viewport — every
      // entity draw below skips (rather than writes out of the map panel's bounds,
      // or into the sidebar/log next to it) anything that fails this.
      auto in_view = [&](int x, int y) {
        return x >= camera_x && x < camera_x + MAP_VIEW_W && y >= camera_y && y < camera_y + MAP_VIEW_H;
      };

      draw_panel(console, MAP_PANEL_X, MAP_PANEL_Y, MAP_PANEL_W, MAP_PANEL_H,
                 "Floor " + std::to_string(current_level + 1));

      int view_x_end = std::min(camera_x + MAP_VIEW_W, level.map.width());
      int view_y_end = std::min(camera_y + MAP_VIEW_H, level.map.height());
      for (int y = camera_y; y < view_y_end; ++y) {
        for (int x = camera_x; x < view_x_end; ++x) {
          // Never seen and not revealing: leave blank.
          if (!level.map.is_explored(x, y) && !reveal_mode) continue;

          bool walkable = level.map.is_walkable(x, y);
          bool visible = level.map.is_in_fov(x, y);
          bool is_stairs_down = (x == level.stairs_down_x && y == level.stairs_down_y);
          bool is_stairs_up = level.has_stairs_up && (x == level.entry_x && y == level.entry_y);

          auto& cell = console.at(MAP_ORIGIN_X + x - camera_x, MAP_ORIGIN_Y + y - camera_y);
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
        if (!in_view(remembered.x, remembered.y)) continue;
        auto& cell = console.at(MAP_ORIGIN_X + remembered.x - camera_x, MAP_ORIGIN_Y + remembered.y - camera_y);
        cell.ch = remembered.glyph;
        cell.fg = dim_color(remembered.color);
      }

      // Items/monsters only show up while actually in view, unlike remembered terrain
      // — unless --reveal is forcing them on, in which case out-of-fov ones are drawn
      // dimmed, same tier as remembered terrain/monsters.
      for (const auto& item : level.items) {
        bool visible = level.map.is_in_fov(item.x, item.y);
        if (!visible && !reveal_mode) continue;
        if (!in_view(item.x, item.y)) continue;
        auto& cell = console.at(MAP_ORIGIN_X + item.x - camera_x, MAP_ORIGIN_Y + item.y - camera_y);
        cell.ch = '/';
        tcod::ColorRGB color{200, 200, 255};
        cell.fg = visible ? color : dim_color(color);
      }

      for (const auto& armor_item : level.armor_items) {
        bool visible = level.map.is_in_fov(armor_item.x, armor_item.y);
        if (!visible && !reveal_mode) continue;
        if (!in_view(armor_item.x, armor_item.y)) continue;
        auto& cell = console.at(MAP_ORIGIN_X + armor_item.x - camera_x, MAP_ORIGIN_Y + armor_item.y - camera_y);
        cell.ch = '[';
        tcod::ColorRGB color{180, 220, 200};
        cell.fg = visible ? color : dim_color(color);
      }

      for (const auto& ground_potion : level.potions) {
        bool visible = level.map.is_in_fov(ground_potion.x, ground_potion.y);
        if (!visible && !reveal_mode) continue;
        if (!in_view(ground_potion.x, ground_potion.y)) continue;
        auto& cell =
            console.at(MAP_ORIGIN_X + ground_potion.x - camera_x, MAP_ORIGIN_Y + ground_potion.y - camera_y);
        cell.ch = ground_potion.potion.glyph;
        cell.fg = visible ? ground_potion.potion.color : dim_color(ground_potion.potion.color);
      }

      for (const auto& monster : level.monsters) {
        bool visible = level.map.is_in_fov(monster.x, monster.y);
        if (!visible && !reveal_mode) continue;
        if (!in_view(monster.x, monster.y)) continue;
        auto& cell = console.at(MAP_ORIGIN_X + monster.x - camera_x, MAP_ORIGIN_Y + monster.y - camera_y);
        cell.ch = monster.glyph;
        cell.fg = visible ? monster.color : dim_color(monster.color);
      }

      // Spells currently in flight (only visible ones matter, same as monsters/items).
      for (const auto& proj : level.projectiles) {
        if (proj.path_index == 0 || proj.path_index > proj.path.size()) continue;
        auto [px, py] = proj.path[proj.path_index - 1];
        if (!level.map.is_in_fov(px, py)) continue;
        if (!in_view(px, py)) continue;
        auto& cell = console.at(MAP_ORIGIN_X + px - camera_x, MAP_ORIGIN_Y + py - camera_y);
        cell.ch = proj.glyph;
        cell.fg = proj.color;
      }

      // The player is always inside the viewport by construction (the camera clamp
      // keeps whatever it's centered on in view), so this is drawn unconditionally.
      console.at(MAP_ORIGIN_X + player.x - camera_x, MAP_ORIGIN_Y + player.y - camera_y).ch = player.glyph;
      console.at(MAP_ORIGIN_X + player.x - camera_x, MAP_ORIGIN_Y + player.y - camera_y).fg = player.color;

      // A running toggle spell (e.g. Sandstorm) gets a persistent highlight around the
      // player showing its current radius, recentered every frame since the aura
      // follows the player rather than sitting still — same recolor-not-overwrite
      // treatment as the AoE targeting preview below, so monsters/terrain inside it
      // stay visible. Uses the spell's own color so different toggle spells (if more
      // are ever added) read as visually distinct auras.
      if (active_toggle_spell >= 0) {
        const Spell& storm = kSpellTable[static_cast<size_t>(active_toggle_spell)];
        for (int by = player.y - storm.aoe_radius; by <= player.y + storm.aoe_radius; ++by) {
          for (int bx = player.x - storm.aoe_radius; bx <= player.x + storm.aoe_radius; ++bx) {
            if (bx < 0 || by < 0 || bx >= level.map.width() || by >= level.map.height()) continue;
            if (!level.map.is_explored(bx, by) && !reveal_mode) continue;
            if (!in_view(bx, by)) continue;
            console.at(MAP_ORIGIN_X + bx - camera_x, MAP_ORIGIN_Y + by - camera_y).fg = storm.color;
          }
        }
        // Re-mark the player's own tile on top so they stay visible inside the tint.
        console.at(MAP_ORIGIN_X + player.x - camera_x, MAP_ORIGIN_Y + player.y - camera_y).fg = player.color;
      }

      if (mode == Mode::Targeting) {
        // Preview the shot: trace the same path a cast would take, and stop drawing at
        // the first tile that would actually stop it, so what you see is what you'd hit.
        auto preview = trace_path(player.x, player.y, target_x, target_y);
        for (size_t i = 0; i < preview.size(); ++i) {
          auto [x, y] = preview[i];
          bool blocked = !level.map.is_walkable(x, y);
          bool has_monster = hostile_monster_at(level.monsters, x, y) >= 0;
          bool stops_here = blocked || has_monster || i + 1 == preview.size();
          if (in_view(x, y)) {
            auto& cell = console.at(MAP_ORIGIN_X + x - camera_x, MAP_ORIGIN_Y + y - camera_y);
            cell.ch = stops_here ? 'X' : '*';
            cell.fg = stops_here ? tcod::ColorRGB{255, 60, 60} : tcod::ColorRGB{150, 60, 60};
          }
          if (blocked || has_monster) break;
        }

        // AoE spells (Fireball etc.) also highlight the blast radius around wherever the
        // shot would actually come to rest — find_impact() applies the exact same
        // stopping rules advance_projectiles() uses, so this matches what firing now
        // would do. Recolors tiles rather than overwriting their glyph, so monsters/
        // terrain caught in the blast stay visible underneath the highlight.
        const Spell& previewed_spell = kSpellTable[static_cast<size_t>(casting_spell_index)];
        if (previewed_spell.aoe_radius > 0) {
          auto [impact_x, impact_y] = find_impact(preview, player.x, player.y, level.map, level.monsters);
          int radius = previewed_spell.aoe_radius;
          for (int by = impact_y - radius; by <= impact_y + radius; ++by) {
            for (int bx = impact_x - radius; bx <= impact_x + radius; ++bx) {
              if (bx < 0 || by < 0 || bx >= level.map.width() || by >= level.map.height()) continue;
              if (!level.map.is_explored(bx, by) && !reveal_mode) continue;
              if (!in_view(bx, by)) continue;
              console.at(MAP_ORIGIN_X + bx - camera_x, MAP_ORIGIN_Y + by - camera_y).fg = tcod::ColorRGB{255, 140, 60};
            }
          }
          // Re-mark the impact tile on top so the center stays visually distinct.
          if (in_view(impact_x, impact_y)) {
            auto& impact_cell = console.at(MAP_ORIGIN_X + impact_x - camera_x, MAP_ORIGIN_Y + impact_y - camera_y);
            impact_cell.ch = 'X';
            impact_cell.fg = tcod::ColorRGB{255, 60, 60};
          }
        }
      }

      if (mode == Mode::RangedAttack) {
        // Same aim-preview line as Mode::Targeting above, minus the AoE step — no
        // player weapon has a blast radius today, so there's nothing extra to predict.
        auto preview = trace_path(player.x, player.y, target_x, target_y);
        for (size_t i = 0; i < preview.size(); ++i) {
          auto [x, y] = preview[i];
          bool blocked = !level.map.is_walkable(x, y);
          bool has_monster = hostile_monster_at(level.monsters, x, y) >= 0;
          bool stops_here = blocked || has_monster || i + 1 == preview.size();
          if (in_view(x, y)) {
            auto& cell = console.at(MAP_ORIGIN_X + x - camera_x, MAP_ORIGIN_Y + y - camera_y);
            cell.ch = stops_here ? 'X' : '*';
            cell.fg = stops_here ? tcod::ColorRGB{255, 60, 60} : tcod::ColorRGB{150, 60, 60};
          }
          if (blocked || has_monster) break;
        }
      }

      if (mode == Mode::MinionFocus) {
        // Highlights whichever minion(s) are currently being commanded — once the
        // cursor wanders away from a minion's own tile there's otherwise no way to
        // tell who you're still aiming for. Recolors the glyph (keeps it, rather than
        // overwriting with a marker) so it still reads as "that minion", just lit up.
        for (const auto& m : level.monsters) {
          if (m.allegiance != Allegiance::Player || !m.is_alive()) continue;
          if (!commanding_all_minions && m.id != focused_minion_id) continue;
          bool visible = level.map.is_in_fov(m.x, m.y);
          if (!visible && !reveal_mode) continue;  // not drawn at all this frame either way
          if (!in_view(m.x, m.y)) continue;
          console.at(MAP_ORIGIN_X + m.x - camera_x, MAP_ORIGIN_Y + m.y - camera_y).fg = tcod::ColorRGB{255, 255, 100};
        }

        // If any commanded minion currently has an AttackTarget order, highlight that
        // monster too, in a color distinct from the minion tint above — with more than
        // one of the same monster type in view (two Goblins, say) there's otherwise no
        // way to tell which one is actually assigned versus just standing nearby.
        for (const auto& m : level.monsters) {
          if (m.allegiance != Allegiance::Player || !m.is_alive()) continue;
          if (!commanding_all_minions && m.id != focused_minion_id) continue;
          if (m.order != MinionOrder::AttackTarget) continue;
          int ti = actor_index_by_id(level.monsters, m.attack_target_id);
          if (ti < 0) continue;  // target died/gone; the minion will revert to Follow on its own
          const Actor& target = level.monsters[static_cast<size_t>(ti)];
          bool target_visible = level.map.is_in_fov(target.x, target.y);
          if (!target_visible && !reveal_mode) continue;
          if (!in_view(target.x, target.y)) continue;
          console.at(MAP_ORIGIN_X + target.x - camera_x, MAP_ORIGIN_Y + target.y - camera_y).fg =
              tcod::ColorRGB{255, 60, 255};
        }

        // No line trace or AoE like a spell — confirming here either attacks (a
        // hostile monster under the cursor) or holds (any other walkable tile), see
        // the Enter handler below, so the cursor color previews which one: red for
        // attack (the monster's own glyph stays visible, just tinted, same as the
        // spell-targeting cursor above), green for hold, dim grey for an invalid tile
        // (a wall, or something already standing there that isn't a valid target). The
        // camera follows this cursor (see camera_focus_x/y above) so it's always in
        // view, unlike a spell's range-limited targeting cursor.
        if (in_view(target_x, target_y)) {
          auto& cell = console.at(MAP_ORIGIN_X + target_x - camera_x, MAP_ORIGIN_Y + target_y - camera_y);
          bool walkable = level.map.is_walkable(target_x, target_y);
          int hostile_hit = hostile_monster_at(level.monsters, target_x, target_y);
          if (hostile_hit >= 0) {
            cell.fg = tcod::ColorRGB{255, 60, 60};
          } else if (walkable && monster_at(level.monsters, target_x, target_y) < 0) {
            cell.ch = 'X';
            cell.fg = tcod::ColorRGB{100, 220, 140};
          } else {
            cell.ch = 'X';
            cell.fg = tcod::ColorRGB{120, 120, 120};
          }
        }
      }

      if (mode == Mode::Look) {
        // Plain recolor, no glyph override — unlike Targeting/MinionFocus there's no
        // action being previewed here, just "this is what the cursor is on", so
        // whatever's actually there (terrain/item/monster) should stay fully visible.
        if (in_view(target_x, target_y)) {
          console.at(MAP_ORIGIN_X + target_x - camera_x, MAP_ORIGIN_Y + target_y - camera_y).fg =
              tcod::ColorRGB{255, 255, 255};
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

      // Moves command focus to the next (direction=+1) or previous (direction=-1)
      // living minion, in level.monsters order, wrapping around; if focused_minion_id
      // doesn't currently name a living minion (nothing focused yet, or it died),
      // starts from the first (next) or last (prev) instead of wrapping relative to a
      // missing position. Always lands on one specific minion — never "all" — and
      // points the cursor at its current position. Returns false (no-op) if there are
      // no minions at all. Shared by the 'o'/'p' trigger keys from Mode::Playing and
      // by the same keys working *inside* Mode::MinionFocus too, so you can tab
      // straight from planning one minion's order to the next without dropping back
      // to normal play in between — the "turn planner" feel this whole system is for.
      auto cycle_minion_focus = [&](int direction) -> bool {
        std::vector<int> minion_ids;
        for (const auto& m : level.monsters) {
          if (m.allegiance == Allegiance::Player && m.is_alive()) minion_ids.push_back(m.id);
        }
        if (minion_ids.empty()) return false;
        int current = -1;
        for (size_t i = 0; i < minion_ids.size(); ++i) {
          if (minion_ids[i] == focused_minion_id) {
            current = static_cast<int>(i);
            break;
          }
        }
        int next_index;
        if (current < 0) {
          next_index = direction >= 0 ? 0 : static_cast<int>(minion_ids.size()) - 1;
        } else {
          next_index = (current + direction + static_cast<int>(minion_ids.size())) %
                       static_cast<int>(minion_ids.size());
        }
        focused_minion_id = minion_ids[static_cast<size_t>(next_index)];
        commanding_all_minions = false;
        int fi = actor_index_by_id(level.monsters, focused_minion_id);
        target_x = level.monsters[static_cast<size_t>(fi)].x;
        target_y = level.monsters[static_cast<size_t>(fi)].y;
        return true;
      };

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

      if (mode == Mode::Help) {
        // Same unshifted-keycode-plus-modifier check the stairs keys use below, since
        // '?' is Shift+/ on a US layout.
        bool pressed_question =
            event.key.key == SDLK_QUESTION || (event.key.key == SDLK_SLASH && (event.key.mod & SDL_KMOD_SHIFT));
        if (event.key.key == SDLK_ESCAPE || pressed_question) mode = Mode::Playing;
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
          int new_max_hp = max_hp_for_level_and_strength(player.level, player.strength);
          player.hp += new_max_hp - player.max_hp;
          player.max_hp = new_max_hp;
          add_message("Strength increased to " + std::to_string(player.strength) + "!");
          pending_attribute_points -= 1;
        } else if (shift_held && event.key.key == SDLK_D) {
          player.dexterity += 1;
          add_message("Dexterity increased to " + std::to_string(player.dexterity) + "!");
          pending_attribute_points -= 1;
        } else if (shift_held && event.key.key == SDLK_I) {
          auto known_before = known_spell_indices(player.intelligence);
          player.intelligence += 1;
          auto known_after = known_spell_indices(player.intelligence);
          int new_max_mana = max_mana_for_intelligence(player.intelligence);
          player.mana += new_max_mana - player.max_mana;
          player.max_mana = new_max_mana;
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

      if (mode == Mode::PotionMenu) {
        if (event.key.key == SDLK_ESCAPE) {
          mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          if (idx < potion_inventory.size()) {
            Potion chosen = potion_inventory[idx];
            potion_inventory.erase(potion_inventory.begin() + static_cast<long>(idx));
            if (chosen.heal_percent > 0) {
              int heal_amount = player.max_hp * chosen.heal_percent / 100;
              player.hp = std::min(player.hp + heal_amount, player.max_hp);
              add_message("You drink the " + chosen.name + " and recover " + std::to_string(heal_amount) + " HP.");
            } else if (chosen.buff_stat == StatKind::Strength) {
              // Re-drinking while already buffed just refreshes the timer, rather than
              // stacking the bonus indefinitely.
              if (player.temp_str_turns <= 0) {
                player.temp_str_bonus = chosen.buff_amount;
                player.max_hp = max_hp_for_level_and_strength(player.level, player.strength + player.temp_str_bonus);
                // Ceiling only, unlike leveling up: current HP doesn't jump with it.
              }
              player.temp_str_turns = chosen.buff_turns;
              add_message("You feel mighty! STR +" + std::to_string(chosen.buff_amount) + " for " +
                          std::to_string(chosen.buff_turns) + " turns.");
            } else if (chosen.buff_stat == StatKind::Dexterity) {
              if (player.temp_dex_turns <= 0) player.temp_dex_bonus = chosen.buff_amount;
              player.temp_dex_turns = chosen.buff_turns;
              add_message("You feel nimble! DEX +" + std::to_string(chosen.buff_amount) + " for " +
                          std::to_string(chosen.buff_turns) + " turns.");
            } else if (chosen.buff_stat == StatKind::Intelligence) {
              if (player.temp_int_turns <= 0) {
                player.temp_int_bonus = chosen.buff_amount;
                player.max_mana = max_mana_for_intelligence(player.intelligence + player.temp_int_bonus);
                // Ceiling only, unlike leveling up: current mana doesn't jump with it.
              }
              player.temp_int_turns = chosen.buff_turns;
              add_message("You feel sharp! INT +" + std::to_string(chosen.buff_amount) + " for " +
                          std::to_string(chosen.buff_turns) + " turns.");
            } else if (chosen.teleports) {
              std::vector<std::pair<int, int>> occupied;
              for (const auto& m : level.monsters) occupied.push_back({m.x, m.y});
              auto [tx, ty] = random_free_tile(level.map, occupied);
              player.x = tx;
              player.y = ty;
              // Not an incremental step, so (unlike normal movement) FOV needs an
              // explicit recompute — same as descend()/ascend() after a floor change.
              level.map.update_fov(player.x, player.y, FOV_RADIUS);
              add_message("You vanish and reappear elsewhere!");
            }
            mode = Mode::Playing;
            end_turn();  // drinking takes a moment; adjacent monsters get a free hit
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
            int spell_idx = known[idx];
            const Spell& spell = kSpellTable[static_cast<size_t>(spell_idx)];
            if (spell.is_toggle) {
              if (active_toggle_spell == spell_idx) {
                // Turning off is always free — no mana cost, but still takes the turn,
                // same as every other spell-menu action.
                active_toggle_spell = -1;
                add_message("Your " + spell.name + " dissipates.");
                mode = Mode::Playing;
                end_turn();
              } else if (player.mana < spell.mana_cost) {
                add_message("Not enough mana to cast " + spell.name + ".");
                mode = Mode::Playing;  // free cancel, no turn spent
              } else {
                player.mana -= spell.mana_cost;
                add_message("You summon a " + spell.name + " around yourself!");
                mode = Mode::Playing;
                end_turn();  // this turn only pays the flat activation cost above
                active_toggle_spell = spell_idx;  // set after end_turn(), so the
                                                   // per-turn tick starts next turn
              }
            } else if (spell.is_summon) {
              int spawn_x, spawn_y;
              if (count_minions(level.monsters) >= kMaxMinions) {
                add_message("You can't command any more minions right now.");
                mode = Mode::Playing;  // free cancel, no turn spent
              } else if (player.mana < spell.mana_cost) {
                add_message("Not enough mana to cast " + spell.name + ".");
                mode = Mode::Playing;  // free cancel, no turn spent
              } else if (!free_adjacent_tile(level.map, level.monsters, player.x, player.y, spawn_x, spawn_y)) {
                add_message("There's no room to summon here!");
                mode = Mode::Playing;  // free cancel, no turn spent
              } else {
                player.mana -= spell.mana_cost;
                const MinionTemplate& tmpl = kMinionTable[static_cast<size_t>(spell.summon_template_index)];
                Actor minion;
                minion.id = allocate_actor_id();
                minion.allegiance = Allegiance::Player;
                minion.x = spawn_x;
                minion.y = spawn_y;
                minion.hp = minion.max_hp = tmpl.max_hp;
                minion.glyph = tmpl.glyph;
                minion.color = tmpl.color;
                minion.name = tmpl.name;
                minion.weapon = tmpl.weapon;
                minion.evasion = tmpl.evasion;
                minion.dexterity = tmpl.accuracy;  // read by dodge_chance_vs(), same as a hostile monster
                minion.attack_range = tmpl.attack_range;
                minion.melee_weapon = tmpl.melee_weapon;
                minion.melee_dexterity = tmpl.melee_accuracy;
                minion.duration_turns = tmpl.duration_turns;
                // Joins the pack's current stance rather than always defaulting to
                // Follow — if an existing minion is already off attacking something,
                // the new recruit should too, not stand around while its allies
                // fight (orders are pack-wide in Phase 1, see the `m` menu).
                minion.order = MinionOrder::Follow;
                for (const auto& existing : level.monsters) {
                  if (existing.allegiance == Allegiance::Player && existing.is_alive() &&
                      existing.order == MinionOrder::AttackTarget) {
                    minion.order = MinionOrder::AttackTarget;
                    minion.attack_target_id = existing.attack_target_id;
                    break;
                  }
                }
                level.monsters.push_back(minion);
                add_message("You raise a " + tmpl.name + " to fight for you!");
                mode = Mode::Playing;
                end_turn();
              }
            } else {
              casting_spell_index = spell_idx;
              target_x = player.x;
              target_y = player.y;
              mode = Mode::Targeting;
            }
          }
        }
        continue;
      }

      if (mode == Mode::MinionRoster) {
        if (event.key.key == SDLK_ESCAPE) {
          mode = Mode::Playing;
          continue;
        }
        if (event.key.key == SDLK_A && (event.key.mod & SDL_KMOD_SHIFT) != 0) {
          // Shift+A always means "All", regardless of which letter it actually landed
          // on this frame (that shifts with the pack's current size) — a fast path so
          // you don't have to read the list to find the right letter every time.
          commanding_all_minions = true;
          target_x = player.x;
          target_y = player.y;
          mode = Mode::MinionFocus;
          continue;
        }
        if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          // Same ordering as the roster's render: a letter per living minion, in
          // level.monsters order ("All" is the fixed Shift+A hotkey above, not a
          // letter in this range — see the check above this one).
          std::vector<int> minion_ids;
          for (const auto& m : level.monsters) {
            if (m.allegiance == Allegiance::Player && m.is_alive()) minion_ids.push_back(m.id);
          }
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          if (idx < minion_ids.size()) {
            focused_minion_id = minion_ids[idx];
            commanding_all_minions = false;
            int fi = actor_index_by_id(level.monsters, focused_minion_id);
            target_x = level.monsters[static_cast<size_t>(fi)].x;
            target_y = level.monsters[static_cast<size_t>(fi)].y;
            mode = Mode::MinionFocus;
          }
        }
        continue;
      }

      if (mode == Mode::MinionFocus) {
        // Applies `fn` to every currently-commanded minion — all of them if this
        // session came from the roster's "All", otherwise just the one named by
        // focused_minion_id. Shared by F (Follow) and Enter (Attack/Hold) below so
        // the "who does this apply to" logic can't drift between the two.
        auto for_each_commanded_minion = [&](auto&& fn) {
          int count = 0;
          if (commanding_all_minions) {
            for (auto& m : level.monsters) {
              if (m.allegiance == Allegiance::Player && m.is_alive()) {
                fn(m);
                ++count;
              }
            }
          } else {
            int fi = actor_index_by_id(level.monsters, focused_minion_id);
            if (fi >= 0) {
              fn(level.monsters[static_cast<size_t>(fi)]);
              count = 1;
            }
          }
          return count;
        };

        bool shift_held = (event.key.mod & SDL_KMOD_SHIFT) != 0;
        if (event.key.key == SDLK_ESCAPE || (event.key.key == SDLK_P && shift_held)) {
          // Esc just backs out of this one planning action; Shift+P additionally
          // resets cycle position, so the next 'o'/'p' starts over from the top —
          // "focusing back on the player instantly."
          if (event.key.key == SDLK_P) focused_minion_id = -1;
          commanding_all_minions = false;
          mode = Mode::Playing;
          continue;
        }
        if (event.key.key == SDLK_O || (event.key.key == SDLK_P && !shift_held)) {
          // Tab straight to the next/previous minion without dropping back to normal
          // play in between — plan one, tab, plan the next, same as 'o'/'p' do from
          // Mode::Playing (see cycle_minion_focus above), just without leaving this mode.
          if (!cycle_minion_focus(event.key.key == SDLK_O ? 1 : -1)) {
            add_message("You have no minions to command.");
            mode = Mode::Playing;
          }
          continue;
        }
        if (event.key.key == SDLK_F) {
          int ordered = for_each_commanded_minion([](Actor& m) { m.order = MinionOrder::Follow; });
          add_message(ordered == 1 ? "Your minion returns to your side." : "Your minions return to your side.");
          commanding_all_minions = false;
          mode = Mode::Playing;
          continue;
        }
        if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
          int hostile_hit = hostile_monster_at(level.monsters, target_x, target_y);
          if (hostile_hit >= 0) {
            int target_id = level.monsters[static_cast<size_t>(hostile_hit)].id;
            std::string target_name = level.monsters[static_cast<size_t>(hostile_hit)].name;
            int ordered = for_each_commanded_minion([&](Actor& m) {
              m.order = MinionOrder::AttackTarget;
              m.attack_target_id = target_id;
            });
            add_message((ordered == 1 ? "Your minion attacks the " : "Your minions attack the ") + target_name +
                        "!");
            commanding_all_minions = false;
            mode = Mode::Playing;
            continue;
          }
          if (target_x == player.x && target_y == player.y) {
            // Targeting yourself reads as "come back to me" — the same Follow order
            // 'F' gives directly, just reachable without moving the cursor off your
            // own tile first. (Previously this fell through to the tile_free/Hold
            // check below, which — since the player isn't in level.monsters and thus
            // invisible to monster_at() — would silently issue a Hold planted on the
            // player's exact tile instead of anything resembling a rejection.)
            int ordered = for_each_commanded_minion([](Actor& m) { m.order = MinionOrder::Follow; });
            add_message(ordered == 1 ? "Your minion returns to your side." : "Your minions return to your side.");
            commanding_all_minions = false;
            mode = Mode::Playing;
            continue;
          }
          bool tile_free = level.map.is_walkable(target_x, target_y) &&
                            monster_at(level.monsters, target_x, target_y) < 0;
          if (tile_free) {
            int hx = target_x;
            int hy = target_y;
            int ordered = for_each_commanded_minion([&](Actor& m) {
              m.order = MinionOrder::Hold;
              m.hold_x = hx;
              m.hold_y = hy;
            });
            add_message(ordered == 1 ? "Your minion holds position." : "Your minions hold position.");
            commanding_all_minions = false;
            mode = Mode::Playing;
            continue;
          }
          add_message("You can't send them there.");
          continue;  // stay in this mode, no turn spent — try again
        }

        // Movement keys move the cursor — unlike a spell's Targeting, there's no
        // range limit here (a minion will path however far it needs to), just the
        // map bounds.
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
          if (level.map.in_bounds(nx, ny)) {
            target_x = nx;
            target_y = ny;
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
          if (player.mana < spell.mana_cost) {
            add_message("Not enough mana to cast " + spell.name + ".");
            mode = Mode::Playing;  // free cancel, same as Esc — no turn spent
            continue;
          }

          // Any tile is a legal target now: the spell travels and resolves against
          // whatever (if anything) it actually reaches, not necessarily the cursor tile.
          Projectile proj;
          proj.path = trace_path(player.x, player.y, target_x, target_y);
          proj.speed = spell.speed;
          proj.dice_count = spell.dice_count;
          proj.dice_sides = spell.dice_sides;
          proj.hit_dice_count = spell.hit_dice_count;
          proj.hit_dice_sides = spell.hit_dice_sides;
          proj.aoe_radius = spell.aoe_radius;
          proj.prev_x = player.x;  // seeds the "last open tile" for an immediate wall hit
          proj.prev_y = player.y;
          // Locked in now, not re-read when it lands. Temporary INT (from a Potion of
          // Intelligence) boosts this the same as permanent INT would — only spell
          // *unlocking* (known_spell_indices, above) ignores the temporary bonus.
          proj.bonus = (player.intelligence + player.temp_int_bonus) / 3;
          proj.name = spell.name;
          proj.glyph = spell.glyph;
          proj.color = spell.color;
          level.projectiles.push_back(proj);
          player.mana -= spell.mana_cost;

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

      if (mode == Mode::RangedAttack) {
        const Weapon& weapon = player.weapon;

        if (event.key.key == SDLK_ESCAPE) {
          mode = Mode::Playing;
          continue;
        }
        if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
          // Same shape as a spell cast (Mode::Targeting above), just sourced from the
          // weapon instead of a Spell, no mana cost, and the accuracy/damage bonus is
          // Dexterity-derived instead of Intelligence-derived (see monster_dodge_chance()
          // and Projectile::accuracy_bonus) — every 3 points of Dexterity adds +1 to
          // both, same divisor Intelligence already uses for spell damage.
          Projectile proj;
          proj.path = trace_path(player.x, player.y, target_x, target_y);
          proj.speed = kInstantSpellSpeed;
          proj.dice_count = weapon.dice_count;
          proj.dice_sides = weapon.dice_sides;
          proj.hit_dice_count = weapon.hit_dice_count;
          proj.hit_dice_sides = weapon.hit_dice_sides;
          proj.prev_x = player.x;
          proj.prev_y = player.y;
          int dex_bonus = (player.dexterity + player.temp_dex_bonus) / 3;
          proj.bonus = weapon.bonus + dex_bonus;
          proj.accuracy_bonus = dex_bonus;
          proj.name = weapon.name;
          proj.glyph = '-';
          proj.color = tcod::ColorRGB{200, 170, 100};
          level.projectiles.push_back(proj);

          add_message("You fire your " + weapon.name + ".");
          mode = Mode::Playing;
          end_turn();
          continue;
        }

        // Movement keys move the targeting cursor instead of the player, clamped to
        // the weapon's own range — same circular-radius shape a spell's Targeting uses.
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
          bool in_range = rdx * rdx + rdy * rdy <= weapon.attack_range * weapon.attack_range;
          if (level.map.in_bounds(nx, ny) && in_range) {
            target_x = nx;
            target_y = ny;
          }
        }
        continue;
      }

      if (mode == Mode::Look) {
        if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_X) {
          mode = Mode::Playing;
          continue;
        }

        // Movement keys move the cursor — no range limit and no confirm action, just
        // look around and back out with Esc/'x' (no turn spent either way).
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
          if (level.map.in_bounds(nx, ny)) {
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
          auto slots = drop_slots(player, inventory, armor_inventory, potion_inventory);
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
            } else if (slot.kind == ItemKind::Armor) {
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
            } else {
              size_t inv_idx = static_cast<size_t>(slot.index);
              Potion dropped = potion_inventory[inv_idx];
              potion_inventory.erase(potion_inventory.begin() + static_cast<long>(inv_idx));
              level.potions.push_back(GroundPotion{player.x, player.y, dropped});
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
      if (event.key.key == SDLK_Q) {
        mode = Mode::PotionMenu;
        continue;
      }
      if (event.key.key == SDLK_G) {
        // Picks up whatever's on the player's current tile (no more auto-pickup on
        // step). Picks up one of each kind if several happen to be here, each as its
        // own message rather than one combined line.
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
        for (auto it = level.potions.begin(); it != level.potions.end(); ++it) {
          if (it->x == player.x && it->y == player.y) {
            add_message("You pick up a " + it->potion.name + ". Press 'q' to drink.");
            potion_inventory.push_back(it->potion);
            level.potions.erase(it);
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
      if (event.key.key == SDLK_F) {
        // Fire the equipped weapon at range — only meaningful for a ranged weapon
        // (Weapon::attack_range > 1, e.g. Bow); a melee weapon still only attacks by
        // bumping into an adjacent monster.
        if (player.weapon.attack_range <= 1) {
          add_message("Your " + player.weapon.name + " isn't a ranged weapon.");
        } else {
          target_x = player.x;
          target_y = player.y;
          mode = Mode::RangedAttack;
        }
        continue;
      }
      if (event.key.key == SDLK_M) {
        if (count_minions(level.monsters) == 0) {
          add_message("You have no minions to command.");
        } else {
          mode = Mode::MinionRoster;
        }
        continue;
      }
      // 'o'/'p' cycle command focus straight to the next/previous minion (skipping
      // the roster menu — a faster path for the same thing), landing in
      // Mode::MinionFocus with the cursor on that minion. Shift+P resets focus
      // without opening anything — see Mode::MinionFocus's own handling of these
      // same keys for tabbing between minions without leaving that mode in between.
      if (event.key.key == SDLK_O || event.key.key == SDLK_P) {
        bool shift_held = (event.key.mod & SDL_KMOD_SHIFT) != 0;
        if (event.key.key == SDLK_P && shift_held) {
          focused_minion_id = -1;
          continue;
        }
        if (!cycle_minion_focus(event.key.key == SDLK_O ? 1 : -1)) {
          add_message("You have no minions to command.");
        } else {
          mode = Mode::MinionFocus;
        }
        continue;
      }
      if (event.key.key == SDLK_RIGHTBRACKET) {
        mode = Mode::MessageLog;
        log_scroll = 0;  // always open showing the most recent messages
        continue;
      }
      if (event.key.key == SDLK_X) {
        // Starts the look cursor on the player's own tile, same as Targeting/
        // MinionFocus do — free to open/close, no turn spent either way.
        mode = Mode::Look;
        target_x = player.x;
        target_y = player.y;
        continue;
      }
      // '?' is Shift+/ on a US layout, so check both the dedicated keycode and the
      // unshifted one with the modifier set — same pattern the stairs keys use below.
      if (event.key.key == SDLK_QUESTION ||
          (event.key.key == SDLK_SLASH && (event.key.mod & SDL_KMOD_SHIFT))) {
        mode = Mode::Help;
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

      if (target_index >= 0 && level.monsters[static_cast<size_t>(target_index)].allegiance == Allegiance::Player) {
        // Bump into your own minion: swap places instead of attacking it — you're
        // squeezing past an ally, not fighting one.
        Actor& minion = level.monsters[static_cast<size_t>(target_index)];
        std::swap(player.x, minion.x);
        std::swap(player.y, minion.y);
        level.map.update_fov(player.x, player.y, FOV_RADIUS);
        end_turn();
      } else if (target_index >= 0) {
        // Bump attack: walking into a monster attacks it instead of moving.
        Actor& target = level.monsters[static_cast<size_t>(target_index)];

        int dodge_chance = monster_dodge_chance(target.evasion, player.weapon.hit_dice_count, player.weapon.hit_dice_sides);
        if (random_int(1, 100) <= dodge_chance) {
          add_message("The " + target.name + " dodges your attack!");
        } else {
          int damage = roll_damage(player.weapon) + player.strength + player.temp_str_bonus;
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

#pragma once

// The game's numbers: every tuning constant, every derived-stat formula, and the one
// accuracy/dodge/damage math that all combat runs through.
//
// Nothing here touches game state — these are pure functions over an Actor and a Weapon.
// Balance changes should be possible by editing this file and content.cpp alone.

#include "entity.hpp"

// --- Derived stats -------------------------------------------------------------------
//
// The player's ceilings are computed from their attributes by the three functions below;
// a monster's are authored directly in its kMonsterTable row. Same fields, same formulas
// downstream — only where the number comes from differs (see entity.hpp's Actor notes on
// derived vs. authored).

// Max HP scales with both level and Strength — no separate Vitality stat. Leveling up
// alone (regardless of which attribute the point actually goes to) grants a flat
// kHpPerLevel per level past the first, so HP always grows with depth/XP even across a
// run that never touches Strength; Strength then adds even more on top per point
// invested (kHpPerStrength > kHpPerLevel), since it's the stat actually themed around
// survivability. (level - 1) so a fresh level-1 character's starting HP is unaffected —
// the level term only kicks in once they've actually leveled up at least once.
constexpr int kHpPerLevel = 3;
constexpr int kHpPerStrength = 7;
int max_hp_for_level_and_strength(int level, int strength);

// Max mana scales with Intelligence only, not level, unlike
// max_hp_for_level_and_strength above — mana costs are small (1-3 per cast, see
// kSpellTable) so the pool only needs to be a handful of casts deep, not hundreds of
// points.
int max_mana_for_intelligence(int intelligence);

// The player's evasion rating, derived from Dexterity — the exact counterpart of
// max_hp_for_level_and_strength() deriving their HP from Strength. A monster's evasion
// is authored in its table row instead (see MonsterTemplate::evasion); both end up in
// the same Actor::evasion field feeding the same formula below.
int evasion_for_dexterity(int dexterity);

// How many actions this Actor gets per world turn: one, plus whatever its row authored
// (Monster/MinionTemplate::extra_actions) and whatever a Haste-style buff is currently
// granting. The single place those two sources are combined — read it rather than
// touching either field directly, exactly as dexterity is always read together with
// temp_dex_bonus. Clamped at 1 so a hypothetical negative bonus can't stall an Actor
// out of acting entirely.
int total_actions_for(const Actor& actor);

// XP required to advance from the given level to the next one.
int xp_needed_for_level(int level);

// --- Regeneration and AI thresholds --------------------------------------------------

// Turns for the *player's* passive HP regen to heal from 0 to full — assigned to
// player.hp_regen_turns in start_new_game(). Regen scales with max HP (see tick_upkeep())
// so this stays constant regardless of build: a tankier character heals more HP per
// turn, but takes the same number of turns to fully recover. Monsters and minions have
// their own per-row hp_regen_turns, left at 0 (no regen) on every table row but the Orc
// Warlord's.
constexpr int kHpRegenTurns = 150;

// Same idea as kHpRegenTurns, but for mana. Independent constant so the two regen rates
// can be tuned separately later.
constexpr int kManaRegenTurns = 150;

// How badly hurt a monster/minion has to be before it spends a turn drinking a healing
// (or escape) potion it's carrying — see try_actor_use_potion(). The player has no
// equivalent threshold because they decide for themselves.
constexpr int kAiDrinkHealBelowPercent = 45;

// How close an enemy has to be before a monster/minion decides a fight is on and pops a
// combat buff potion (the Orc Warlord's Potion of Strength). Slightly wider than any
// melee reach so it drinks as you close rather than after you're already hitting it.
constexpr int kAiBuffPotionRange = 4;

// --- One combat formula, used by every attack in the game ----------------------------
//
// This replaced three separate ones (a Dexterity contest for monster-vs-player, an
// evasion-minus-hit-dice roll for player-vs-monster, and a third variant for
// minion-vs-monster). There is now exactly one question — "how likely is the defender
// to dodge this?" — and one answer:
//
//     dodge% = kDodgeBaseline + defender's evasion rating - attacker's accuracy roll
//
// Both sides of that come from plain Actor/Weapon fields that mean the same thing on
// everybody, which is what makes a monster and the player balanceable against each
// other: raising a monster's evasion by 8 is worth exactly as much as giving the player
// one more point of Dexterity.
//
// The two per-point knobs are deliberately different sizes, and that asymmetry is
// between *offense and defense*, not between player and monster — everyone uses both:
//   - kDodgePerDexPoint (defense): what one point of Dexterity is worth as evasion.
//   - kAccuracyPerDexPoint (offense): what one point of Dexterity is worth as accuracy.
// The floor/ceiling keep either side from ever reaching a guaranteed hit or a
// guaranteed miss, however lopsided the matchup.
constexpr int kDodgePerDexPoint = 8;
constexpr int kAccuracyPerDexPoint = 4;
constexpr int kDodgeBaseline = 10;
constexpr int kDodgeFloor = 5;
constexpr int kDodgeCeiling = 85;

// What an attacker contributes to landing a hit: their Dexterity plus a roll of
// whatever they're swinging/casting. The weapon (or spell) hit-dice is the "this
// particular attack is hard to dodge" term — a fast Dagger or a wide Fireball rolls
// high, a heavy Battle Axe or a lobbed Rock rolls low.
int accuracy_roll(const Actor& attacker, int hit_dice_count, int hit_dice_sides);

// Percent chance the defender dodges entirely, given an already-rolled accuracy. Split
// out from dodge_chance() below so a Projectile — which locks its caster's accuracy in
// at cast time and only meets its target several turns later — can use the same math.
int dodge_chance_vs_accuracy(const Actor& defender, int accuracy);

// Percent chance `defender` dodges `attacker` swinging `weapon`. Every melee/ranged
// attack in the game — player, hostile monster, or minion, in any combination — goes
// through this one call.
int dodge_chance(const Actor& defender, const Actor& attacker, const Weapon& weapon);

// Flat damage an attacker adds on top of their weapon's dice. Melee scales with
// Strength, ranged with Dexterity (the same /3 rate the Bow already used) — the
// distinction is the weapon's, not the wielder's, so a Troll swinging a club and the
// player swinging a club both get Strength, and an Orc Archer and the player both get
// Dexterity out of a bow.
int damage_bonus_for(const Actor& attacker, const Weapon& weapon);

// Average damage this Actor would do with this weapon, used only to rank the weapons
// it's carrying (see equip_best_weapon_for_range) — never for actual damage, which is
// always rolled.
double expected_damage(const Actor& actor, const Weapon& weapon);

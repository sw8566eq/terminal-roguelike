#pragma once

// The spell pool: one Spell struct broad enough to describe every "kind" of spell the
// game has, one table of rows, and the filter deciding which of them the player knows.
//
// There is no Spell subclassing — a spell's kind is a set of bool flags, and the code
// that resolves a cast branches on those. Six kinds exist today:
//   - a fired Projectile (Magic Dart, Energy Lance, Fireball, Lightning Bolt)
//   - a toggled aura        (is_toggle:      Sandstorm)
//   - a summon              (is_summon:      Raise Skeleton, Summon Demon)
//   - a place swap          (is_swap:        Place Swap)
//   - a self-buff           (is_melee_buff / is_armor_buff / is_haste_buff)
// Fields not relevant to a given kind are simply left at their defaults.

#include <libtcod.hpp>

#include <string>
#include <vector>

#include "entity.hpp"

// Tiles a projectile travels per player turn, high enough to always cross the whole map
// in the turn it's cast. "Instant" is just this speed, not a special case — the same
// advance_projectiles() machinery moves a fast spell and a slow one.
constexpr int kInstantSpellSpeed = 99;  // safely more tiles than this map's diagonal

// A spell. Damage is dice_count dice of dice_sides each, plus floor(INT/3). Known
// automatically once the player's Intelligence reaches unlock_int and their school
// matches — nothing to learn or pick up, it just unlocks. (Thresholds are placeholders
// until the rest of the spell pool is designed.)
struct Spell {
  std::string name;
  int unlock_int;
  int dice_count;
  int dice_sides;
  // Tiles traveled per player turn, not real time — there's no animation. "Instant" is
  // kInstantSpellSpeed; slower spells (e.g. Fireball) visibly take multiple turns to
  // reach a distant target.
  int speed;
  int range;  // max cast distance from the caster, in tiles (straight-line)
  int mana_cost;
  // 0 means the spell only ever hits the single tile/monster it collides with (Magic
  // Dart). A positive radius means it explodes on impact into a square blast of that
  // Chebyshev radius (1 = 3x3) centered on wherever it stopped, damaging every monster
  // caught inside — see advance_projectiles() for how "wherever it stopped" is
  // determined (wall/monster/max-range all count). For a toggle spell, this instead
  // sizes the aura around the player — radius R covers a (2R+1)x(2R+1) box — recentered
  // every turn.
  int aoe_radius = 0;
  // A persistent aura (Sandstorm) instead of a fired Projectile: selecting it in the
  // spell menu turns it on/off directly, no Targeting step (it's always centered on the
  // player). dice_count/dice_sides/speed/range/glyph/color are unused for these —
  // mana_cost instead becomes the flat one-time cost to turn it on, and tick_damage/
  // tick_mana_cost are the flat (no dice, no INT bonus) per-turn effect/cost while it
  // stays active.
  bool is_toggle = false;
  int tick_damage = 0;     // toggle spells only: flat damage/turn to everything in range
  int tick_mana_cost = 0;  // toggle spells only: flat mana drained/turn while active
  // This spell's accuracy against a defender's evasion, same shape and same
  // dodge_chance() formula as Weapon::hit_dice_count/sides — a bigger roll is harder to
  // dodge. An AoE spell should generally roll much higher than a precise single-target
  // one: "wide" is its own kind of hard-to-dodge, same as "fast" is for a weapon.
  int hit_dice_count;
  int hit_dice_sides;
  char glyph;
  tcod::ColorRGB color;
  // Trailing (with defaults) so existing rows don't need updating when a new kind is
  // added — same convention as Weapon::hit_dice_count/sides.
  //
  // A summon immediately spawns a copy of kMinionTable[summon_template_index] as a new
  // Allegiance::Player Actor next to the player — no Targeting step, same "resolves
  // right from the menu" shape as a toggle's on/off, just a one-time effect instead of a
  // persistent aura. dice_count/dice_sides/speed/range/aoe_radius/hit_dice/glyph/color
  // are unused; mana_cost is what it costs to cast, same field every other spell uses.
  bool is_summon = false;
  int summon_template_index = 0;  // summon spells only: row into kMinionTable
  // A swap instantly trades places with one of the player's own minions. It *does* enter
  // Mode::Targeting (like a regular fired spell) so a cursor can pick which minion when
  // more than one is around — but Enter swaps the player's and the targeted minion's x/y
  // directly (a guaranteed effect, no dodge/damage roll, since it's cooperating with
  // your own ally) instead of launching anything, and only succeeds if the cursor is
  // actually on a living minion. No FOV requirement when auto-aiming the cursor (see
  // closest_own_minion()) — a minion's position is always known to its own summoner.
  // range still applies (how far a minion can be and still be swapped with).
  bool is_swap = false;
  // Which permanent spell school this spell belongs to (see SpellSchool in entity.hpp) —
  // None means shared, available under any school. Checked by known_spell_indices()
  // alongside unlock_int, so a spell needs both a high enough Intelligence *and* the
  // right (or no) school to actually be known.
  SpellSchool school = SpellSchool::None;
  // Combat Mage self-buffs: resolve immediately from the spell menu like a summon (no
  // Mode::Targeting), applying a fixed-duration buff to the caster — same "refresh timer
  // without stacking" idiom apply_potion() uses for STR/DEX/INT. dice_count/dice_sides/
  // speed/range/aoe_radius/hit_dice/glyph/color are unused; buff_amount/buff_turns
  // mirror Potion's fields of the same name, and all three kinds share that one pair.
  //   is_melee_buff -> flat melee damage       (Battle Fury)
  //   is_armor_buff -> flat damage reduction   (Iron Skin)
  //   is_haste_buff -> extra actions per turn  (Haste; see total_actions_for())
  bool is_melee_buff = false;
  bool is_armor_buff = false;
  bool is_haste_buff = false;
  int buff_amount = 0;
  int buff_turns = 0;
  // True for a piercing spell (Lightning Bolt): the fired Projectile keeps traveling
  // through a hostile hit instead of stopping there. Deliberately not combined with
  // aoe_radius today — see advance_projectiles().
  bool pierces = false;
};

extern const std::vector<Spell> kSpellTable;

// Indices into kSpellTable of every spell the player currently knows, in display order.
// A spell needs both a high enough Intelligence *and* the right school —
// kSpellTable[i].school == SpellSchool::None (shared, e.g. Magic Dart) or a match against
// chosen_school (see Actor::chosen_school / Mode::SchoolChoice). Kept as one function so
// the spell-menu render, the spell-menu input, and both new-unlock diffs can't drift.
std::vector<int> known_spell_indices(int intelligence, SpellSchool chosen_school);

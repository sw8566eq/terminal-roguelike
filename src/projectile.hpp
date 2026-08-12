#pragma once

// A spell or arrow in flight, the geometry it travels along, and the phrasing for who
// fired it.
//
// Travel is turn-based, not animated: a Projectile carries a precomputed Bresenham path
// and advances `speed` tiles per player turn (see advance_projectiles()). "Instant" is
// simply a speed high enough to cross the whole map in one turn (kInstantSpellSpeed),
// not a separate code path.

#include <libtcod.hpp>

#include <string>
#include <utility>
#include <vector>

#include "actors.hpp"
#include "entity.hpp"
#include "map.hpp"

// A spell — or, via Mode::RangedAttack, a fired weapon shot; same struct, same
// resolution code, just sourced from a Weapon instead of a Spell at fire time — in
// flight. Advances along its precomputed path, stopping at the first wall it reaches,
// and, unless it pierces, the first opposing Actor too.
struct Projectile {
  std::vector<std::pair<int, int>> path;  // tiles from just past the caster through the target
  size_t path_index = 0;                  // how many tiles of the path have been consumed so far
  int speed = 1;
  int dice_count = 1;
  int dice_sides = 2;
  int bonus = 0;  // locked in at cast/fire time (e.g. floor(INT/3), floor(DEX/3)), not re-read later
  int hit_dice_count = 1;  // locked in from the spell/weapon at cast/fire time, see dodge_chance()
  int hit_dice_sides = 4;
  // The caster's own accuracy contribution (their Dexterity term, see accuracy_roll()),
  // locked in at cast/fire time because a slow projectile may not reach anything for
  // several turns. The hit-dice above are still rolled fresh per target on impact, so
  // together these reconstruct exactly the accuracy any melee swing would have had.
  int accuracy_bonus = 0;
  int aoe_radius = 0;  // 0 = single-target hit only; >0 = explode in a square blast on impact
  // True for a piercing spell (Lightning Bolt): keeps traveling through a hostile hit
  // instead of stopping there, hitting every monster along the line. Copied from
  // Spell::pierces at cast time; never combined with aoe_radius > 0 today (see
  // advance_projectiles()).
  bool pierces = false;
  int prev_x = 0;  // last tile actually entered so far (seeded at the caster's tile at cast
  int prev_y = 0;  // time) — where an aoe_radius>0 spell explodes if the next tile is a wall
  std::string name;
  char glyph = '*';
  tcod::ColorRGB color{255, 255, 255};
  // Who fired this. Before monsters could cast, every Projectile was implicitly the
  // player's and all three of these were baked into advance_projectiles() as constants.
  //
  // owner_allegiance decides both what the shot can hit (never its own side — the same
  // rule that already made minions transparent to the player's spells, just no longer
  // hard-coded to one direction) and which way XP flows on a kill. owner_is_player and
  // owner_name only drive message wording.
  //
  // The owner's *name* is snapshotted at cast time rather than resolved by id on impact,
  // for exactly the reason accuracy_bonus is: a slow projectile can outlive its caster,
  // and "the Goblin Shaman's Magic Dart" should still read correctly if the Shaman died
  // before it landed.
  Allegiance owner_allegiance = Allegiance::Player;
  bool owner_is_player = true;
  std::string owner_name;
};

// Where a shot fired along `path` from (start_x,start_y) would come to rest, applying
// the same three rules advance_projectiles() applies turn-by-turn: a wall stops it on
// the tile just before the wall, a monster stops it on the monster's own tile, and
// running off the end of the path (nothing there) stops it on the final tile. Used by
// the Targeting aim preview to show where an AoE spell would actually explode before the
// player commits to firing — keep this in sync with advance_projectiles() if the
// stopping rules ever change.
//
// Only ever called for aoe_radius > 0 spells, which are never piercing today, so it
// needs no pierce-awareness (a piercing shot has no single impact point).
std::pair<int, int> find_impact(const std::vector<std::pair<int, int>>& path, int start_x, int start_y, const Map& map,
                                const std::vector<Actor>& monsters);

// The Projectile counterparts of actor_possessive()/actor_subject(). They can't just call
// those, because a Projectile only carries a snapshot of who fired it — the caster may be
// dead by the time it lands (see Projectile's owner fields).
//   projectile_possessive -> "your" / "your Imp's" / "the Goblin Shaman's"
//   projectile_subject    -> "Your Magic Dart" / "The Goblin Shaman's Magic Dart"
std::string projectile_possessive(const Projectile& proj);
std::string projectile_subject(const Projectile& proj);

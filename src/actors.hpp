#pragma once

// Queries and phrasing over a floor's Actor list, plus the two things that read an
// Actor's inventory (weapon selection and the drop/equip slot list).
//
// Everything here is a pure function over `const std::vector<Actor>&` — the vector is
// Level::monsters in practice, but nothing here knows that, which is what keeps these
// usable from the AI, the renderer and the input handlers alike.
//
// A note on the three "who is at (x,y)" queries: they differ only by allegiance, and
// picking the right one is how friendly fire is prevented. monster_at() matches anyone,
// hostile_monster_at() only enemies, own_minion_at() only the player's own. All three
// require is_alive(), because a killed Actor isn't erased until the sweep at the end of
// the turn — a corpse must not keep blocking its tile.

#include <string>
#include <vector>

#include "entity.hpp"
#include "map.hpp"

// Chebyshev distance between two Actors — the "how far apart are these" measure used
// everywhere reach is decided (attack range, aura radius, weapon selection). Diagonal
// movement costs the same as orthogonal, so this is the metric that matches how things
// actually move. Note the aim-cursor range clamps use squared-Euclidean instead, which
// is a deliberate difference: a circular targeting radius, a square reach.
int distance_between(const Actor& a, const Actor& b);

// Index into `monsters` of the living Actor standing at (x,y), regardless of side, or
// -1 if none.
int monster_at(const std::vector<Actor>& monsters, int x, int y);

// Same, but only ever matches a hostile — a minion at (x,y) is invisible to this query.
// Used everywhere a player-cast spell decides what blocks/stops it (find_impact(), the
// live aim-preview line), so the player's own minions are fully transparent to their
// spells: never targeted, never blocking the shot, never the thing a Magic Dart fizzles
// against.
int hostile_monster_at(const std::vector<Actor>& monsters, int x, int y);

// The other way around — only ever matches the player's own minion. Used by the Place
// Swap spell to decide whether the cursor is on a minion to swap with.
int own_minion_at(const std::vector<Actor>& monsters, int x, int y);

// The living Actor at (x,y) that a projectile fired by `owner` is allowed to hit: only
// ever the opposing side. This is hostile_monster_at() generalized by who fired — with
// owner = Allegiance::Player it matches exactly what that does, and with
// owner = Allegiance::Hostile it matches minions instead. Note the player is never in
// `monsters`, so a hostile shot's chance to hit *them* is checked separately by
// advance_projectiles().
int projectile_target_at(const std::vector<Actor>& monsters, int x, int y, Allegiance owner);

// Index into `actors` of the living Actor with the given id, or -1 if it's dead/gone.
// Used to resolve MinionOrder::AttackTarget's attack_target_id back to an actual Actor
// each turn, rather than holding a raw index (unsafe across a vector that erases in
// place on death — see Actor::id).
int actor_index_by_id(const std::vector<Actor>& actors, int id);

// Picks which hostile to auto-target when Targeting/RangedAttack mode is entered: the
// last-targeted hostile (last_target_id) if it's still alive, hostile, in the player's
// FOV, and within range — otherwise the closest hostile meeting the same two conditions
// — otherwise -1 (caller falls back to the player's own tile). Range is squared-
// Euclidean (dx*dx+dy*dy <= range*range), matching the cursor-movement clamp both modes
// already use. FOV is the same "can currently be targeted" proxy monster AI's own target
// selection uses.
int auto_target_hostile(const std::vector<Actor>& monsters, const Actor& player, const Map& map, int last_target_id,
                        int range);

// The closest living minion the player could actually swap places with: within range
// (same squared-Euclidean metric auto_target_hostile() uses) *and* with an unobstructed
// line to it (line_clear(), so a wall blocks the swap but a hole doesn't — the same rule
// every other shot in the game follows). Seeds the cursor when entering Mode::Targeting
// for Place Swap (Spell::is_swap).
//
// Still no FOV-radius filter, unlike auto_target_hostile(): a minion's position is always
// known to its own summoner, which is why the renderer draws yours out of sight. The
// line-of-sight requirement is a separate, deliberate limit on the *spell* rather than on
// what you know — swapping through a wall to leave a room was too strong an escape.
int closest_own_minion(const std::vector<Actor>& monsters, const Actor& player, const Map& map, int range);

// How many of the player's minions are currently alive on this floor (minions always
// live on whichever floor the player is on — see move_minions_to_new_floor()).
int count_minions(const std::vector<Actor>& monsters);

// How many living player-allegiance minions are named `name` — used to enforce a
// summon spell's own Spell::minion_cap independent of any other minion source (Summon
// Imp's pool doesn't compete with Summon Demon's or Raise Dead's for room).
int count_minions_named(const std::vector<Actor>& monsters, const std::string& name);

// How many living player-allegiance minions came from raising a corpse rather than
// being conjured — Actor::monster_template_index != -1 is what "raised" means (see
// spawn_reanimated()). Used to enforce Raise Dead's own Spell::minion_cap in aggregate
// across every species it can produce, since it has no single fixed template.
int count_raised_minions(const std::vector<Actor>& monsters);

// One-line status for a minion, for the roster menu (Mode::MinionRoster) — "attacking"
// names the target if it can still be resolved, with the same fallback wording the
// minion itself falls back to (Follow) if it can't. Full console width there, so this
// sentence-length form is safe.
std::string describe_minion_order(const Actor& minion, const std::vector<Actor>& monsters);

// Compact single-letter order indicator for the sidebar's Minions list, where a target's
// name ("attacking the Goblin Slinger") would run past the panel's edge.
std::string minion_order_flag(const Actor& minion);

// --- Message phrasing ---
//
// Second person for the player, "your X" for a minion, "the X" for a hostile. Every
// combat message is one template built from these, rather than each call site writing
// its own — that's what lets a single resolve_attack() narrate all nine
// attacker/defender combinations correctly.
std::string actor_subject(const Actor& a);
std::string actor_object(const Actor& a);
std::string actor_possessive(const Actor& a);
// English verb agreement for the templates above: "you hit", but "the Rat hits".
std::string actor_verb(const Actor& a, const std::string& base);

// Equips the best weapon this Actor is carrying for a target `distance` tiles away,
// swapping whatever was equipped back into the inventory. "Best" is the highest average
// damage among those that can actually reach that far. This is what replaced the old
// bespoke MonsterTemplate::melee_weapon pair: a Goblin Slinger lobs its Rock (range 5)
// across the room and draws its Dagger the instant you close, purely because the Dagger
// scores higher at distance 1 — no special-cased "melee weapon" slot involved.
//
// Drawing a melee weapon while adjacent sets melee_engaged (see Actor), after which only
// range-1 weapons are considered — so a ranged monster that has been reached commits to
// the brawl instead of backing off to snipe at whoever is standing next to it.
//
// That commitment lasts only as long as the fight does: if `distance` exceeds the reach
// of every weapon this Actor carries, the flag is cleared first, so a creature whose
// target genuinely got away re-arms. Using its own longest reach rather than a tuning
// constant makes the rule state itself — "nothing I have can touch you, so this is over"
// — and it scales automatically to whatever a future row carries.
//
// An Actor carrying no spare weapons (most monsters, and the player, whose swaps are
// manual through the 'w' menu) returns immediately and is completely unaffected.
//
// Free — it's a draw, not a turn.
void equip_best_weapon_for_range(Actor& actor, int distance);

enum class ItemKind { Weapon, Armor, Potion };

// One selectable row in the equip or drop screen. index == -1 means the intrinsic
// default (Fists / Nothing) for Weapon/Armor respectively; otherwise it's an index into
// the Actor's weapons, armors, or potions inventory. Potions have no intrinsic/equipped
// state, so -1 never appears for ItemKind::Potion.
struct ItemSlot {
  ItemKind kind;
  int index;
};

// The droppable list: the currently equipped weapon/armor (omitted if intrinsic),
// followed by everything carried of each kind, including potions. Reads straight off the
// Actor, so this would work just as well for a monster's pack if anything needed to list
// one.
std::vector<ItemSlot> drop_slots(const Actor& actor);

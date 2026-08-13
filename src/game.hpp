#pragma once

// The whole mutable state of a run, and the game-logic operations over it.
//
// Everything here takes `GameState&` as its first parameter. That is deliberately
// uniform — some of these functions would technically need only a Level or an Actor, but
// a single convention means you never have to guess a signature, and a function that
// grows a new dependency doesn't need its callers rewritten.
//
// The turn loop itself lives in turn.hpp; rendering in render.hpp; input in input.hpp.

#include <string>
#include <vector>

#include "actors.hpp"
#include "entity.hpp"
#include "level.hpp"
#include "projectile.hpp"
#include "run_history.hpp"
#include "spells.hpp"

// The dungeon's own dimensions, independent of how much of it is on screen at once (see
// MAP_VIEW_W/H in render.hpp — the map panel is a scrolling window onto this).
constexpr int MAP_WIDTH = 100;
constexpr int MAP_HEIGHT = 27;

// How far the player can see. Unrelated to any spell's range, which each carry their own.
constexpr int FOV_RADIUS = 8;

// Which screen is up. Drives a modal UI: each mode has its own render function and its
// own input handler, and the transitions between them are the whole of the game's
// screen flow. Adding a full-screen menu means a new value here, a render function, an
// input handler, and a trigger key from Mode::Playing.
enum class Mode {
  StartMenu,
  SetSeed,
  RunHistory,
  Playing,
  WeaponMenu,
  ArmorMenu,
  PotionMenu,
  Drop,
  Dead,
  Win,
  LevelUp,
  SchoolChoice,
  SpellMenu,
  Targeting,
  MessageLog,
  Help,
  MinionRoster,
  MinionFocus,
  MinionAbilityMenu,
  Pickup,
  Look,
  RangedAttack
};

// Everything that makes up a run in progress.
//
// The split between what lives here and what lives on Actor is: a *character* stat goes
// on Actor (so monsters and minions get it too — see entity.hpp), while state about
// where we are in the turn loop or the UI goes here. active_toggle_spell and
// focused_minion_id are the clearest examples: they describe the session, not the player.
struct GameState {
  // The player is deliberately NOT in any Level::monsters list — floors persist and the
  // player moves between them, so they'd have to be spliced in and out constantly. Every
  // "is anything at (x,y)" check that matters therefore tests the player's tile
  // separately; that's a known, load-bearing asymmetry, not an oversight.
  Actor player;

  std::vector<Level> levels;
  int current_level = 0;

  Mode mode = Mode::Playing;

  std::vector<std::string> message_log;  // full history; the log panel shows the last few
  int log_scroll = 0;                    // lines scrolled up from the bottom, while Mode::MessageLog
  std::string death_cause;               // whatever last killed the player, for the death screen
  std::string win_cause;                 // the final boss's name, for the win screen (Mode::Win)
  int pending_attribute_points = 0;      // unspent level-up points forcing a Mode::LevelUp prompt

  // Mode::StartMenu session state: which of its four options is highlighted. Session
  // state rather than a character stat, same category as active_toggle_spell/
  // focused_minion_id below.
  int start_menu_selection = 0;
  std::string seed_input;  // digits typed so far, while Mode::SetSeed
  // The seed the shared RNG was last explicitly pinned to (via --seed=N or the Set Seed
  // screen), shown on the start menu and recorded into run_history.txt when a run ends.
  // "random" until one is actually set — see seed_rng() in rng.hpp.
  std::string current_seed_display = "random";

  int casting_spell_index = -1;  // which kSpellTable entry is being aimed, while Mode::Targeting
  // Who is casting it: -1 for the player, otherwise an Actor::id — a minion using one of
  // its own abilities (see Actor::abilities). Mode::Targeting reads this to decide whose
  // position the shot originates from, whose mana pays, whose Dexterity aims it, and how
  // the log line is phrased. Reset to -1 whenever the player casts, so a stale minion id
  // can never redirect an ordinary spell.
  int casting_actor_id = -1;
  int target_x = 0;              // cursor, while Mode::Targeting, MinionFocus, Look or RangedAttack
  int target_y = 0;
  // The id of the last hostile Targeting/RangedAttack's cursor was aimed at when the
  // player fired (see auto_target_hostile()) — shared between the two modes the same way
  // target_x/target_y are. -1 until the first shot connects with something.
  int last_target_id = -1;
  // Index into kSpellTable of the currently-running toggle spell (Sandstorm), or -1 if
  // none. Only one can run at a time — simple on purpose, since there's only one so far;
  // a second would need its own slot or a small vector.
  int active_toggle_spell = -1;
  // How many of the player's extra actions (total_actions_for(player) - 1, normally 0)
  // have already been spent inside the current world turn. See end_turn()'s free-action
  // guard, which is the only thing that reads or writes it.
  int free_actions_used = 0;
  // Which minion currently has command focus while cycling with o/p — persists across
  // focus sessions so repeated presses keep moving through the roster in order rather
  // than resetting to the first minion. -1 = none focused (never cycled, or Shift+P
  // reset it).
  int focused_minion_id = -1;
  // True only during a Mode::MinionFocus session opened via the roster's "All" option:
  // the resulting order applies to every living minion instead of just focused_minion_id.
  // Doesn't touch focused_minion_id, so cycling position survives an "All" session.
  bool commanding_all_minions = false;

  // Which entries of ground_slots_at(level, player.x, player.y) are checked, while
  // Mode::Pickup. Parallel to that list rather than a copy of the items, which is safe
  // because no turn passes while the menu is open, so the ground can't shift under it.
  // Rebuilt every time the menu opens; meaningless outside Mode::Pickup.
  std::vector<bool> pickup_selected;

  bool reveal_all = false;  // --reveal debug flag
  bool running = true;      // cleared to break the main loop

  // The floor the player is standing on. Returns a fresh reference every call on
  // purpose: `levels` can be push_back()'d mid-turn (descend() generating a new floor),
  // which reallocates and would dangle any reference held across that point. Never cache
  // the result across a call that might add a floor.
  Level& level() { return levels[static_cast<size_t>(current_level)]; }
  const Level& level() const { return levels[static_cast<size_t>(current_level)]; }
};

// The only way anything should touch the message log — never build a combined string
// with +=. Every distinct event (a hit, a dodge, a pickup, a retaliation) gets its own
// call, even several in the same turn. An exact repeat of the last entry is coalesced
// into it with a "xN" counter, so no caller needs to think about repetition.
void add_message(GameState& gs, const std::string& text);

// The one place the player's level actually advances: bumps `level` and queues one more
// forced attribute-point prompt. Deliberately doesn't touch XP itself — grant_xp() spends
// XP as it loops through however many thresholds one reward crosses, and --level=N calls
// this directly to spawn pre-leveled without faking XP. Safe to call repeatedly;
// pending_attribute_points accumulates and the prompt loops until it's spent.
void level_up_once(GameState& gs);

// Grants XP and processes any level-ups it triggers (normally one, but a large reward
// could trigger several), queuing a forced attribute-point prompt for each.
void grant_xp(GameState& gs, int amount);

// Drinking a potion, for anybody. The player's q menu and a monster deciding it's hurt
// enough to quaff both land here, so an item's effect is defined exactly once and can't
// drift between "what it does for you" and "what it does for them".
//
// A buff's knock-on ceiling (Strength's max HP, Dexterity's evasion, Intelligence's max
// mana) is applied as a *delta* rather than by recomputing from the attribute. That's
// what lets one function serve both sides: the player's ceilings are derived from their
// attributes and a monster's are authored in its table row, but "+5 STR is worth +35 max
// HP" is true either way. Ceiling only — unlike a level-up, current HP/mana don't jump.
void apply_potion(GameState& gs, Actor& actor, const Potion& potion);

// Applies a targeted debuff (Spell::is_debuff — Wither Curse today) to `target`. No dodge
// roll and no projectile: a curse lands, the same way Place Swap resolves without a roll.
// Refreshes rather than stacks, the identical idiom apply_potion() uses for a buff.
void apply_debuff(GameState& gs, Actor& target, const Spell& spell);

// Whether this Actor decides to spend its turn drinking something: gulp a heal when badly
// hurt, pop a buff when a fight is actually on. The player never routes through this —
// they pick potions themselves — but it calls the same apply_potion() they do. Returns
// true if a potion was drunk, in which case the caller skips the rest of that Actor's
// turn (drinking costs a turn for a monster exactly as it does for you).
bool try_actor_use_potion(GameState& gs, Actor& actor, bool enemy_near);

// Everything that happens when an Actor's HP reaches 0, wherever the killing blow came
// from — a melee swing, a spell, an aura tick. Deliberately does NOT erase the victim: a
// single deferred sweep at the end of end_turn() does that, so no loop can have the
// vector shift out from under it mid-turn.
//
// The two things here that only make sense for one side are exactly the two flagged on
// Actor::is_player: a dead player becomes a death screen instead of a corpse, and XP only
// flows to the player (including from a minion's kill — your minion's kill is your kill).
void on_actor_killed(GameState& gs, Actor& victim, bool killed_by_player_side, const std::string& cause);

// True once the run is over (Mode::Dead or Mode::Win) — the shared "stop acting" check.
// Every place that used to test `mode == Mode::Dead` to keep a dead run from continuing
// (stairs, the AI loops, the toggle-spell tick, end_turn()'s free-action guard) now
// tests this instead, so a win freezes the world exactly the same way a death always
// has. Load-bearing, not just tidy: without it, e.g. a bump-melee kill of the final
// boss would still let every other hostile on the floor take its turn immediately
// afterward, since resolve_attack() and end_turn() run back-to-back with no mode check
// between them.
bool is_game_over(const GameState& gs);

// The one and only melee/ranged attack resolution, for every possible pairing: you
// hitting a Goblin, a Goblin hitting you, a Goblin hitting your minion, your minion
// hitting the Goblin. Dodge, damage, armor and death are computed identically in all
// four cases; only the wording of the log line differs, and that comes out of
// actor_subject()/actor_object() rather than from a branch.
void resolve_attack(GameState& gs, Actor& attacker, Actor& defender, const Weapon& weapon);

// Advances every in-flight projectile on the current floor by its speed in tiles,
// checking each tile it passes for a wall or an Actor to hit.
//
// instant_only restricts it to hit-scan shots (speed >= kInstantSpellSpeed: Magic Dart,
// Energy Lance, Lightning Bolt, the Bow). That's the mode a hasted player's *free* action
// runs in: an instant shot has no travel to observe, so making it wait for the world to
// move would feel broken, while a genuinely slow projectile (Fireball's orb) is supposed
// to be observably in flight and so must only advance on real world turns.
void advance_projectiles(GameState& gs, bool instant_only = false);

// Moves the given ground items into the player's inventory, each as its own log message,
// and removes them from the floor. Shared by the single-item fast path and the Pickup
// menu's confirm so "what picking up does" is defined once. Erases highest-index-first
// within each kind, so removing one entry can't shift another out from under it.
// Does not spend a turn — the caller decides that.
void pick_up_ground_items(GameState& gs, const std::vector<ItemSlot>& slots);

// (Re)generates the dungeon and resets the player, for both the initial game and every
// restart after death.
void start_new_game(GameState& gs);

// Moves every one of the player's minions from `from_level` to `to_level`, positioned
// near the player's new spot — so minions follow the player between floors instead of
// being left behind (floors are otherwise fully independent and persistent). Falls back
// to any free tile on the new floor if the area around the player is too crowded.
void move_minions_to_new_floor(GameState& gs, Level& from_level, Level& to_level);

// Goes down the stairs the player is standing on, generating the floor below the first
// time it's visited; and its counterpart, landing on the stairs-down of the floor above.
//
// Neither spends a turn — end_turn() is called at the input call sites instead, so the
// floor being left gets one parting action. That placement matters: --floor=N replays
// descend() in a loop at startup and must not run monster AI on every intermediate floor.
void descend(GameState& gs);
void ascend(GameState& gs);

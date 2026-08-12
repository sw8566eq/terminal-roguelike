#pragma once

#include <libtcod.hpp>
#include <string>
#include <vector>

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
  // This weapon's accuracy contribution: rolled and subtracted from the defender's
  // evasion rating every time it swings (see dodge_chance() in rules.hpp), so a bigger
  // hit-dice roll makes the attack harder to dodge. This applies to *whoever* is
  // holding the weapon — the player, a monster, or a minion all roll the same way, so
  // a monster's Bite/Rock is exactly as much of an accuracy stat as the player's
  // Dagger. Trailing fields with defaults so positional literals that don't mention
  // them keep compiling unchanged.
  int hit_dice_count = 1;
  int hit_dice_sides = 4;
  // How many tiles away (Chebyshev distance) this weapon can attack from. 1 (the
  // default) is melee/adjacency-only. This is the *only* source of an Actor's attack
  // range — a monster with a range-5 Rock and the player with a range-8 Bow are the
  // same case, and swapping weapons changes reach for either of them. The player fires
  // a range>1 weapon via Mode::RangedAttack in input.cpp (aim with a cursor like a
  // spell, Enter to loose a Projectile); monsters just attack from range directly. The
  // bump-to-attack path stays melee-only regardless of what's equipped.
  int attack_range = 1;
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

// Which permanent spell path the player has picked (see Actor::chosen_school below and
// Mode::SchoolChoice in game.hpp, forced the first time Intelligence reaches 4). None on
// a Spell (spells.hpp's Spell::school) means shared — available regardless of which path
// is chosen (Magic Dart); None on the player means the choice hasn't happened yet.
enum class SpellSchool { None, Caster, Summoner, CombatMage };

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
  // instead of healing/buffing — see the Mode::PotionMenu drink handler in input.cpp.
  bool teleports = false;
};

// Which side an Actor fights on. Hostile is the default so every existing monster
// spawn path is unaffected; Player marks a summoned minion. The player's own Actor
// doesn't use this field at all (see main()'s combat-target-selection code, which
// treats "the player" as a special case rather than an Allegiance::Player Actor
// living in level.monsters).
enum class Allegiance { Hostile, Player };

// What a player-owned minion is currently doing — see the minion-AI block in
// end_turn() (turn.hpp). Set per-unit (not one pack-wide variable) so a pack-wide
// command and per-minion overrides (cycle-focus, see main()'s focused_minion_id) are
// both just "write this to one or every minion" — no rearchitecting between the two.
// Follow paths toward the player, fighting anything adjacent along the way.
// AttackTarget paths toward/fights one specific enemy by id (attack_target_id below).
// Hold paths toward and then stands at one specific tile (hold_x/y below), still
// fighting anything that comes into range — "guard this spot" rather than "sit idle."
// Aggressive is Follow's proactive sibling: it paths toward the player exactly like
// Follow does when nothing hostile is around, but as soon as a hostile monster is
// anywhere in the player's FOV it breaks off to chase and fight that one instead
// (attack_target_id tracks the current pursuit, same field AttackTarget uses, just
// auto-selected instead of player-chosen) — "go engage anything you can see" rather
// than "wait for something to wander into my own reach."
enum class MinionOrder { Follow, AttackTarget, Hold, Aggressive };

// A living thing on the map: the player, a hostile monster, or a player-owned minion.
// There is exactly one of these types, deliberately: every one of them equips a weapon
// and armor, carries an inventory, rolls the same accuracy/damage/dodge math
// (resolve_attack() in game.hpp), regenerates, and can be under a temporary stat buff.
// A monster is not a reduced Actor — it's a full one whose numbers happen to come from
// a content table instead of from level-ups.
//
// The handful of things that genuinely aren't symmetric are called out per-field below,
// and they're all *content* differences rather than mechanical ones:
//   - derived vs. authored: the player's max_hp/evasion are computed from their
//     attributes (max_hp_for_level_and_strength/evasion_for_dexterity); a monster's are
//     written directly in its table row. Both end up as the same two numbers feeding
//     the same formulas.
//   - progression (xp/level/xp_reward) and mana only ever move for the player today,
//     because nothing grants a monster XP and no monster casts — not because the fields
//     mean anything different on them.
struct Actor {
  int x = 0;
  int y = 0;
  int hp = 1;
  int max_hp = 1;
  char glyph = '?';
  tcod::ColorRGB color{255, 255, 255};
  std::string name;

  // Equipped gear. Everyone has both: armor's flat reduction applies to any hit that
  // lands on this Actor whoever threw it, and the equipped weapon supplies damage
  // dice, accuracy hit-dice, and attack range.
  Weapon weapon;
  Armor armor;

  // Carried but not equipped — the same inventory the player's w/a/q/d menus operate
  // on, and the same one a monster swaps weapons out of, drinks potions from, and
  // drops on death (see drop_actor_gear() in level.hpp). A monster with an empty
  // inventory behaves exactly like a monster did before inventories existed, so this
  // costs nothing for the plain ones.
  std::vector<Weapon> weapons;
  std::vector<Armor> armors;
  std::vector<Potion> potions;

  // Stable identity, assigned once at spawn (see next_actor_id in main()) and never
  // reused — level.monsters erases in place on death, which shifts a std::vector's
  // later elements down, so a raw index isn't safe to hold onto across turns. Needed
  // by MinionOrder::AttackTarget's attack_target_id below, which must keep pointing at
  // the same enemy even if some other monster earlier in the vector died this turn.
  // The player's own Actor doesn't need one (nothing ever targets it by id).
  int id = 0;

  // Which side this Actor is on. See Allegiance above.
  Allegiance allegiance = Allegiance::Hostile;

  // True only for the one Actor the human is driving. Nothing mechanical branches on
  // this — combat, gear, potions, regen and buffs all run the identical code either
  // way. It exists so the message log can be written in second person ("You hit the
  // Rat" vs. "The Rat hits your Skeletal Minion"), and so a death or an XP award can be
  // routed into the UI (death screen / level-up prompt) instead of just removing an
  // Actor. If you find yourself adding a rule that reads this, that rule is probably
  // the asymmetry worth questioning.
  bool is_player = false;

  // Minions only (Allegiance::Player): the order the player last gave this unit —
  // while order == AttackTarget, which enemy's id it's fighting (reverts to Follow on
  // its own once that target dies or otherwise disappears, see end_turn()); while
  // order == Hold, which tile it's holding.
  MinionOrder order = MinionOrder::Follow;
  int attack_target_id = -1;
  int hold_x = 0;
  int hold_y = 0;

  // Minions only: turns remaining before this minion expires on its own, or -1 for a
  // permanent minion that only ever dies in combat. Sourced from
  // MinionTemplate::duration_turns at summon time; ticks down in end_turn() the same
  // shape as the temp stat-buff timers below, removing the minion (with a message,
  // not a death) at 0.
  int duration_turns = -1;

  // Attributes, and they mean the same thing on everyone. Strength is the flat melee
  // damage bonus (and, for the player, also drives max HP — see the struct comment on
  // derived-vs-authored). Dexterity is the accuracy an attacker brings to every swing
  // (kAccuracyPerDexPoint in rules.hpp) and, for the player, what evasion is derived
  // from. Intelligence drives spell unlocks/damage and max mana. Monsters read all
  // three from their table row, so a Troll hits hard because it has Strength, not
  // because monster damage is computed differently.
  int strength = 0;
  int dexterity = 1;
  int intelligence = 1;
  int level = 1;
  int xp = 0;

  int xp_reward = 0;  // XP this Actor grants its killer; only monsters set it today

  // This Actor's dodge rating: how much of an incoming attack's accuracy roll it soaks
  // before the attack lands (see dodge_chance() in rules.hpp). Everyone has one. The
  // player's is derived from Dexterity (evasion_for_dexterity(), recomputed whenever
  // DEX changes, exactly like max_hp is recomputed when STR does); a monster's is
  // written straight into its table row, so "how hard is this thing to hit" stays a
  // single authored knob per monster instead of falling out of its other stats.
  int evasion = 0;

  // One-way flip, set the first time this Actor attacks from an adjacent tile with a
  // melee (attack_range 1) weapon while also carrying a longer-ranged one. A ranged
  // monster (e.g. Goblin Slinger) snipes with its Rock and never has to approach —
  // right up until its target actually reaches it, at which point it draws its Dagger
  // and commits for good: from then on choose_weapon_for_range() refuses to hand it
  // back the ranged weapon, so it behaves exactly like an ordinary melee-only monster
  // (chasing when not adjacent) for the rest of its life.
  bool melee_engaged = false;

  // How many turns this Actor takes to regenerate from 0 HP to full, or **0 for an
  // Actor that doesn't regenerate at all** — which is every monster and minion today
  // (the player is set to kHpRegenTurns in start_new_game()). Authored per table row
  // like max_hp and evasion are, so a future boss or elite can simply switch it on, and
  // at its own rate rather than being locked to the player's: a slow-healing troll-king
  // is `hp_regen_turns = 400`, not a special case.
  //
  // Regen is deliberately off by default because wounds on an ordinary monster should
  // stick — chip-and-retreat tactics stop working if everything you walk away from
  // quietly heals up.
  int hp_regen_turns = 0;

  // Fractional HP banked toward the next point of passive regen (HP/turn is usually not
  // a whole number, so this carries the remainder between turns).
  float hp_regen_accumulator = 0.0f;

  // Mana, spent to cast spells (see Spell::mana_cost in spells.hpp) and regenerated
  // passively the same way HP is. max_mana comes from max_mana_for_intelligence();
  // mana_regen_accumulator is the same fractional-banking trick as
  // hp_regen_accumulator above. Only the player casts today, so on a monster these
  // stay 0 and the regen loop no-ops on its own — no special case needed.
  int mana = 0;
  int max_mana = 0;
  float mana_regen_accumulator = 0.0f;

  // Which spell school the player has permanently chosen (see SpellSchool above), or
  // None until Intelligence first reaches 4 (see Mode::SchoolChoice in game.hpp). Only
  // the player ever sets this away from None, since nothing else casts — same shared-
  // but-player-only shape as mana above.
  SpellSchool chosen_school = SpellSchool::None;

  // Temporary stat bonuses from stat potions (Potion of Strength/Dexterity/
  // Intelligence), and turns remaining before each reverts. Ticked down once per turn
  // for every Actor in end_turn(); drinking another potion of the same stat while one
  // is already active just refreshes the timer rather than stacking the bonus. A
  // buff's knock-on ceilings (STR's max HP, DEX's evasion, INT's max mana) are applied
  // and removed as a *delta* in apply_potion()/end_turn() rather than by recomputing
  // from the attribute, so the exact same code works for the player (whose ceilings are
  // derived) and for a monster that drinks the same potion (whose are authored).
  // Intelligence's bonus feeds spell damage, but deliberately NOT
  // known_spell_indices() — only permanent, unmodified intelligence unlocks new spells.
  int temp_str_bonus = 0;
  int temp_str_turns = 0;
  int temp_dex_bonus = 0;
  int temp_dex_turns = 0;
  int temp_int_bonus = 0;
  int temp_int_turns = 0;

  // Temporary flat melee-damage and flat-armor bonuses from Combat Mage spells (Battle
  // Fury / Iron Skin), and turns remaining before each reverts. Same tick/refresh shape
  // as temp_str/dex/int above — ticked down once per turn in end_turn(), re-casting
  // refreshes the timer rather than stacking. Unlike STR/DEX/INT, neither feeds a
  // derived ceiling (no max_hp/evasion/max_mana knock-on) — they're read directly at
  // point of use (resolve_attack()'s damage roll for melee damage, every
  // armor.defense damage-reduction site for armor), so reverting is a plain zero-out,
  // no delta subtraction needed. Spell-only today — only the player casts, so these
  // stay 0 on every monster.
  int temp_melee_damage_bonus = 0;
  int temp_melee_damage_turns = 0;
  int temp_armor_bonus = 0;
  int temp_armor_turns = 0;

  // How many *extra* actions this Actor gets per world turn, beyond the one everything
  // gets. 0 everywhere today except while the player has Haste up. Authored per row for
  // a monster (MonsterTemplate::extra_actions, 0 on every current row — the boss/elite
  // knob, same reserved-for-later shape as hp_regen_turns above); temporary and
  // spell-driven for the player, via the bonus/turns pair below.
  //
  // Genuinely shared, not player-only: the player spends theirs by having end_turn()
  // return early without advancing the world (see its free-action guard), and a monster
  // spends theirs by running its AI-loop body more than once. Different plumbing, since
  // the player's turn is input-driven and a monster's is a loop iteration, but the same
  // field with the same meaning — a monster with extra_actions=1 acts twice for each of
  // your turns, and a hasted player acts twice for each of its turns.
  int extra_actions = 0;
  // Same tick/refresh shape as the buff pairs above; read together with extra_actions
  // via total_actions_for() in rules.hpp, never on its own.
  int temp_extra_actions_bonus = 0;
  int temp_extra_actions_turns = 0;

  // Index into kSpellTable of the one spell this Actor casts, or -1 for "doesn't cast".
  // The player doesn't use this — they pick from known_spell_indices() in the z menu —
  // it's how a caster *monster* (the Goblin Shaman) knows what to throw. One spell
  // rather than a list, deliberately minimal: a second one needs a small vector plus a
  // rule for choosing between them, and nothing wants that yet.
  int spell_index = -1;
  // Indices into kSpellTable of the abilities this Actor can be *ordered* to use, as
  // opposed to spell_index above which is what its own AI throws unprompted. Populated
  // from MinionTemplate::abilities; empty on everything else today.
  //
  // The distinction is who chooses: a Goblin Shaman decides for itself, while a Demon's
  // Wither Curse is fired by the player through the 'z' menu of Mode::MinionFocus. It's
  // a list rather than a single index because a minion is a unit you're managing, so
  // "which of its abilities" is a decision worth having — the reason spell_index stayed
  // singular is that nothing picks *for* a monster.
  std::vector<int> abilities;
  // Turns to regenerate 0 -> full mana, or 0 for "doesn't regenerate", exactly mirroring
  // hp_regen_turns above. This gate didn't exist while the player was the only Actor
  // with a mana pool at all (the regen loop no-opped on its own at max_mana 0); the
  // moment a monster had mana, "no regen" stopped being expressible without it.
  int mana_regen_turns = 0;

  // Monsters only: the last tile this monster actually saw the player standing on,
  // or (-1, -1) if it's never seen them (or already reached that tile without finding
  // them there). Lets a monster keep heading for where the player was after losing
  // line of sight, instead of immediately reverting to idle wandering — see the
  // chase/investigate/wander logic in end_turn().
  int last_seen_player_x = -1;
  int last_seen_player_y = -1;

  bool is_alive() const { return hp > 0; }
};

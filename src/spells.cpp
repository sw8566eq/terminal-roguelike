#include "spells.hpp"

const std::vector<Spell> kSpellTable = {
    // range happens to match the player's starting FOV radius today, but it's its own
    // fixed number — it won't change if FOV radius ever does (e.g. a future perception
    // mechanic).
    {"Magic Dart", /*unlock_int=*/3, /*dice_count=*/1, /*dice_sides=*/2, kInstantSpellSpeed, /*range=*/8,
     /*mana_cost=*/1, /*aoe_radius=*/0, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/1, /*hit_dice_sides=*/4, '*', tcod::ColorRGB{200, 100, 255}, /*is_summon=*/false,
     /*summon_template_index=*/0, /*is_swap=*/false, /*school=*/SpellSchool::None},
    // A straightforward upgrade to Magic Dart, not a new archetype — same instant
    // single-target shape, just better dice and harder to dodge (2d4 hit-dice vs. Magic
    // Dart's 1d4), for a steeper mana cost. school=None so it's shared like Magic Dart
    // rather than gated to one path — unlock_int=6 puts it a couple levels past the
    // Intelligence-4 school choice, a stated default (same tier Fireball used to unlock
    // at before the school split), adjustable like other spells' numbers have been.
    {"Energy Lance", /*unlock_int=*/6, /*dice_count=*/1, /*dice_sides=*/6, kInstantSpellSpeed, /*range=*/8,
     /*mana_cost=*/3, /*aoe_radius=*/0, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/2, /*hit_dice_sides=*/4, '+', tcod::ColorRGB{120, 180, 255}, /*is_summon=*/false,
     /*summon_template_index=*/0, /*is_swap=*/false, /*school=*/SpellSchool::None},
    // Slow-moving orb (visibly crosses several turns instead of resolving instantly) that
    // explodes into a 3x3 blast wherever it stops, rather than just hitting one target.
    // Its high hit-dice (vs. Magic Dart's low one) is the AoE-is-hard-to-dodge case.
    // Caster's entry spell — unlock_int=4 so it's already castable the moment the
    // Caster/Summoner choice (Mode::SchoolChoice) is made, same as Raise Skeleton is
    // for Summoner below.
    {"Fireball", /*unlock_int=*/4, /*dice_count=*/1, /*dice_sides=*/6, /*speed=*/2, /*range=*/8,
     /*mana_cost=*/3, /*aoe_radius=*/1, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/3, /*hit_dice_sides=*/6, 'o', tcod::ColorRGB{255, 120, 40}, /*is_summon=*/false,
     /*summon_template_index=*/0, /*is_swap=*/false, /*school=*/SpellSchool::Caster},
    // Toggled aura, not a fired spell: 7x7 around the player (aoe_radius=3), 2 flat
    // damage/turn to every monster caught in it, drains 2 mana/turn while active.
    // Turning it on costs a flat 3 mana for that turn instead of the per-turn drain
    // (see the SpellMenu toggle handler) — steep enough that flicking it on and off
    // every turn to save mana isn't actually cheaper than just leaving it running.
    // Caster's second spell — unlock_int=7 keeps the same +3 gap after Fireball (4)
    // that it had after Fireball's old unlock_int (6 -> 9), just shifted down.
    {"Sandstorm", /*unlock_int=*/7, /*dice_count=*/0, /*dice_sides=*/0, /*speed=*/0, /*range=*/0,
     /*mana_cost=*/3, /*aoe_radius=*/3, /*is_toggle=*/true, /*tick_damage=*/2, /*tick_mana_cost=*/2,
     /*hit_dice_count=*/2, /*hit_dice_sides=*/6, 's', tcod::ColorRGB{230, 190, 90}, /*is_summon=*/false,
     /*summon_template_index=*/0, /*is_swap=*/false, /*school=*/SpellSchool::Caster},
    // First summon spell: raises kMinionTable[0] (Skeletal Minion) next to the player.
    // Summoner's entry spell — unlock_int=4 so it's already castable the moment the
    // Caster/Summoner choice is made, same as Fireball is for Caster above. See the
    // SpellMenu handler for how casting a summon spell differs from firing/toggling.
    {"Raise Skeleton", /*unlock_int=*/4, /*dice_count=*/0, /*dice_sides=*/0, /*speed=*/0, /*range=*/0,
     /*mana_cost=*/4, /*aoe_radius=*/0, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/0, /*hit_dice_sides=*/0, 'u', tcod::ColorRGB{100, 200, 220}, /*is_summon=*/true,
     /*summon_template_index=*/0, /*is_swap=*/false, /*school=*/SpellSchool::Summoner},
    // Trades places with a minion instead of dealing damage — a tactical reposition
    // (pull yourself to a minion holding a doorway, or swap a hurt minion out of melee
    // and take its spot yourself). Summoner's second spell — unlock_int=5 keeps the
    // same +1 gap after Raise Skeleton (4) that it had after Raise Skeleton's old
    // unlock_int (5 -> 6), just shifted down. range=6 is a stated default. mana_cost=8
    // is deliberately steep, not cheap like a damage spell's cost — this is a
    // no-dodge escape (swap to a minion standing somewhere safer) as much as it's an
    // engage tool — though it now needs line of sight, so it can't pull you through a
    // wall — and is priced to be an emergency option,
    // not something to lean on every encounter: at the unlock threshold (max_mana=13
    // at INT 5) a single cast leaves barely a third of the bar, and takes a good chunk
    // of passive regen to recover. Scales down in relative cost as INT climbs further,
    // same as every other spell's mana cost does against a rising max_mana ceiling.
    {"Place Swap", /*unlock_int=*/5, /*dice_count=*/0, /*dice_sides=*/0, /*speed=*/0, /*range=*/6,
     /*mana_cost=*/8, /*aoe_radius=*/0, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/0, /*hit_dice_sides=*/0, '=', tcod::ColorRGB{100, 220, 255}, /*is_summon=*/false,
     /*summon_template_index=*/0, /*is_swap=*/true, /*school=*/SpellSchool::Summoner},
    // Combat Mage's entry spell — unlock_int=4, same rule as Fireball/Raise Skeleton:
    // INT 4 grants a spell no matter which of the three schools is picked. Flat melee
    // damage, not accuracy — melee-only (see resolve_attack()'s raw_damage line):
    // doesn't help a fired spell or the Bow. Stated defaults, adjustable like Place
    // Swap's mana cost was after playtesting.
    {"Battle Fury", /*unlock_int=*/4, /*dice_count=*/0, /*dice_sides=*/0, /*speed=*/0, /*range=*/0,
     /*mana_cost=*/3, /*aoe_radius=*/0, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/0, /*hit_dice_sides=*/0, 'f', tcod::ColorRGB{220, 80, 60}, /*is_summon=*/false,
     /*summon_template_index=*/0, /*is_swap=*/false, /*school=*/SpellSchool::CombatMage,
     /*is_melee_buff=*/true, /*is_armor_buff=*/false, /*is_haste_buff=*/false,
     /*buff_amount=*/4, /*buff_turns=*/10},
    // Combat Mage's second spell — unlock_int=5, same +1 gap after the entry spell as
    // Summoner's Raise Skeleton(4)->Place Swap(5). buff_amount=3 matches Chainmail's own
    // +3 defense. Applies to all damage sources (see every armor.defense site), not
    // just melee, unlike Battle Fury.
    {"Iron Skin", /*unlock_int=*/5, /*dice_count=*/0, /*dice_sides=*/0, /*speed=*/0, /*range=*/0,
     /*mana_cost=*/3, /*aoe_radius=*/0, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/0, /*hit_dice_sides=*/0, 'k', tcod::ColorRGB{150, 150, 220}, /*is_summon=*/false,
     /*summon_template_index=*/0, /*is_swap=*/false, /*school=*/SpellSchool::CombatMage,
     /*is_melee_buff=*/false, /*is_armor_buff=*/true, /*is_haste_buff=*/false,
     /*buff_amount=*/3, /*buff_turns=*/9},
    // Caster's third spell — a genuinely new mechanic, not just another stat tier: a
    // piercing beam (Projectile::pierces) that hits every hostile monster along its
    // line of travel instead of stopping at the first one. Deliberately NOT combined
    // with an aoe_radius blast (kept 0) — a pure line-pierce, which keeps the
    // advance_projectiles() change minimal (reuses the existing single-target
    // per-victim roll block verbatim, just doesn't stop on a hit) rather than calling
    // explode() in a loop. hit_dice_count=3/hit_dice_sides=6 matches Fireball's own
    // hard-to-dodge hit-dice: hitting several targets in a line is comparably hard to
    // evade as an AoE blast. speed=kInstantSpellSpeed (unlike Fireball's slow orb) so
    // the whole line resolves the turn it's cast. unlock_int=9/mana_cost=5 are stated
    // defaults, a tier above Sandstorm (7 unlock/3 mana), reflecting that it can hit
    // several targets per cast — adjustable after playtesting like every other
    // spell's numbers have been.
    {"Lightning Bolt", /*unlock_int=*/9, /*dice_count=*/1, /*dice_sides=*/6, kInstantSpellSpeed, /*range=*/8,
     /*mana_cost=*/5, /*aoe_radius=*/0, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/3, /*hit_dice_sides=*/6, '/', tcod::ColorRGB{255, 255, 120}, /*is_summon=*/false,
     /*summon_template_index=*/0, /*is_swap=*/false, /*school=*/SpellSchool::Caster,
     /*is_melee_buff=*/false, /*is_armor_buff=*/false, /*is_haste_buff=*/false,
     /*buff_amount=*/0, /*buff_turns=*/0, /*pierces=*/true},
    // Summoner's third spell: raises kMinionTable[1] (Demon), the school's stronger,
    // permanent counterpart to Raise Skeleton's early, temporary Skeletal Minion —
    // pure content-table addition, zero new mechanism (identical is_summon shape).
    // unlock_int=9 is the parallel tier to Lightning Bolt above. mana_cost=9 is
    // steeper than Raise Skeleton's 4, reflecting a permanent and much stronger
    // minion — stated default, adjustable after playtesting.
    {"Summon Demon", /*unlock_int=*/9, /*dice_count=*/0, /*dice_sides=*/0, /*speed=*/0, /*range=*/0,
     /*mana_cost=*/9, /*aoe_radius=*/0, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/0, /*hit_dice_sides=*/0, 'D', tcod::ColorRGB{200, 40, 180}, /*is_summon=*/true,
     /*summon_template_index=*/1, /*is_swap=*/false, /*school=*/SpellSchool::Summoner},
    // Combat Mage's third spell, and the school's payoff: buff_amount extra actions per
    // world turn (see Actor::extra_actions / total_actions_for()). Structurally just a
    // third self-buff resolving from the menu like Battle Fury and Iron Skin — the new
    // mechanism is entirely in how the turn loop spends the actions, not here.
    //
    // unlock_int=9 is the parallel tier to Lightning Bolt and Summon Demon.
    // mana_cost=9 matches Summon Demon's, the other school's spell of comparable
    // "changes the whole fight" weight, and buff_turns=8 is deliberately shorter than
    // Battle Fury's 10 — 8 world turns is already 16 actions. Note this multiplies
    // *everything*, including Battle Fury swings and further casts, so it's the most
    // likely number in the table to need tuning down after playtesting.
    {"Haste", /*unlock_int=*/9, /*dice_count=*/0, /*dice_sides=*/0, /*speed=*/0, /*range=*/0,
     /*mana_cost=*/9, /*aoe_radius=*/0, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/0, /*hit_dice_sides=*/0, 'h', tcod::ColorRGB{120, 255, 180}, /*is_summon=*/false,
     /*summon_template_index=*/0, /*is_swap=*/false, /*school=*/SpellSchool::CombatMage,
     /*is_melee_buff=*/false, /*is_armor_buff=*/false, /*is_haste_buff=*/true,
     /*buff_amount=*/1, /*buff_turns=*/8},
    // Not a player spell: a Demon's ability, reachable only by focusing the minion and
    // pressing 'z' (see MinionTemplate::abilities and Mode::MinionAbilityMenu).
    // minion_only keeps it out of known_spell_indices() entirely, so unlock_int and
    // school below are inert — set to values that would be nonsense if it ever did leak
    // into the player's menu, which is itself a small tripwire.
    //
    // The Demon carries max_mana 10 and no regen, so 4 mana buys two curses per Demon
    // and then it's a pure melee bruiser — deliberately the same "spend it and you're
    // done" budget shape as the Goblin Shaman's ten darts. buff_amount=-2 lands on the
    // victim's temp_melee_damage_bonus (see Spell::is_debuff for why not temp_str_bonus);
    // range 5 is shorter than the player's own spells, so a cursing Demon has to be
    // committed to the fight rather than lobbing it from safety. All stated defaults.
    {"Wither Curse", /*unlock_int=*/99, /*dice_count=*/0, /*dice_sides=*/0, /*speed=*/0, /*range=*/5,
     /*mana_cost=*/4, /*aoe_radius=*/0, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/0, /*hit_dice_sides=*/0, 'x', tcod::ColorRGB{160, 60, 200}, /*is_summon=*/false,
     /*summon_template_index=*/0, /*is_swap=*/false, /*school=*/SpellSchool::None,
     /*is_melee_buff=*/false, /*is_armor_buff=*/false, /*is_haste_buff=*/false,
     /*buff_amount=*/-2, /*buff_turns=*/10, /*pierces=*/false,
     /*minion_only=*/true, /*is_debuff=*/true},
    // Summoner's fourth spell and the school's thematic centre: raise the specific thing
    // you just killed, rather than conjuring a generic servant. Whatever the floor throws
    // at you becomes what you can field, so the spell scales with where you are without
    // any depth gating of its own — a floor-8 necromancer raises Trolls because Trolls
    // are what's dying nearby.
    //
    // It needs no power gate because reanimated_hp() is the gate: the strongest raiseable
    // row (Troll, 22 HP) comes back at 14, under the Demon's 20. unlock_int=7 sits between
    // Place Swap (5) and Summon Demon (9), and mana_cost=6 likewise between their 8 and 9.
    //
    // What it buys is a *timed* minion (kRaisedMinionTurns) whose quality depends on what
    // you managed to kill, which is what keeps Summon Demon worth its extra mana: the
    // Demon is the permanent, dependable option, while raising is opportunistic and rots.
    // Stated defaults.
    {"Raise Dead", /*unlock_int=*/7, /*dice_count=*/0, /*dice_sides=*/0, /*speed=*/0, /*range=*/6,
     /*mana_cost=*/6, /*aoe_radius=*/0, /*is_toggle=*/false, /*tick_damage=*/0, /*tick_mana_cost=*/0,
     /*hit_dice_count=*/0, /*hit_dice_sides=*/0, '%', tcod::ColorRGB{140, 200, 140}, /*is_summon=*/false,
     /*summon_template_index=*/0, /*is_swap=*/false, /*school=*/SpellSchool::Summoner,
     /*is_melee_buff=*/false, /*is_armor_buff=*/false, /*is_haste_buff=*/false,
     /*buff_amount=*/0, /*buff_turns=*/0, /*pierces=*/false,
     /*minion_only=*/false, /*is_debuff=*/false, /*is_raise=*/true},
};

std::vector<int> known_spell_indices(int intelligence, SpellSchool chosen_school) {
  std::vector<int> indices;
  for (size_t i = 0; i < kSpellTable.size(); ++i) {
    const Spell& s = kSpellTable[i];
    // A creature ability is never something the player knows, at any Intelligence.
    if (s.minion_only) continue;
    if (intelligence < s.unlock_int) continue;
    if (s.school != SpellSchool::None && s.school != chosen_school) continue;
    indices.push_back(static_cast<int>(i));
  }
  return indices;
}

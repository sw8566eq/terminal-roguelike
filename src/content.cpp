#include "content.hpp"

#include <algorithm>

#include "rules.hpp"
#include "spells.hpp"

const Weapon kFists = Weapon{"Fists",       1,  2,  0, /*is_intrinsic=*/true, /*min_depth=*/1, /*max_depth=*/-1,
                              /*hit_dice_count=*/2, /*hit_dice_sides=*/4};

const Armor kNoArmor = Armor{"Nothing", 0, /*is_intrinsic=*/true};

// Depth-gated the same shape as kMonsterTable: rough tiers, not a strict per-floor
// curve. Placeholder ranges — the actual balance (exactly which floor a Mace should
// start showing up on, etc.) is a follow-up pass; this just makes sure a Dagger stops
// being possible loot on floor 10.
//
// hit_dice trails off as the weapons get heavier: a fast/light Dagger is hardest to
// dodge, a massive Battle Axe easiest — a tradeoff against the heavier weapons' bigger
// damage dice, not a strict upgrade path (see dodge_chance() in rules.hpp).
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
    // Dexterity-derived bonus on top — plus the range and safety of not needing to close
    // to melee. Ammo is unlimited for now; flagged as a balance question to revisit once
    // it's actually been played with.
    {"Bow", 1, 6, 0, /*is_intrinsic=*/false, /*min_depth=*/1, /*max_depth=*/-1,
     /*hit_dice_count=*/2, /*hit_dice_sides=*/4, /*attack_range=*/8},
};

// Same depth-gating shape as kWeaponTable.
const std::vector<Armor> kArmorTable = {
    {"Leather Armor", 1, /*is_intrinsic=*/false, /*min_depth=*/1, /*max_depth=*/5},
    {"Chainmail", 3, /*is_intrinsic=*/false, /*min_depth=*/3, /*max_depth=*/-1},
    {"Plate Armor", 5, /*is_intrinsic=*/false, /*min_depth=*/6, /*max_depth=*/-1},
};

// Stat potions all use the same +5/15-turn shape for now; only Strength has a described
// mechanical effect today (max HP, regen, melee damage) — Dexterity (evasion) and
// Intelligence (spell damage) piggyback on the same existing formulas that already read
// those stats. All are left ungated (default min_depth=1, max_depth=-1, same as
// Weapon/Armor) since there's no real tiering rationale among them yet — unlike
// kWeaponTable/kArmorTable, this table doesn't exercise the depth filter, but the fields
// are there once it needs to.
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

namespace {

// Natural weapons: part of the monster, not loot, so they're marked intrinsic exactly
// like the player's kFists and never drop. Their hit-dice follow the same light-is-
// accurate / heavy-is-not convention as kWeaponTable, and a thrown Rock carries its
// range on the weapon rather than on the monster (see MonsterTemplate::weapon).
//
// Naming convention for any *non*-intrinsic monster weapon (Rusty Sword, Short Bow,
// Orc Axe, Massive Club, ...): its name must be unique across kWeaponTable, because
// once it drops the player can only tell items apart by name. The Orc Archer's weapon
// is a "Short Bow" (range 5) rather than a second "Bow" for exactly this reason — a
// dropped one was otherwise indistinguishable from kWeaponTable's range-8 Bow.
const Weapon kBite = Weapon{"Bite", 1, 3, 0, /*is_intrinsic=*/true, 1, -1, /*hit_dice=*/2, 3};
const Weapon kClaws = Weapon{"Claws", 1, 3, 0, /*is_intrinsic=*/true, 1, -1, /*hit_dice=*/2, 3};
const Weapon kThrownRock =
    Weapon{"Rock", 1, 4, 0, /*is_intrinsic=*/true, 1, -1, /*hit_dice=*/1, 2, /*attack_range=*/5};

}  // namespace

// Roughly increasing toughness/reward with min_depth, so descending gets harder. Each
// "tier" fully replaces the previous one at its cutover rather than overlapping: Rat
// and Goblin stop past floor 4, and Skeleton/Orc pick up as the new baseline exactly
// where they leave off, same idea Troll will hand off to whatever comes after it.
// Both offensive knobs climb with the tiers: `dexterity` (how accurate its swings are)
// and `strength` (flat damage on top of its weapon dice) — the same two stats that do
// the same two jobs for the player.
const std::vector<MonsterTemplate> kMonsterTable = {
    {"Rat", 'r', tcod::ColorRGB{150, 100, 60}, 4, kBite, /*xp_reward=*/5, /*evasion=*/15,
     /*dexterity=*/2, /*strength=*/0, /*min_depth=*/1, /*max_depth=*/4},
    {"Goblin", 'g', tcod::ColorRGB{80, 180, 80}, 7, kClaws, /*xp_reward=*/10, /*evasion=*/5,
     /*dexterity=*/4, /*strength=*/1, /*min_depth=*/1, /*max_depth=*/4},
    // A squishier, ranged relative of the melee Goblin above — glass cannon: less HP,
    // same damage tier, but can fight from range instead of closing to melee. Its Rock
    // has attack_range 5, so it snipes from well across a room without needing to
    // approach at all — but only until the player actually reaches it: it carries a
    // real Dagger (the same one out of kWeaponTable the player can find and use) in
    // extra_weapons, switches to it the moment it's adjacent, and permanently commits
    // to melee from then on (Actor::melee_engaged), behaving exactly like an ordinary
    // chasing Goblin for the rest of the fight rather than backing off to snipe again.
    // The Dagger is much more accurate than the Rock purely because of its hit-dice,
    // and it drops as loot when the Slinger dies.
    {"Goblin Slinger", 'G', tcod::ColorRGB{150, 150, 70}, 5, kThrownRock, /*xp_reward=*/8,
     /*evasion=*/8, /*dexterity=*/2, /*strength=*/1, /*min_depth=*/1, /*max_depth=*/4, /*armor=*/kNoArmor,
     /*extra_weapons=*/{kWeaponTable[0]}},
    // The first monster in the game that actually casts: it spends real mana on the same
    // kSpellTable Magic Dart the player learns, and it launches a real travelling
    // Projectile you can see coming and dodge — not an instant hit resolved inside the AI
    // loop the way a Goblin Slinger's Rock is.
    //
    // Its whole design is the mana budget. INT 3 gives max_mana 10 (authored here to
    // match what max_mana_for_intelligence(3) would give the player) and Magic Dart costs
    // 1, so it opens with ten darts. Each is 1d2 + INT/3 = 2-3 damage, so a full pool is
    // ~25 damage if every one lands: genuinely threatening to a floor-1 character. Out of
    // mana it falls through to ordinary chase-and-melee with 5 HP and Claws, i.e. the
    // weakest thing on the floor. Outlasting it is the intended counterplay, alongside
    // breaking line of sight or closing to melee.
    //
    // It regenerates at the player's own rate now (kManaRegenTurns, 150 turns for a full
    // pool = 1 dart per 15). That's slow enough that the pool is still effectively finite
    // inside a single fight, which is where "outlast it" is actually played — what
    // changed is that a Shaman you walked away from isn't permanently defanged when you
    // come back. If it ever needs to be a strict one-time budget again, this cell is the
    // knob: 0 restores exactly the old behavior.
    {"Goblin Shaman", 'S', tcod::ColorRGB{160, 100, 220}, 5, kClaws, /*xp_reward=*/14,
     /*evasion=*/8, /*dexterity=*/3, /*strength=*/0, /*min_depth=*/1, /*max_depth=*/4,
     /*armor=*/kNoArmor, /*extra_weapons=*/{}, /*potions=*/{}, /*hp_regen_turns=*/0,
     /*extra_actions=*/0, /*is_boss=*/false, /*intelligence=*/3, /*max_mana=*/10,
     /*mana_regen_turns=*/kManaRegenTurns, /*spell_index=*/0},
    {"Skeleton", 's', tcod::ColorRGB{220, 220, 200}, 10,
     Weapon{"Rusty Sword", 1, 4, 0, false, 1, -1, /*hit_dice=*/2, 4}, /*xp_reward=*/15,
     /*evasion=*/8, /*dexterity=*/6, /*strength=*/2, /*min_depth=*/5, /*max_depth=*/-1},
    // Wears real armor — the same Leather Armor the player can find, soaking 1 off
    // every hit that lands on it, and dropped when it dies. Nothing about the damage
    // math is orc-specific: defender.armor.defense is subtracted for whoever is being
    // hit (see resolve_attack()).
    {"Orc", 'o', tcod::ColorRGB{60, 120, 60}, 14, Weapon{"Orc Axe", 1, 6, 0, false, 1, -1, /*hit_dice=*/1, 4},
     /*xp_reward=*/22, /*evasion=*/5, /*dexterity=*/8, /*strength=*/2, /*min_depth=*/5, /*max_depth=*/-1,
     /*armor=*/kArmorTable[0]},
    // Orc Archer: the same snipe-then-permanently-melee behavior as Goblin Slinger,
    // just with every stat scaled up to match this floor-5+ tier — the same
    // relationship Orc already has to Goblin.
    {"Orc Archer", 'O', tcod::ColorRGB{110, 130, 60}, 10,
     Weapon{"Short Bow", 1, 6, 0, false, 1, -1, /*hit_dice=*/2, 4, /*attack_range=*/5}, /*xp_reward=*/18,
     /*evasion=*/6, /*dexterity=*/4, /*strength=*/2, /*min_depth=*/5, /*max_depth=*/-1, /*armor=*/kNoArmor,
     /*extra_weapons=*/{kWeaponTable[1]}},
    {"Troll", 'T', tcod::ColorRGB{100, 110, 80}, 22,
     Weapon{"Massive Club", 1, 8, 0, false, 1, -1, /*hit_dice=*/1, 3}, /*xp_reward=*/40,
     /*evasion=*/2, /*dexterity=*/10, /*strength=*/3, /*min_depth=*/8, /*max_depth=*/-1},
    // The first boss (is_boss — exactly one spawns on floor 3, and it's excluded from
    // that floor's random pool; see bosses_at_depth()). An outsized Orc arriving two
    // floors before ordinary Orcs do, on a floor whose normal residents are Rats and
    // Goblins — it should read as something you weren't supposed to meet yet.
    //
    // Uses both boss knobs' worth of what's already wired, and nothing bespoke:
    //   - hp_regen_turns=120 is the first non-zero value in the game. At 28 max HP
    //     that's roughly 1 HP every 4 turns — far too slow to matter inside a fight,
    //     which is the point: it doesn't make the fight longer, it makes chip-and-
    //     retreat stop working, since anything you don't finish it heals back off while
    //     you're away. See Actor::hp_regen_turns.
    //   - A Potion of Strength in `potions`, the first row in the game to carry one.
    //     try_actor_use_potion() makes it drink once the player is within
    //     kAiBuffPotionRange (4) — so it usually opens the fight by buffing to +5 STR
    //     for 15 turns, through the exact same apply_potion() the player's q menu uses.
    //     If it dies without drinking, the potion drops as loot like any other gear.
    //     (This is the deliberate exception to "no row carries potions today" — one
    //     potion on one boss on one floor isn't the consumables buffet that made
    //     MonsterTemplate::potions get emptied out.)
    //   - Chainmail (3 defense, itself min_depth 3) and a heavy Warlord's Cleaver, both
    //     dropping on death — the fight's actual reward, since neither is something a
    //     floor-3 character reliably has yet.
    // extra_actions stays 0: this is meant to be a wall, not a blender. Numbers are
    // stated defaults, as tunable as every other row.
    {"Orc Warlord", 'W', tcod::ColorRGB{190, 70, 40}, 28,
     Weapon{"Warlord's Cleaver", 1, 8, 0, false, 1, -1, /*hit_dice=*/1, 3}, /*xp_reward=*/55,
     /*evasion=*/4, /*dexterity=*/8, /*strength=*/3, /*min_depth=*/3, /*max_depth=*/3,
     /*armor=*/kArmorTable[1], /*extra_weapons=*/{}, /*potions=*/{kPotionTable[1]},
     /*hp_regen_turns=*/120, /*extra_actions=*/0, /*is_boss=*/true},
};

const std::vector<MinionTemplate> kMinionTable = {
    // A basic, temporary conscript — glyph/color deliberately distinct from the
    // hostile Skeleton ('s', white) so friend and foe never look alike at a glance.
    // Weaker than a real (hostile) Skeleton and time-limited, reflecting that this is
    // an early, low-commitment summon rather than true necromancy (see the roadmap's
    // Phase 3 for permanently reanimating a specific slain monster).
    {"Skeletal Minion", 'u', tcod::ColorRGB{100, 200, 220}, /*max_hp=*/8,
     Weapon{"Bone Claws", 1, 4, 0, /*is_intrinsic=*/true, 1, -1, /*hit_dice=*/2, 4},
     /*evasion=*/6, /*dexterity=*/6, /*strength=*/1, /*duration_turns=*/40,
     /*armor=*/kNoArmor, /*extra_weapons=*/{}, /*potions=*/{}, /*hp_regen_turns=*/0,
     /*extra_actions=*/0, /*abilities=*/{}, /*max_mana=*/0,
     // No pool and no abilities today, so this rate is inert — set anyway so that giving
     // this row a max_mana is the only edit needed to make its mana behave like everyone
     // else's.
     /*mana_regen_turns=*/kManaRegenTurns},
    // Summoner's third spell's summon: stronger and permanent, contrasting with the
    // Skeletal Minion's early, low-commitment, temporary framing above. Glyph 'D' and
    // this magenta are unused by any hostile monster or the Skeletal Minion, so it
    // reads unmistakably as its own thing.
    // Also the first minion with an ability: Wither Curse (kSpellTable's last row), fired
    // by focusing it and pressing 'z'. 10 mana at the player's own regen rate is two
    // curses up front and roughly one more per 60 turns after — enough that a permanent
    // Demon stays useful without the curse becoming a per-turn tool.
    {"Demon", 'D', tcod::ColorRGB{200, 40, 180}, /*max_hp=*/20,
     Weapon{"Demon Claws", 1, 6, 0, /*is_intrinsic=*/true, 1, -1, /*hit_dice=*/2, 6},
     /*evasion=*/10, /*dexterity=*/8, /*strength=*/5, /*duration_turns=*/-1,
     /*armor=*/kNoArmor, /*extra_weapons=*/{}, /*potions=*/{}, /*hp_regen_turns=*/0,
     /*extra_actions=*/0, /*abilities=*/{kWitherCurseIndex},
     /*max_mana=*/10, /*mana_regen_turns=*/kManaRegenTurns},
};

std::vector<int> monsters_available_at_depth(int depth) {
  std::vector<int> indices = available_at_depth(kMonsterTable, depth);
  indices.erase(std::remove_if(indices.begin(), indices.end(),
                               [](int i) { return kMonsterTable[static_cast<size_t>(i)].is_boss; }),
                indices.end());
  return indices;
}

std::vector<int> bosses_at_depth(int depth) {
  std::vector<int> indices = available_at_depth(kMonsterTable, depth);
  indices.erase(std::remove_if(indices.begin(), indices.end(),
                               [](int i) { return !kMonsterTable[static_cast<size_t>(i)].is_boss; }),
                indices.end());
  return indices;
}

std::vector<int> weapons_available_at_depth(int depth) { return available_at_depth(kWeaponTable, depth); }
std::vector<int> armor_available_at_depth(int depth) { return available_at_depth(kArmorTable, depth); }
std::vector<int> potions_available_at_depth(int depth) { return available_at_depth(kPotionTable, depth); }

std::string describe_potion(const Potion& potion) {
  if (potion.teleports) return "Random teleport";
  if (potion.heal_percent > 0) return "+" + std::to_string(potion.heal_percent) + "% HP";
  const char* stat_name = potion.buff_stat == StatKind::Strength
                               ? "STR"
                               : potion.buff_stat == StatKind::Dexterity ? "DEX" : "INT";
  return "+" + std::to_string(potion.buff_amount) + " " + stat_name + " (" + std::to_string(potion.buff_turns) +
         " turns)";
}

std::string describe_weapon(const Weapon& weapon) {
  std::string desc = std::to_string(weapon.dice_count) + "d" + std::to_string(weapon.dice_sides);
  if (weapon.bonus != 0) desc += "+" + std::to_string(weapon.bonus);
  if (weapon.attack_range > 1) desc += ", range " + std::to_string(weapon.attack_range);
  return desc;
}

std::string describe_armor(const Armor& armor) { return "+" + std::to_string(armor.defense); }

bool give_starting_item(const std::string& name, Actor& actor) {
  for (const auto& w : kWeaponTable) {
    if (w.name == name) {
      actor.weapons.push_back(w);
      return true;
    }
  }
  for (const auto& a : kArmorTable) {
    if (a.name == name) {
      actor.armors.push_back(a);
      return true;
    }
  }
  for (const auto& p : kPotionTable) {
    if (p.name == name) {
      actor.potions.push_back(p);
      return true;
    }
  }
  return false;
}

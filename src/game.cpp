#include "game.hpp"

#include <algorithm>
#include <cmath>

#include "content.hpp"
#include "rng.hpp"
#include "rules.hpp"

void add_message(GameState& gs, const std::string& text) {
  if (text.empty()) return;
  if (!gs.message_log.empty()) {
    std::string& last = gs.message_log.back();
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
  gs.message_log.push_back(text);
}

void level_up_once(GameState& gs) {
  gs.player.level += 1;
  gs.pending_attribute_points += 1;
  // The level-up itself grants HP (see max_hp_for_level_and_strength) regardless of
  // which attribute the point later gets spent on — previously a run that never
  // chose Strength never gained any max HP at all past character creation. Same
  // "current-jump" convention every other permanent max_hp increase already uses
  // (e.g. the LevelUp Shift+S handler below): current HP rises by the same delta,
  // not just the ceiling.
  // Any active temp Strength buff contributed its HP as a delta (see apply_potion),
  // so it has to be re-added on top of the recomputed base or leveling up mid-buff
  // would silently cancel the potion.
  int new_max_hp = max_hp_for_level_and_strength(gs.player.level, gs.player.strength) +
                   gs.player.temp_str_bonus * kHpPerStrength;
  gs.player.hp += new_max_hp - gs.player.max_hp;
  gs.player.max_hp = new_max_hp;
}

void grant_xp(GameState& gs, int amount) {
  gs.player.xp += amount;
  while (gs.player.xp >= xp_needed_for_level(gs.player.level)) {
    gs.player.xp -= xp_needed_for_level(gs.player.level);
    level_up_once(gs);
  }
  // Only surface the prompt if the player is still standing. A kill can land *after*
  // the player has already died this same turn — you die to your own Fireball in
  // advance_projectiles(), and the Sandstorm tick that runs next finishes off a
  // monster — and without this check the resulting level-up would overwrite
  // Mode::Dead, taking the death screen away and letting play continue at <=0 HP.
  // XP itself still accrues; only the mode transition is suppressed.
  //
  // Mode::Win needs the exact same guard for the mirror-image reason: an AoE blast that
  // catches the final boss *and* an ordinary monster in the same explode() call grants
  // XP for both, and the second grant must not overwrite the win screen with a level-up
  // prompt just because the player is (very much) still alive.
  if (gs.pending_attribute_points > 0 && gs.player.is_alive() && gs.mode != Mode::Win) gs.mode = Mode::LevelUp;
}

bool is_game_over(const GameState& gs) { return gs.mode == Mode::Dead || gs.mode == Mode::Win; }

void apply_potion(GameState& gs, Actor& actor, const Potion& potion) {
  Level& level = gs.level();
  // The player narrates each effect in first person below; for anyone else, one line
  // saying what they drank is enough — and only if you can actually see them do it.
  if (!actor.is_player && level.map.is_in_fov(actor.x, actor.y)) {
    add_message(gs, actor_subject(actor) + " drinks a " + potion.name + ".");
  }

  if (potion.heal_percent > 0) {
    int heal_amount = actor.max_hp * potion.heal_percent / 100;
    int before = actor.hp;
    actor.hp = std::min(actor.hp + heal_amount, actor.max_hp);
    if (actor.is_player) {
      add_message(gs, "You drink the " + potion.name + " and recover " + std::to_string(actor.hp - before) + " HP.");
    }
    return;
  }
  if (potion.buff_stat == StatKind::Strength) {
    // Re-drinking while already buffed just refreshes the timer, rather than stacking
    // the bonus indefinitely.
    if (actor.temp_str_turns <= 0) {
      actor.temp_str_bonus = potion.buff_amount;
      actor.max_hp += potion.buff_amount * kHpPerStrength;
    }
    actor.temp_str_turns = potion.buff_turns;
    if (actor.is_player) {
      add_message(gs, "You feel mighty! STR +" + std::to_string(potion.buff_amount) + " for " +
                  std::to_string(potion.buff_turns) + " turns.");
    }
    return;
  }
  if (potion.buff_stat == StatKind::Dexterity) {
    if (actor.temp_dex_turns <= 0) {
      actor.temp_dex_bonus = potion.buff_amount;
      actor.evasion += potion.buff_amount * kDodgePerDexPoint;
    }
    actor.temp_dex_turns = potion.buff_turns;
    if (actor.is_player) {
      add_message(gs, "You feel nimble! DEX +" + std::to_string(potion.buff_amount) + " for " +
                  std::to_string(potion.buff_turns) + " turns.");
    }
    return;
  }
  if (potion.buff_stat == StatKind::Intelligence) {
    if (actor.temp_int_turns <= 0) {
      actor.temp_int_bonus = potion.buff_amount;
      actor.max_mana += max_mana_for_intelligence(actor.intelligence + potion.buff_amount) -
                        max_mana_for_intelligence(actor.intelligence);
    }
    actor.temp_int_turns = potion.buff_turns;
    if (actor.is_player) {
      add_message(gs, "You feel sharp! INT +" + std::to_string(potion.buff_amount) + " for " +
                  std::to_string(potion.buff_turns) + " turns.");
    }
    return;
  }
  if (potion.teleports) {
    std::vector<std::pair<int, int>> occupied;
    for (const auto& m : level.monsters) occupied.push_back({m.x, m.y});
    occupied.push_back({gs.player.x, gs.player.y});
    auto [tx, ty] = random_free_tile(level.map, occupied);
    actor.x = tx;
    actor.y = ty;
    if (actor.is_player) {
      // Not an incremental step, so (unlike normal movement) FOV needs an explicit
      // recompute — same as descend()/ascend() after a floor change.
      level.map.update_fov(gs.player.x, gs.player.y, FOV_RADIUS);
      add_message(gs, "You vanish and reappear elsewhere!");
    }
  }
}

void apply_debuff(GameState& gs, Actor& target, const Spell& spell) {
  // Lands on temp_melee_damage_bonus rather than temp_str_bonus — see Spell::is_debuff
  // for why a negative Strength bonus would be actively wrong. Refresh-don't-stack, the
  // same shape apply_potion() uses: re-cursing mid-duration resets the clock without
  // compounding the penalty.
  if (target.temp_melee_damage_turns <= 0) target.temp_melee_damage_bonus = spell.buff_amount;
  target.temp_melee_damage_turns = spell.buff_turns;
  add_message(gs, actor_subject(target) + actor_verb(target, " wither") + " under the curse.");
}

bool try_actor_use_potion(GameState& gs, Actor& actor, bool enemy_near) {
  if (actor.potions.empty()) return false;
  bool badly_hurt = actor.hp * 100 < actor.max_hp * kAiDrinkHealBelowPercent;
  for (size_t i = 0; i < actor.potions.size(); ++i) {
    const Potion& potion = actor.potions[i];
    bool want = false;
    if (potion.heal_percent > 0) {
      want = badly_hurt;
    } else if (potion.buff_stat == StatKind::Strength) {
      want = enemy_near && actor.temp_str_turns <= 0;
    } else if (potion.buff_stat == StatKind::Dexterity) {
      want = enemy_near && actor.temp_dex_turns <= 0;
    } else if (potion.buff_stat == StatKind::Intelligence) {
      want = false;  // nothing but the player casts, so INT does nothing for a monster
    } else if (potion.teleports) {
      want = badly_hurt;  // a last-ditch escape, same as a cornered player would use it
    }
    if (!want) continue;
    Potion chosen = actor.potions[i];  // copy before erase invalidates the reference
    actor.potions.erase(actor.potions.begin() + static_cast<long>(i));
    apply_potion(gs, actor, chosen);
    return true;
  }
  return false;
}

void on_actor_killed(GameState& gs, Actor& victim, bool killed_by_player_side,
                     const std::string& cause) {
  if (victim.is_player) {
    gs.death_cause = cause;
    append_run_history_entry(
        {/*won=*/false, gs.current_level + 1, gs.player.level, cause, gs.current_seed_display});
    gs.mode = Mode::Dead;
    return;
  }
  // The win condition: whoever/whatever actually landed the blow, killing the final
  // boss ends the run right here — no gear drop, no corpse, no XP, same early-return
  // shape as the is_player branch above, since none of that matters anymore.
  if (victim.monster_template_index >= 0 &&
      kMonsterTable[static_cast<size_t>(victim.monster_template_index)].is_final_boss) {
    gs.win_cause = kMonsterTable[static_cast<size_t>(victim.monster_template_index)].name;
    append_run_history_entry(
        {/*won=*/true, gs.current_level + 1, gs.player.level, gs.win_cause, gs.current_seed_display});
    gs.mode = Mode::Win;
    return;
  }
  drop_actor_gear(gs.level(), victim);

  // A slain monster may leave a body behind, for a Summoner to raise later. Only real
  // *hostile* monsters do — the explicit allegiance check is what actually excludes
  // minions, both kinds of them: a conjured one (Imp/Demon) already fails the
  // monster_template_index >= 0 check below since it comes from kMinionTable, but a
  // *raised* one (spawn_reanimated()) is built via spawn_monster() and keeps a real
  // monster_template_index pointing at the species it was raised from — without this
  // check, a raised Orc dying in combat (as opposed to its duration simply running out,
  // which never reaches on_actor_killed() at all) could leave a fresh Orc corpse of its
  // own, letting a Summoner re-raise the same "life" indefinitely instead of it staying
  // the borrowed time kRaisedMinionTurns is supposed to be.
  if (victim.allegiance != Allegiance::Player && victim.monster_template_index >= 0) {
    const MonsterTemplate& tmpl = kMonsterTable[static_cast<size_t>(victim.monster_template_index)];
    // Two independent exclusions. `is_boss` is automatic and always wins: a boss is the
    // hardest thing on its floor by construction, so handing its stat line back as a
    // permanent minion would make killing one strictly better than intended, and that
    // shouldn't be a rule a future boss row can forget to opt into. Its gear is already
    // the reward. `leaves_corpse` is the per-species knob for ordinary monsters (the
    // Skeleton, which is animated bones and leaves nothing to raise).
    //
    // Either way it's no corpse at all, rather than one that refuses to be raised: a body
    // you can see and inspect but never use would read as a bug.
    // One body per tile: two corpses stacked on the same square would be indistinguishable
    // to both the map glyph and the raise cursor, so a second death on an occupied tile
    // simply leaves nothing rather than creating a body the player can't single out.
    if (!tmpl.is_boss && tmpl.leaves_corpse && corpse_at(gs.level(), victim.x, victim.y) < 0 &&
        random_int(1, 100) <= kCorpseChancePercent) {
      gs.level().corpses.push_back(Corpse{victim.x, victim.y, victim.monster_template_index});
    }
  }

  if (killed_by_player_side) grant_xp(gs, victim.xp_reward);
}

void resolve_attack(GameState& gs, Actor& attacker, Actor& defender, const Weapon& weapon) {
  if (random_int(1, 100) <= dodge_chance(defender, attacker, weapon)) {
    add_message(gs, actor_subject(defender) + actor_verb(defender, " dodge") + " " + actor_possessive(attacker) +
                " attack!");
    return;
  }

  int raw_damage = roll_damage(weapon) + damage_bonus_for(attacker, weapon);
  // Battle Fury: flat melee-only damage, not accuracy. resolve_attack() is also how
  // a ranged monster (e.g. a Goblin Slinger's Rock) resolves its attack — melee-only
  // is what keeps this off of that case too, not "player only."
  if (weapon.attack_range <= 1) raw_damage += attacker.temp_melee_damage_bonus;
  int damage = std::max(raw_damage - defender.armor.defense - defender.temp_armor_bonus, 0);
  defender.hp -= damage;

  std::string wielder = attacker.is_player ? "your " : "its ";
  if (defender.is_alive()) {
    add_message(gs, actor_subject(attacker) + actor_verb(attacker, " hit") + " " + actor_object(defender) + " with " +
                wielder + weapon.name + " for " + std::to_string(damage) + ".");
    return;
  }
  add_message(gs, actor_subject(attacker) + actor_verb(attacker, " slay") + " " + actor_object(defender) + " with " +
              wielder + weapon.name + "!");
  on_actor_killed(gs, defender, attacker.is_player || attacker.allegiance == Allegiance::Player, attacker.name);
}

void advance_projectiles(GameState& gs, bool instant_only) {
  Level& level = gs.level();

  // Deals independently-rolled damage to every living monster within aoe_radius tiles
  // (Chebyshev distance, so a radius of 1 is a 3x3 box) of (cx,cy) — same per-target
  // math as the single-target hit below, just applied to more than one target. Used
  // for spells with aoe_radius > 0 (e.g. Fireball); see find_impact() for how (cx,cy)
  // is chosen to match what the Targeting preview showed the player.
  //
  // The caster's own side isn't exempt from its own blast: if the player is within
  // radius of a blast they own (a point-blank cast, or a wall/monster close enough that
  // the impact lands next to them), they take the same roll. This is the actual
  // deterrent against casting an AoE on something adjacent. Their armor soaks it like
  // anyone else's would, but they get no dodge roll — you can't evade your own
  // point-blank explosion. Someone *else's* blast is an ordinary attack and does get a
  // dodge roll, which is the one place the two cases differ.
  auto explode = [&](Projectile& proj, int cx, int cy) {
    bool from_player_side = proj.owner_allegiance == Allegiance::Player;
    add_message(gs, projectile_subject(proj) + " explodes!");
    for (auto& target : level.monsters) {
      // A blast never catches its own side: the player's own minions stand in their
      // Fireball untouched, and a hostile caster's blast likewise spares other
      // hostiles. Same rule from either direction, no longer hard-coded to one.
      if (target.allegiance == proj.owner_allegiance || !target.is_alive()) continue;
      if (std::abs(target.x - cx) > proj.aoe_radius || std::abs(target.y - cy) > proj.aoe_radius) continue;

      int dodge = dodge_chance_vs_accuracy(
          target, proj.accuracy_bonus + roll_dice(proj.hit_dice_count, proj.hit_dice_sides));
      if (random_int(1, 100) <= dodge) {
        add_message(gs, actor_subject(target) + actor_verb(target, " dodge") + " the blast!");
        continue;
      }
      int damage = std::max(
          roll_dice(proj.dice_count, proj.dice_sides) + proj.bonus - target.armor.defense - target.temp_armor_bonus,
          0);
      target.hp -= damage;
      if (!target.is_alive()) {
        add_message(gs, "The blast kills " + actor_object(target) + "!");
        on_actor_killed(gs, target, from_player_side, proj.name);
        continue;
      }
      add_message(gs, "The blast hits " + actor_object(target) + " for " + std::to_string(damage) + ".");
    }

    // The player is never in level.monsters, so they need their own check either way.
    if (gs.player.is_alive() && std::abs(gs.player.x - cx) <= proj.aoe_radius && std::abs(gs.player.y - cy) <= proj.aoe_radius) {
      bool hit = true;
      if (!from_player_side) {
        // Someone else's blast: an ordinary attack, so it can be dodged.
        int dodge = dodge_chance_vs_accuracy(
            gs.player, proj.accuracy_bonus + roll_dice(proj.hit_dice_count, proj.hit_dice_sides));
        if (random_int(1, 100) <= dodge) {
          add_message(gs, "You dodge the blast!");
          hit = false;
        }
      }
      if (hit) {
        int raw_damage = roll_dice(proj.dice_count, proj.dice_sides) + proj.bonus;
        int damage = std::max(raw_damage - gs.player.armor.defense - gs.player.temp_armor_bonus, 0);
        gs.player.hp -= damage;
        if (from_player_side) {
          add_message(gs, "You're caught in your own " + proj.name + " for " + std::to_string(damage) + "!");
        } else {
          add_message(gs, "The blast hits you for " + std::to_string(damage) + "!");
        }
        if (!gs.player.is_alive()) {
          on_actor_killed(gs, gs.player, /*killed_by_player_side=*/false,
                          from_player_side ? proj.name + " you cast" : projectile_possessive(proj) + " " + proj.name);
        }
      }
    }
  };

  // One projectile hit against one target, used by both the walk below's monster branch
  // and its player branch — dodge roll, armor-reduced damage, death, and the three log
  // lines, all phrased from the projectile's owner rather than assuming it's yours.
  auto resolve_projectile_hit = [&](const Projectile& proj, Actor& target) {
    int dodge = dodge_chance_vs_accuracy(
        target, proj.accuracy_bonus + roll_dice(proj.hit_dice_count, proj.hit_dice_sides));
    if (random_int(1, 100) <= dodge) {
      add_message(gs, actor_subject(target) + actor_verb(target, " dodge") + " " + projectile_possessive(proj) + " " +
                  proj.name + "!");
      return;
    }
    int damage = std::max(
        roll_dice(proj.dice_count, proj.dice_sides) + proj.bonus - target.armor.defense - target.temp_armor_bonus, 0);
    target.hp -= damage;
    if (!target.is_alive()) {
      add_message(gs, projectile_subject(proj) + " kills " + actor_object(target) + "!");
      on_actor_killed(gs, target, proj.owner_allegiance == Allegiance::Player, proj.name);
      return;
    }
    add_message(gs, projectile_subject(proj) + " hits " + actor_object(target) + " for " + std::to_string(damage) + ".");
  };

  for (size_t i = 0; i < level.projectiles.size();) {
    Projectile& proj = level.projectiles[i];
    if (instant_only && proj.speed < kInstantSpellSpeed) {
      ++i;  // leave it exactly where it is; it moves on the next real world turn
      continue;
    }
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

      if (level.map.blocks_projectile(x, y)) {
        if (proj.aoe_radius > 0) {
          explode(proj, proj.prev_x, proj.prev_y);  // last open tile before the wall
        } else {
          add_message(gs, projectile_subject(proj) + " fizzles against a wall.");
        }
        consumed = true;
        break;
      }

      // The player's own tile. Checked separately from the monster lookup below purely
      // because the player isn't in level.monsters, and only for an enemy shot — your
      // own spells pass straight over you, with the sole exception of an AoE's splash,
      // which explode() handles on its own terms.
      if (proj.owner_allegiance == Allegiance::Hostile && gs.player.is_alive() && x == gs.player.x && y == gs.player.y) {
        if (proj.aoe_radius > 0) {
          explode(proj, x, y);
          consumed = true;
        } else {
          resolve_projectile_hit(proj, gs.player);
          if (!proj.pierces) consumed = true;
        }
        if (consumed) break;
      }

      int target_index = projectile_target_at(level.monsters, x, y, proj.owner_allegiance);
      if (target_index >= 0) {
        if (proj.aoe_radius > 0) {
          explode(proj, x, y);  // the target's own tile is a valid, walkable center
          consumed = true;
        } else {
          resolve_projectile_hit(proj, level.monsters[static_cast<size_t>(target_index)]);
          // A piercing spell (Lightning Bolt) keeps traveling through a hit instead of
          // stopping — everything along the line takes its own independently-rolled
          // dodge/damage, the same idiom explode() already uses for multiple targets,
          // just walked one tile at a time instead of scanned by radius.
          if (!proj.pierces) consumed = true;
        }
        if (consumed) break;
      }

      // Empty, walkable tile — or a hostile tile a piercing spell just passed
      // through without stopping: the spell keeps going. Remember it as the last
      // open tile in case the very next one stops it (the aoe_radius wall-hit case
      // above).
      proj.prev_x = x;
      proj.prev_y = y;
    }

    if (consumed) {
      level.projectiles.erase(level.projectiles.begin() + static_cast<long>(i));
    } else {
      ++i;
    }
  }
}

void pick_up_ground_items(GameState& gs, const std::vector<ItemSlot>& slots) {
  Level& level = gs.level();

  // Collect indices per kind first, then erase in descending order. Erasing as we go
  // would shift every later index in that vector down by one and start taking the wrong
  // items — the same hazard the old single-pass loop avoided by walking backwards.
  std::vector<int> weapons, armors, potions;
  for (const ItemSlot& slot : slots) {
    if (slot.kind == ItemKind::Weapon) weapons.push_back(slot.index);
    else if (slot.kind == ItemKind::Armor) armors.push_back(slot.index);
    else potions.push_back(slot.index);
  }

  // Messages are emitted in the order shown on screen, not erase order, so the log reads
  // the way the menu did.
  for (int i : weapons) {
    add_message(gs, "You pick up a " + level.items[static_cast<size_t>(i)].weapon.name + ". Press 'w' to equip.");
    gs.player.weapons.push_back(level.items[static_cast<size_t>(i)].weapon);
  }
  for (int i : armors) {
    add_message(gs, "You pick up a " + level.armor_items[static_cast<size_t>(i)].armor.name + ". Press 'a' to equip.");
    gs.player.armors.push_back(level.armor_items[static_cast<size_t>(i)].armor);
  }
  for (int i : potions) {
    add_message(gs, "You pick up a " + level.potions[static_cast<size_t>(i)].potion.name + ". Press 'q' to drink.");
    gs.player.potions.push_back(level.potions[static_cast<size_t>(i)].potion);
  }

  std::sort(weapons.rbegin(), weapons.rend());
  std::sort(armors.rbegin(), armors.rend());
  std::sort(potions.rbegin(), potions.rend());
  for (int i : weapons) level.items.erase(level.items.begin() + i);
  for (int i : armors) level.armor_items.erase(level.armor_items.begin() + i);
  for (int i : potions) level.potions.erase(level.potions.begin() + i);
}

void start_new_game(GameState& gs) {
  gs.levels.clear();
  gs.levels.push_back(generate_level(MAP_WIDTH, MAP_HEIGHT, /*has_stairs_up=*/false, /*depth=*/1));
  gs.current_level = 0;

  Level& level = gs.level();
  gs.player.x = level.entry_x;
  gs.player.y = level.entry_y;
  gs.player.strength = 2;
  gs.player.dexterity = 2;
  gs.player.intelligence = 2;
  gs.player.level = 1;
  gs.player.xp = 0;
  gs.player.max_hp = max_hp_for_level_and_strength(gs.player.level, gs.player.strength);
  gs.player.hp = gs.player.max_hp;
  // Derived from Dexterity, where a monster's is read straight off its table row —
  // both land in the same Actor::evasion the one dodge formula reads.
  gs.player.evasion = evasion_for_dexterity(gs.player.dexterity);
  gs.player.melee_engaged = false;
  // The player is the one Actor that regenerates; every monster/minion table row
  // leaves hp_regen_turns at 0. See Actor::hp_regen_turns.
  gs.player.hp_regen_turns = kHpRegenTurns;
  gs.player.hp_regen_accumulator = 0.0f;
  gs.player.mana_regen_turns = kManaRegenTurns;  // gated now (see Actor::mana_regen_turns)
  gs.player.max_mana = max_mana_for_intelligence(gs.player.intelligence);
  gs.player.mana = gs.player.max_mana;
  gs.player.mana_regen_accumulator = 0.0f;
  gs.player.weapon = kFists;
  gs.player.armor = kNoArmor;
  gs.player.temp_str_bonus = 0;
  gs.player.temp_str_turns = 0;
  gs.player.temp_dex_bonus = 0;
  gs.player.temp_dex_turns = 0;
  gs.player.temp_int_bonus = 0;
  gs.player.temp_int_turns = 0;
  gs.player.temp_melee_damage_bonus = 0;
  gs.player.temp_melee_damage_turns = 0;
  gs.player.temp_armor_bonus = 0;
  gs.player.temp_armor_turns = 0;
  gs.player.temp_extra_actions_bonus = 0;
  gs.player.temp_extra_actions_turns = 0;
  gs.free_actions_used = 0;  // session state, but a restart must not inherit a part-spent turn
  level.map.update_fov(gs.player.x, gs.player.y, FOV_RADIUS);

  gs.player.weapons.clear();
  gs.player.armors.clear();
  gs.player.potions.clear();
  gs.pending_attribute_points = 0;
  gs.active_toggle_spell = -1;
  gs.message_log.clear();
  add_message(gs, "Welcome to the dungeon. Press '?' for controls.");
  gs.mode = Mode::Playing;
}

void move_minions_to_new_floor(GameState& gs, Level& from_level, Level& to_level) {
  for (size_t i = 0; i < from_level.monsters.size();) {
    if (from_level.monsters[i].allegiance != Allegiance::Player) {
      ++i;
      continue;
    }
    Actor minion = from_level.monsters[i];
    from_level.monsters.erase(from_level.monsters.begin() + static_cast<long>(i));
    int nx, ny;
    if (free_adjacent_tile(to_level.map, to_level.monsters, gs.player.x, gs.player.y, nx, ny)) {
      minion.x = nx;
      minion.y = ny;
    } else {
      std::vector<std::pair<int, int>> occupied = {{gs.player.x, gs.player.y}};
      for (const auto& m : to_level.monsters) occupied.push_back({m.x, m.y});
      auto [rx, ry] = random_free_tile(to_level.map, occupied);
      minion.x = rx;
      minion.y = ry;
    }
    to_level.monsters.push_back(minion);
    // Don't advance i — the erase above shifted the next element into slot i.
  }
}

void descend(GameState& gs) {
  int old_index = gs.current_level;
  gs.current_level += 1;
  if (static_cast<size_t>(gs.current_level) >= gs.levels.size()) {
    gs.levels.push_back(generate_level(MAP_WIDTH, MAP_HEIGHT, /*has_stairs_up=*/true, /*depth=*/gs.current_level + 1));
  }
  // Both re-fetched fresh, after the possible push_back above — which can reallocate
  // `levels` and would dangle a reference taken any earlier (same hazard noted where
  // the render loop re-fetches `level` per event).
  Level& old_level = gs.levels[static_cast<size_t>(old_index)];
  Level& level = gs.level();
  gs.player.x = level.entry_x;
  gs.player.y = level.entry_y;
  move_minions_to_new_floor(gs, old_level, level);
  level.map.update_fov(gs.player.x, gs.player.y, FOV_RADIUS);
  add_message(gs, "You descend the stairs.");
}

void ascend(GameState& gs) {
  int old_index = gs.current_level;
  gs.current_level -= 1;
  Level& old_level = gs.levels[static_cast<size_t>(old_index)];
  Level& level = gs.level();
  gs.player.x = level.stairs_down_x;
  gs.player.y = level.stairs_down_y;
  move_minions_to_new_floor(gs, old_level, level);
  level.map.update_fov(gs.player.x, gs.player.y, FOV_RADIUS);
  add_message(gs, "You ascend the stairs.");
}

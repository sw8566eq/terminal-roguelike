#include "turn.hpp"

#include <algorithm>
#include <cmath>

#include "content.hpp"
#include "rng.hpp"
#include "rules.hpp"

namespace {

  // Per-turn upkeep, run identically for every living Actor on the floor: passive HP
  // and mana regen, then any temporary stat buff counting down. A monster that drank a
  // Potion of Strength loses it on exactly the same schedule the player would.
  //
  // Regen is per-Actor and opt-in (Actor::hp_regen_turns, 0 = doesn't regenerate):
  // the player and the Orc Warlord are the only two that heal, so an ordinary
  // monster's wounds still stick and chip-and-retreat still works on it. The rate
  // scales with max HP, so a full heal takes hp_regen_turns turns regardless of how
  // big the pool is. Silent — no log message — since it ticks often enough that
  // logging it would just spam. Mana is gated the same way (Actor::mana_regen_turns) —
  // it used not to need one, back when the player was the only Actor with a pool at
  // all, but the Goblin Shaman has a finite 10-mana budget that's meant to stay spent.
  //
  // Note this only runs for Actors on the *current* floor. That's now observable: the
  // Orc Warlord (hp_regen_turns=120, the only non-zero row) heals back the damage you
  // leave on it if you retreat *within* floor 3, but not while you're on another
  // floor — consistent with it not acting while you're away either. So stairs remain a
  // way to break off a losing boss fight; backing into a corridor is not.
  //
  // Each expiring buff removes exactly the delta apply_potion() added, rather than
  // recomputing a ceiling from the attribute — that's what lets the same code undo a
  // buff on the player (whose ceilings are derived from attributes) and on a monster
  // (whose are authored in its table row).
void tick_upkeep(GameState& gs, Actor& actor) {
    if (!actor.is_alive()) return;

    if (actor.hp_regen_turns > 0 && actor.hp < actor.max_hp) {
      actor.hp_regen_accumulator += static_cast<float>(actor.max_hp) / static_cast<float>(actor.hp_regen_turns);
      while (actor.hp_regen_accumulator >= 1.0f && actor.hp < actor.max_hp) {
        actor.hp_regen_accumulator -= 1.0f;
        actor.hp += 1;
      }
    }
    if (actor.mana_regen_turns > 0 && actor.mana < actor.max_mana) {
      // actor.mana_regen_turns, not the kManaRegenTurns constant — the field is a rate,
      // exactly like hp_regen_turns above, not just an on/off gate. Reading the constant
      // here happened to be invisible because the player is the only Actor that regens
      // mana and start_new_game() sets theirs *to* kManaRegenTurns; a monster row with a
      // different rate would silently have regenerated at the player's.
      actor.mana_regen_accumulator += static_cast<float>(actor.max_mana) / static_cast<float>(actor.mana_regen_turns);
      while (actor.mana_regen_accumulator >= 1.0f && actor.mana < actor.max_mana) {
        actor.mana_regen_accumulator -= 1.0f;
        actor.mana += 1;
      }
    }

    if (actor.temp_str_turns > 0 && --actor.temp_str_turns == 0) {
      actor.max_hp -= actor.temp_str_bonus * kHpPerStrength;
      actor.hp = std::min(actor.hp, actor.max_hp);  // clamp in case regen filled past the new, lower ceiling
      actor.temp_str_bonus = 0;
      if (actor.is_player) add_message(gs, "Your surge of strength fades.");
    }
    if (actor.temp_dex_turns > 0 && --actor.temp_dex_turns == 0) {
      actor.evasion -= actor.temp_dex_bonus * kDodgePerDexPoint;
      actor.temp_dex_bonus = 0;
      if (actor.is_player) add_message(gs, "Your surge of agility fades.");
    }
    if (actor.temp_int_turns > 0 && --actor.temp_int_turns == 0) {
      actor.max_mana -= max_mana_for_intelligence(actor.intelligence + actor.temp_int_bonus) -
                        max_mana_for_intelligence(actor.intelligence);
      actor.mana = std::min(actor.mana, actor.max_mana);  // clamp past the new, lower ceiling
      actor.temp_int_bonus = 0;
      if (actor.is_player) add_message(gs, "Your surge of insight fades.");
    }
    // Combat Mage buffs (Battle Fury / Iron Skin) — simpler than STR/DEX/INT above,
    // since neither feeds a derived ceiling; reverting is just zeroing the bonus.
    if (actor.temp_melee_damage_turns > 0 && --actor.temp_melee_damage_turns == 0) {
      actor.temp_melee_damage_bonus = 0;
      if (actor.is_player) add_message(gs, "Your battle fury fades.");
    }
    if (actor.temp_armor_turns > 0 && --actor.temp_armor_turns == 0) {
      actor.temp_armor_bonus = 0;
      if (actor.is_player) add_message(gs, "Your iron skin fades.");
    }
    // Haste. Nothing to unwind beyond zeroing the bonus: free_actions_used is reset at
    // the top of every real turn (see end_turn()'s guard), and this only ever runs on a
    // real turn, so an expiry can't strand a half-spent action budget.
    if (actor.temp_extra_actions_turns > 0 && --actor.temp_extra_actions_turns == 0) {
      actor.temp_extra_actions_bonus = 0;
      if (actor.is_player) add_message(gs, "You slow back to normal speed.");
    }
}

  // Minion duration: a timed minion (duration_turns > 0, see MinionTemplate) expires
  // on its own once it hits 0, same "tick down, then resolve" shape as the temp stat
  // buffs above. A permanent minion (duration_turns <= 0, the default) never enters
  // this countdown at all. Marked via hp = 0 rather than erased here — same deferred-
  // sweep reasoning as the AI loops below, and it also means an expiring minion
  // doesn't get to act this same turn (is_alive() already gates both AI loops).
void tick_minion_durations(GameState& gs) {
  Level& level = gs.level();
  for (auto& m : level.monsters) {
    if (m.allegiance != Allegiance::Player || !m.is_alive() || m.duration_turns <= 0) continue;
    m.duration_turns -= 1;
    if (m.duration_turns == 0) {
      add_message(gs, "Your " + m.name + " collapses into dust.");
      m.hp = 0;
      drop_actor_gear(level, m);  // anything it was carrying outlives it, same as a kill
    }
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
  // this only starts draining/damaging on the turn after. Skipped entirely once the
  // player is dead — they can die earlier in this same turn (their own Fireball, in
  // advance_projectiles() above), and a corpse's aura shouldn't keep draining mana and
  // killing things. The two AI loops below carry the same guard.
void tick_toggle_spell(GameState& gs) {
  Level& level = gs.level();
  if (gs.active_toggle_spell >= 0 && !is_game_over(gs)) {
    const Spell& storm = kSpellTable[static_cast<size_t>(gs.active_toggle_spell)];
    if (gs.player.mana < storm.tick_mana_cost) {
      add_message(gs, "Your " + storm.name + " dies down - out of mana.");
      gs.active_toggle_spell = -1;
    } else {
      gs.player.mana -= storm.tick_mana_cost;
      for (auto& target : level.monsters) {
        // Minions stand in the storm untouched — see explode()'s identical
        // exemption for Fireball's blast.
        if (target.allegiance == Allegiance::Player || !target.is_alive()) continue;
        if (std::abs(target.x - gs.player.x) > storm.aoe_radius || std::abs(target.y - gs.player.y) > storm.aoe_radius) {
          continue;
        }
        int dodge = dodge_chance_vs_accuracy(
            target, accuracy_roll(gs.player, storm.hit_dice_count, storm.hit_dice_sides));
        if (random_int(1, 100) <= dodge) {
          add_message(gs, "The " + target.name + " dodges the " + storm.name + "!");
          continue;
        }
        int damage = std::max(storm.tick_damage - target.armor.defense - target.temp_armor_bonus, 0);
        target.hp -= damage;
        if (!target.is_alive()) {
          add_message(gs, "Your " + storm.name + " kills the " + target.name + "!");
          on_actor_killed(gs, target, /*killed_by_player_side=*/true, storm.name);
          continue;
        }
        add_message(gs, "Your " + storm.name + " hits the " + target.name + " for " + std::to_string(damage) + ".");
      }
    }
  }
}

  // Tries to step a monster by (step_dx, step_dy); does nothing and returns false if
  // that tile is a wall, already has another living monster on it, or is the
  // player's own tile (the player isn't in level.monsters, so that needs its own
  // check — no monster/minion ever displaces the player by walking into them; the
  // player initiates all bump-to-attack contact, never the other way around).
bool try_monster_step(GameState& gs, Actor& m, int step_dx, int step_dy) {
  Level& level = gs.level();
    if (step_dx == 0 && step_dy == 0) return false;
    int nx = m.x + step_dx;
    int ny = m.y + step_dy;
    if (!level.map.is_walkable(nx, ny)) return false;
    if (nx == gs.player.x && ny == gs.player.y) return false;
    for (const auto& other : level.monsters) {
      if (&other != &m && other.is_alive() && other.x == nx && other.y == ny) return false;
    }
    m.x = nx;
    m.y = ny;
    return true;
}

// Every living hostile monster takes its turn.
void run_hostile_ai(GameState& gs) {
  Level& level = gs.level();
  for (auto& monster : level.monsters) {
    if (is_game_over(gs)) break;  // the player already died, or already won, earlier this turn
    if (!monster.is_alive() || monster.allegiance != Allegiance::Hostile) continue;
    // Extra actions: an ordinary monster runs this body once, and a boss/elite row
    // with extra_actions > 0 runs it again for each extra. Every "this creature is
    // done" exit inside the body is already a `continue`, which inside this inner loop
    // reads correctly as "this action is spent, take the next one" — so a two-action
    // monster can drink a potion and then attack, or attack twice, or close the
    // distance and then swing. total_actions_for() is the same function the player's
    // own extra actions come from (see end_turn()'s free-action guard).
    for (int action = 0; action < total_actions_for(monster); ++action) {
      // Re-checked per action, not just per monster: the player can die to this
      // monster's first swing, and nothing should get a second one after that. Also
      // covers a Win set mid-loop — e.g. one monster's earlier action killed the final
      // boss via a stray AoE splash — so no other hostile gets a free turn afterward.
      if (is_game_over(gs) || !monster.is_alive()) break;

      // Picks this monster's target for the turn: the player, or the closest living
      // minion whose tile is currently in the player's FOV (there's no separate
      // per-monster FOV; "lit right now" — the same is_in_fov() check already used to
      // decide whether to even render a minion — stands in for "this monster would
      // notice it"). Ties favor the player. Minions don't get their own "remembered
      // last position" the way the player does (last_seen_player_x/y below is still
      // player-specific) — a Phase 1 simplification. With zero minions on the floor
      // this always resolves to the player, so solo-player behavior is unchanged.
      Actor* target = &gs.player;
      int best_dist = distance_between(monster, gs.player);
      for (auto& candidate : level.monsters) {
        if (candidate.allegiance != Allegiance::Player || !candidate.is_alive()) continue;
        if (!level.map.is_in_fov(candidate.x, candidate.y)) continue;
        int dist = distance_between(monster, candidate);
        if (dist < best_dist) {
          best_dist = dist;
          target = &candidate;
        }
      }

      // Perception, recorded before this monster decides what to *do*. Seeing where the
      // player is isn't an action, so it has to happen ahead of every branch below that
      // ends in `continue` — a caster spends every turn it can see you casting, and a
      // Slinger shooting, and if the memory were only written further down (in the chase
      // block, where it used to live) neither would ever record anything. They'd then
      // have last_seen_player_x == -1 the moment you broke line of sight, and wander
      // instead of following. Reuses the player's own FOV as the mutual-visibility
      // proxy, same stand-in for per-monster sight as everywhere else in this loop.
      bool can_see_player = level.map.is_in_fov(monster.x, monster.y);
      if (can_see_player) {
        monster.last_seen_player_x = gs.player.x;
        monster.last_seen_player_y = gs.player.y;
      }

      // Gear and consumables, decided before anything else this turn and using the same
      // code the player's own menus drive. Swapping to whichever carried weapon suits
      // the current distance is free (it's a draw, not a turn); actually drinking
      // something costs the turn, exactly as it does for the player.
      equip_best_weapon_for_range(monster, best_dist);
      if (try_actor_use_potion(gs, monster, /*enemy_near=*/best_dist <= kAiBuffPotionRange)) continue;

      // Spellcasting, for a monster whose row gave it one or more spells
      // (Actor::spell_indices — the Goblin Shaman's one dart, the Orc Wizard's two).
      // Deliberately built from the same pieces the player's own cast is: it pays
      // kSpellTable's mana_cost out of a real pool, and it pushes a real Projectile onto
      // level.projectiles that travels, is drawn on the map, and rolls dodge/damage in
      // advance_projectiles() like any other shot. Nothing here resolves damage — that's
      // the whole difference from a Goblin Slinger's Rock, which lands instantly inside
      // this loop with nothing to see or react to.
      //
      // With more than one known spell, the AI scores every affordable, in-range one by
      // expected_spell_damage() (rules.hpp) and casts whichever scores highest — the
      // same "score every option, take the best" idiom equip_best_weapon_for_range()
      // already uses for weapons. A single-spell caster (the Shaman) just always has one
      // candidate, so this collapses to the old behavior for it.
      //
      // Preferred over melee whenever anything is affordable and in range, so a caster
      // opens at range and only reverts to swinging its melee weapon once its pool is
      // dry. A hit-scan spell resolves in this same turn, via the advance_projectiles()
      // call just after this loop — see the comment there. It never appears on the map,
      // for the same reason the player's own Magic Dart doesn't: instant means instant.
      int best_spell_index = -1;
      double best_spell_score = -1.0;
      for (int candidate : monster.spell_indices) {
        const Spell& candidate_spell = kSpellTable[static_cast<size_t>(candidate)];
        // The is_in_fov() term keeps a caster from sniping out of the dark. Range is
        // Chebyshev here (like every other AI reach check) while FOV_RADIUS is radial,
        // so a diagonal caster at range 8 would otherwise sit outside the player's
        // sight while shooting into it — which would undercut the whole point of
        // making monster fire visible. The existing Goblin Slinger has this property
        // for free at range 5 (its furthest diagonal is still inside FOV_RADIUS); this
        // just makes a longer-ranged caster match it rather than get an exception.
        // Reuses the player's own FOV as the mutual-visibility proxy, the same stand-in
        // for per-monster sight used everywhere else in this loop.
        bool candidate_castable = monster.mana >= candidate_spell.mana_cost && best_dist > 0 &&
                                   best_dist <= candidate_spell.range && level.map.is_in_fov(monster.x, monster.y) &&
                                   line_clear(monster.x, monster.y, target->x, target->y, level.map);
        if (!candidate_castable) continue;
        double score = expected_spell_damage(monster, candidate_spell);
        if (score > best_spell_score) {
          best_spell_score = score;
          best_spell_index = candidate;
        }
      }
      if (best_spell_index >= 0) {
        const Spell& spell = kSpellTable[static_cast<size_t>(best_spell_index)];
        monster.mana -= spell.mana_cost;
        Projectile proj;
        proj.path = trace_path(monster.x, monster.y, target->x, target->y);
        proj.speed = spell.speed;
        proj.dice_count = spell.dice_count;
        proj.dice_sides = spell.dice_sides;
        proj.hit_dice_count = spell.hit_dice_count;
        proj.hit_dice_sides = spell.hit_dice_sides;
        proj.aoe_radius = spell.aoe_radius;
        proj.pierces = spell.pierces;
        proj.prev_x = monster.x;
        proj.prev_y = monster.y;
        // Same two snapshots the player's cast takes, off the caster's own stats —
        // spell damage from INT/3, accuracy from Dexterity — so a monster's dart is
        // built by the identical formula the player's is.
        proj.bonus = (monster.intelligence + monster.temp_int_bonus) / 3;
        proj.accuracy_bonus = (monster.dexterity + monster.temp_dex_bonus) * kAccuracyPerDexPoint;
        proj.name = spell.name;
        proj.glyph = spell.glyph;
        proj.color = spell.color;
        proj.owner_allegiance = monster.allegiance;
        proj.owner_is_player = false;
        proj.owner_name = monster.name;
        level.projectiles.push_back(proj);
        add_message(gs, actor_subject(monster) + actor_verb(monster, " cast") + " " + spell.name + ".");
        continue;  // casting is this monster's whole action
      }

      // "In range" is just the equipped weapon's reach — the same field that decides
      // whether the player can fire what they're holding. A wall between the two still
      // blocks it (line_clear()), which at range 1 is always trivially true, so melee is
      // unaffected by that check. Once melee_engaged (see Actor), a ranged monster's
      // reach collapses to 1: it snipes right up until its target reaches it, then
      // fights like any other melee monster — until the target gets beyond everything it
      // carries, at which point the commitment lifts and it re-arms (see
      // equip_best_weapon_for_range()).
      int effective_range = monster.melee_engaged ? 1 : monster.weapon.attack_range;
      bool in_range = best_dist <= effective_range && best_dist > 0;
      if (in_range && line_clear(monster.x, monster.y, target->x, target->y, level.map)) {
        resolve_attack(gs, monster, *target, monster.weapon);
        continue;
      }

      int tgt_x = target->x;
      int tgt_y = target->y;
      bool target_is_player = target->is_player;

      // Out of range (or no line of sight): chase toward the chosen target if it's
      // currently visible — for the player specifically, "visible" means the
      // FOV-reciprocity check (can_see_player, recorded up at the top of this turn);
      // a minion target was already required to be in_fov to be picked as the target
      // at all, above. Otherwise, if the monster still remembers where it last saw the
      // player specifically, head there instead of immediately giving up — once it
      // arrives and the player isn't there, the memory clears and it falls back to idle
      // wandering. Movement follows a real A* path (Map::find_path(), libtcod's
      // TCODPath) recomputed fresh every turn — cheap enough at this map size that
      // there's no need to cache it turn-to-turn — so a monster routes around a wall
      // segment instead of pacing against it.
      int move_dx = 0;
      int move_dy = 0;
      bool can_see_target = target_is_player ? can_see_player : true;  // minion targets are always is_in_fov, see above
      bool has_chase_target = can_see_target || monster.last_seen_player_x >= 0;
      if (has_chase_target) {
        int chase_x = can_see_target ? tgt_x : monster.last_seen_player_x;
        int chase_y = can_see_target ? tgt_y : monster.last_seen_player_y;
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
      if (!try_monster_step(gs, monster, move_dx, move_dy)) {
        if (!try_monster_step(gs, monster, move_dx, 0)) try_monster_step(gs, monster, 0, move_dy);
      }
    }
  }
}

  // Distance to the nearest living hostile, or -1 if there are none left on the floor.
  // Used to decide whether a minion should draw a melee weapon or pop a buff potion,
  // the same two questions the hostile loop above asks about its own target.
int nearest_hostile_distance(GameState& gs, const Actor& minion) {
  Level& level = gs.level();
    int best = -1;
    for (const auto& hostile : level.monsters) {
      if (hostile.allegiance != Allegiance::Hostile || !hostile.is_alive()) continue;
      int dist = distance_between(minion, hostile);
      if (best < 0 || dist < best) best = dist;
    }
    return best;
}

  // Attacks the closest hostile within reach of the minion's equipped weapon
  // (line_clear()'d), if any — the "still defend yourself" half of Follow and Hold, so
  // a minion doing either isn't a free hit for anything that wanders adjacent. Returns
  // whether it attacked (the caller should skip movement for the turn if so).
bool try_minion_auto_defend(GameState& gs, Actor& minion) {
  Level& level = gs.level();
    Actor* best_hostile = nullptr;
    int best_dist = 0;
    for (auto& hostile : level.monsters) {
      if (hostile.allegiance != Allegiance::Hostile || !hostile.is_alive()) continue;
      int dist = distance_between(minion, hostile);
      if (dist > minion.weapon.attack_range) continue;
      if (!line_clear(minion.x, minion.y, hostile.x, hostile.y, level.map)) continue;
      if (best_hostile == nullptr || dist < best_dist) {
        best_hostile = &hostile;
        best_dist = dist;
      }
    }
    if (best_hostile == nullptr) return false;
    resolve_attack(gs, minion, *best_hostile, minion.weapon);
    return true;
}

  // The player's minions act after every hostile monster has had its turn. Each one
  // is Following (path toward the player, ignoring FOV — a summoned minion always
  // knows where its own summoner is, unlike a hostile monster tracking the player),
  // Holding (path toward and then stand at a specific tile — "guard this spot"),
  // AttackTarget (path toward/attack one specific enemy, by id), or Aggressive
  // (Follow's proactive sibling — see the MinionOrder doc comment in entity.hpp).
  // Follow, Hold, and Aggressive all defend themselves via try_minion_auto_defend(gs, )
  // instead of moving if a hostile is already in range. An AttackTarget minion whose
  // target has died or otherwise disappeared (actor_index_by_id returns -1) reverts
  // to Follow and just holds position for the rest of this turn, picking up the
  // chase next turn.
void run_minion_ai(GameState& gs) {
  Level& level = gs.level();
  for (auto& minion : level.monsters) {
    if (is_game_over(gs)) break;
    if (!minion.is_alive() || minion.allegiance != Allegiance::Player) continue;
    // Same extra-actions inner loop the hostile loop above uses, for exactly the same
    // reason — a minion is an Actor, so a fast summon (MinionTemplate::extra_actions,
    // 0 on every row today) gets its extra actions through the same one function the
    // player and every monster do.
    for (int action = 0; action < total_actions_for(minion); ++action) {
      if (is_game_over(gs) || !minion.is_alive()) break;

      // Same gear/consumable upkeep the hostile loop runs, through the same helpers —
      // a minion carrying a spare weapon or a potion uses it on exactly the same terms
      // a monster does.
      int hostile_dist = nearest_hostile_distance(gs, minion);
      if (hostile_dist >= 0) equip_best_weapon_for_range(minion, hostile_dist);
      if (try_actor_use_potion(gs, minion, /*enemy_near=*/hostile_dist >= 0 && hostile_dist <= kAiBuffPotionRange)) {
        continue;
      }

      if (minion.order == MinionOrder::AttackTarget) {
        int ti = actor_index_by_id(level.monsters, minion.attack_target_id);
        if (ti < 0) {
          minion.order = MinionOrder::Follow;
          continue;
        }
        Actor& target = level.monsters[static_cast<size_t>(ti)];
        bool in_range = distance_between(minion, target) <= minion.weapon.attack_range;
        if (in_range && line_clear(minion.x, minion.y, target.x, target.y, level.map)) {
          resolve_attack(gs, minion, target, minion.weapon);
          continue;
        }
        auto path = level.map.find_path(minion.x, minion.y, target.x, target.y);
        if (!path.empty()) {
          int move_dx = path[0].first - minion.x;
          int move_dy = path[0].second - minion.y;
          if (!try_monster_step(gs, minion, move_dx, move_dy)) {
            if (!try_monster_step(gs, minion, move_dx, 0)) try_monster_step(gs, minion, 0, move_dy);
          }
        }
        continue;
      }

      if (minion.order == MinionOrder::Hold) {
        if (try_minion_auto_defend(gs, minion)) continue;
        if (minion.x == minion.hold_x && minion.y == minion.hold_y) continue;  // already there
        auto path = level.map.find_path(minion.x, minion.y, minion.hold_x, minion.hold_y);
        if (!path.empty()) {
          int move_dx = path[0].first - minion.x;
          int move_dy = path[0].second - minion.y;
          if (!try_monster_step(gs, minion, move_dx, move_dy)) {
            if (!try_monster_step(gs, minion, move_dx, 0)) try_monster_step(gs, minion, 0, move_dy);
          }
        }
        continue;
      }

      if (minion.order == MinionOrder::Aggressive) {
        if (try_minion_auto_defend(gs, minion)) continue;  // already-adjacent hostiles first

        // Stick with the same hostile while it's still alive and still visible, to
        // avoid flitting between targets when several are in view at once — same idea
        // AttackTarget already has, just auto-selected instead of player-chosen.
        // "Visible" reuses the player's FOV, the same mutual-visibility proxy every
        // other target-selection in the game already uses.
        int ti = actor_index_by_id(level.monsters, minion.attack_target_id);
        bool still_valid = ti >= 0 && level.monsters[static_cast<size_t>(ti)].allegiance == Allegiance::Hostile &&
                            level.map.is_in_fov(level.monsters[static_cast<size_t>(ti)].x,
                                                 level.monsters[static_cast<size_t>(ti)].y);
        if (!still_valid) {
          Actor* closest = nullptr;
          int best_dist = 0;
          for (auto& hostile : level.monsters) {
            if (hostile.allegiance != Allegiance::Hostile || !hostile.is_alive()) continue;
            if (!level.map.is_in_fov(hostile.x, hostile.y)) continue;
            int dist = distance_between(minion, hostile);
            if (closest == nullptr || dist < best_dist) {
              closest = &hostile;
              best_dist = dist;
            }
          }
          minion.attack_target_id = closest != nullptr ? closest->id : -1;
          ti = closest != nullptr ? actor_index_by_id(level.monsters, minion.attack_target_id) : -1;
        }

        if (ti >= 0) {
          Actor& target = level.monsters[static_cast<size_t>(ti)];
          bool in_range = distance_between(minion, target) <= minion.weapon.attack_range;
          if (in_range && line_clear(minion.x, minion.y, target.x, target.y, level.map)) {
            resolve_attack(gs, minion, target, minion.weapon);
            continue;
          }
          auto path = level.map.find_path(minion.x, minion.y, target.x, target.y);
          if (!path.empty()) {
            int move_dx = path[0].first - minion.x;
            int move_dy = path[0].second - minion.y;
            if (!try_monster_step(gs, minion, move_dx, move_dy)) {
              if (!try_monster_step(gs, minion, move_dx, 0)) try_monster_step(gs, minion, 0, move_dy);
            }
          }
          continue;
        }
        // No hostile currently visible — fall through to the Follow movement below.
      }

      if (minion.order == MinionOrder::Follow) {
        if (try_minion_auto_defend(gs, minion)) continue;
      }
      // Follow (also Aggressive with nothing currently visible to chase — its own
      // auto-defend attempt already happened above in that case). Close the distance
      // to the player; try_monster_step already refuses to step onto the player's own
      // tile, so this naturally stops once adjacent rather than trying to stack on them.
      auto path = level.map.find_path(minion.x, minion.y, gs.player.x, gs.player.y);
      if (!path.empty()) {
        int move_dx = path[0].first - minion.x;
        int move_dy = path[0].second - minion.y;
        try_monster_step(gs, minion, move_dx, move_dy);
      }
    }
  }
}

  // Deferred cleanup: the single place anything that died this turn is actually
  // removed, wherever the killing blow came from — a projectile, an AoE blast, the
  // Sandstorm tick, either AI loop, the player's own bump attack, or a minion's
  // duration expiring. Every one of those only zeroes HP (see on_actor_killed()) and
  // leaves the erase to here.
  //
  // It has to work this way because several of those loops iterate this same vector
  // and can each kill something *another* one is still holding a reference or index
  // into during the same turn (e.g. monster #2 kills a minion that monster #5 is also
  // targeting) — erasing in place would shift elements out from under whichever loop
  // hadn't gotten there yet. This is also why monster_at()/hostile_monster_at() filter
  // on is_alive(): between the killing blow and this sweep, a corpse is still sitting
  // in the vector and must not keep blocking its tile.
void sweep_dead(GameState& gs) {
  Level& level = gs.level();
  for (size_t i = 0; i < level.monsters.size();) {
    if (!level.monsters[i].is_alive()) {
      level.monsters.erase(level.monsters.begin() + static_cast<long>(i));
    } else {
      ++i;
    }
  }
}

}  // namespace

void end_turn(GameState& gs) {
  // A free action (Haste, or a boss-style Actor::extra_actions) advances nothing else in
  // the world. end_turn() being the single hook every turn-consuming player action goes
  // through is what makes "act twice per world turn" expressible as a guard here rather
  // than a change at all twenty-odd call sites.
  //
  // Hit-scan projectiles are the one exception: an instant shot has no travel to observe,
  // so making it wait for the world would look broken. A slow projectile (Fireball's orb)
  // is meant to be visibly in flight and only advances on real turns.
  if (gs.free_actions_used < total_actions_for(gs.player) - 1 && !is_game_over(gs)) {
    ++gs.free_actions_used;
    advance_projectiles(gs, /*instant_only=*/true);
    return;
  }
  gs.free_actions_used = 0;

  tick_upkeep(gs, gs.player);
  for (auto& actor : gs.level().monsters) tick_upkeep(gs, actor);

  advance_projectiles(gs);
  tick_minion_durations(gs);
  tick_toggle_spell(gs);

  run_hostile_ai(gs);

  // Load-bearing, and the counterpart of the advance_projectiles() call above that
  // follows the *player's* action. A hit-scan spell has to resolve in the turn it was
  // cast, because a Projectile's path is precomputed at cast time. Without this, a
  // monster's dart waited a turn and then flew to the tile the player had already left —
  // walking in any direction dodged every shot for free. Slow projectiles stay excluded
  // so they're still visibly in flight across turns.
  //
  // Nothing between here and run_hostile_ai() invalidates what that loop touched: a kill
  // only zeroes HP and drops gear, and sweep_dead() does the erasing at the very end. If
  // a minion ever casts, run_minion_ai() needs the mirror of this call.
  advance_projectiles(gs, /*instant_only=*/true);

  run_minion_ai(gs);
  sweep_dead(gs);
}

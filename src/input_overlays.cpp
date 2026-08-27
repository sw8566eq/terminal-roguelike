// Input handling for the modes that overlay the Playing HUD instead of taking over the
// whole screen: Targeting, RangedAttack, MinionFocus, Look. Split out of input.cpp
// purely to keep that file's size down (see input_internal.hpp and CLAUDE.md's "Known
// limitations" note) — reached the same way every other mode is, through
// handle_event()'s one switch. Each function still owns exactly one Mode, same as before
// the split.

#include "input.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "actors.hpp"
#include "content.hpp"
#include "input_internal.hpp"
#include "level.hpp"
#include "projectile.hpp"
#include "render.hpp"
#include "rng.hpp"
#include "rules.hpp"
#include "run_history.hpp"
#include "spells.hpp"
#include "turn.hpp"

void handle_minion_focus_input(GameState& gs, const SDL_Event& event) {
  Level& level = gs.level();
  // Applies `fn` to every currently-commanded minion — all of them if this
  // session came from the roster's "All", otherwise just the one named by
  // focused_minion_id. Shared by F (Follow) and Enter (Attack/Hold) below so
  // the "who does this apply to" logic can't drift between the two.
  auto for_each_commanded_minion = [&](auto&& fn) {
    int count = 0;
    if (gs.commanding_all_minions) {
      for (auto& m : level.monsters) {
        if (m.allegiance == Allegiance::Player && m.is_alive()) {
          fn(m);
          ++count;
        }
      }
    } else {
      int fi = actor_index_by_id(level.monsters, gs.focused_minion_id);
      if (fi >= 0) {
        fn(level.monsters[static_cast<size_t>(fi)]);
        count = 1;
      }
    }
    return count;
  };

  bool shift_held = (event.key.mod & SDL_KMOD_SHIFT) != 0;
  if (event.key.key == SDLK_ESCAPE || (event.key.key == SDLK_P && shift_held)) {
    // Esc just backs out of this one planning action; Shift+P additionally
    // resets cycle position, so the next 'o'/'p' starts over from the top —
    // "focusing back on the player instantly."
    if (event.key.key == SDLK_P) gs.focused_minion_id = -1;
    gs.commanding_all_minions = false;
    gs.mode = Mode::Playing;
    return;
  }
  if (event.key.key == SDLK_O || (event.key.key == SDLK_P && !shift_held)) {
    // Tab straight to the next/previous minion without dropping back to normal
    // play in between — plan one, tab, plan the next, same as 'o'/'p' do from
    // Mode::Playing (see cycle_minion_focus above), just without leaving this mode.
    if (!cycle_minion_focus(gs, event.key.key == SDLK_O ? 1 : -1)) {
      add_message(gs, "You have no minions to command.");
      gs.mode = Mode::Playing;
    }
    return;
  }
  // 'z' opens this minion's own abilities, mirroring 'z' for the player's spells in
  // normal play. Scoped to a single minion: "All" commands a stance, but an ability is
  // aimed and paid for by one specific creature, so it uses focused_minion_id even in an
  // All session rather than trying to fire the whole pack's.
  if (event.key.key == SDLK_Z) {
    auto abilities = focused_minion_abilities(gs);
    if (abilities.empty()) {
      add_message(gs, gs.focused_minion_id < 0 ? "Focus a single minion first."
                                               : "That minion has no abilities.");
      return;
    }
    gs.mode = Mode::MinionAbilityMenu;
    return;
  }

  if (event.key.key == SDLK_F) {
    int ordered = for_each_commanded_minion([](Actor& m) { m.order = MinionOrder::Follow; });
    add_message(gs, ordered == 1 ? "Your minion returns to your side." : "Your minions return to your side.");
    gs.commanding_all_minions = false;
    gs.mode = Mode::Playing;
    return;
  }
  if (event.key.key == SDLK_G) {
    // Aggressive: same as Follow, but engages anything hostile it can see
    // instead of waiting for something to wander into its own reach — see the
    // MinionOrder doc comment in entity.hpp.
    int ordered = for_each_commanded_minion([](Actor& m) { m.order = MinionOrder::Aggressive; });
    add_message(gs, ordered == 1 ? "Your minion goes on the offensive." : "Your minions go on the offensive.");
    gs.commanding_all_minions = false;
    gs.mode = Mode::Playing;
    return;
  }
  if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
    int hostile_hit = hostile_monster_at(level.monsters, gs.target_x, gs.target_y);
    if (hostile_hit >= 0) {
      int target_id = level.monsters[static_cast<size_t>(hostile_hit)].id;
      std::string target_name = level.monsters[static_cast<size_t>(hostile_hit)].name;
      int ordered = for_each_commanded_minion([&](Actor& m) {
        m.order = MinionOrder::AttackTarget;
        m.attack_target_id = target_id;
      });
      add_message(gs, (ordered == 1 ? "Your minion attacks the " : "Your minions attack the ") + target_name +
                  "!");
      gs.commanding_all_minions = false;
      gs.mode = Mode::Playing;
      return;
    }
    if (gs.target_x == gs.player.x && gs.target_y == gs.player.y) {
      // Targeting yourself reads as "come back to me" — the same Follow order
      // 'F' gives directly, just reachable without moving the cursor off your
      // own tile first. (Previously this fell through to the tile_free/Hold
      // check below, which — since the player isn't in level.monsters and thus
      // invisible to monster_at() — would silently issue a Hold planted on the
      // player's exact tile instead of anything resembling a rejection.)
      int ordered = for_each_commanded_minion([](Actor& m) { m.order = MinionOrder::Follow; });
      add_message(gs, ordered == 1 ? "Your minion returns to your side." : "Your minions return to your side.");
      gs.commanding_all_minions = false;
      gs.mode = Mode::Playing;
      return;
    }
    bool tile_free = level.map.is_walkable(gs.target_x, gs.target_y) &&
                      monster_at(level.monsters, gs.target_x, gs.target_y) < 0;
    if (tile_free) {
      int hx = gs.target_x;
      int hy = gs.target_y;
      int ordered = for_each_commanded_minion([&](Actor& m) {
        m.order = MinionOrder::Hold;
        m.hold_x = hx;
        m.hold_y = hy;
      });
      add_message(gs, ordered == 1 ? "Your minion holds position." : "Your minions hold position.");
      gs.commanding_all_minions = false;
      gs.mode = Mode::Playing;
      return;
    }
    add_message(gs, "You can't send them there.");
    return;  // stay in this mode, no turn spent — try again
  }

  // Movement keys move the cursor — unlike a spell's Targeting, there's no
  // range limit here (a minion will path however far it needs to), just the
  // map bounds.
  int tdx = 0;
  int tdy = 0;
  switch (event.key.key) {
    case SDLK_UP:
    case SDLK_K:
      tdy = -1;
      break;
    case SDLK_DOWN:
    case SDLK_J:
      tdy = 1;
      break;
    case SDLK_LEFT:
    case SDLK_H:
      tdx = -1;
      break;
    case SDLK_RIGHT:
    case SDLK_L:
      tdx = 1;
      break;
    case SDLK_Y:
      tdx = -1;
      tdy = -1;
      break;
    case SDLK_U:
      tdx = 1;
      tdy = -1;
      break;
    case SDLK_B:
      tdx = -1;
      tdy = 1;
      break;
    case SDLK_N:
      tdx = 1;
      tdy = 1;
      break;
    default:
      break;
  }
  if (tdx != 0 || tdy != 0) {
    int nx = gs.target_x + tdx;
    int ny = gs.target_y + tdy;
    if (level.map.in_bounds(nx, ny)) {
      gs.target_x = nx;
      gs.target_y = ny;
    }
  }
  return;
}

void handle_targeting_input(GameState& gs, const SDL_Event& event) {
  Level& level = gs.level();
  const Spell& spell = kSpellTable[static_cast<size_t>(gs.casting_spell_index)];

  // Who is actually casting. -1 is the player; anything else is a minion using one of
  // its own abilities, in which case the shot originates at *its* tile, its mana pays,
  // and its Dexterity aims. A minion that died between opening the menu and pressing
  // Enter resolves to -1 here, so the cast is cancelled rather than fired from the
  // player by accident.
  int caster_index = gs.casting_actor_id < 0 ? -1 : actor_index_by_id(level.monsters, gs.casting_actor_id);
  if (gs.casting_actor_id >= 0 && caster_index < 0) {
    add_message(gs, "Your minion is gone.");
    gs.casting_actor_id = -1;
    gs.mode = Mode::Playing;
    return;
  }
  Actor& caster = caster_index < 0 ? gs.player : level.monsters[static_cast<size_t>(caster_index)];

  if (event.key.key == SDLK_ESCAPE) {
    gs.casting_actor_id = -1;
    gs.mode = Mode::Playing;
    return;
  }
  if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
    if (caster.mana < spell.mana_cost) {
      add_message(gs, actor_subject(caster) + (caster.is_player ? " haven't" : " hasn't") +
                          " the mana to cast " + spell.name + ".");
      gs.casting_actor_id = -1;
      gs.mode = Mode::Playing;  // free cancel, same as Esc — no turn spent
      return;
    }

    // Raising resolves here too: it targets a Corpse rather than an Actor, the only
    // spell that does. Same three gates the preview colors by — a body under the cursor,
    // in range, with a clear line.
    if (spell.is_raise) {
      int ci = corpse_at(level, gs.target_x, gs.target_y);
      if (ci < 0 || !line_clear(caster.x, caster.y, gs.target_x, gs.target_y, level.map)) {
        add_message(gs, "There's no corpse there to raise.");
        gs.casting_actor_id = -1;
        gs.mode = Mode::Playing;  // free cancel, same as Esc
        return;
      }
      // Raise Dead has its own pool (Spell::minion_cap), counted across every species it
      // can produce — independent of how many Imps or Demons are already out.
      if (spell.minion_cap >= 0 && count_raised_minions(level.monsters) >= spell.minion_cap) {
        add_message(gs, "You can't raise any more corpses right now.");
        gs.casting_actor_id = -1;
        gs.mode = Mode::Playing;  // free cancel — the corpse is left where it is
        return;
      }
      int nx, ny;
      // Raised on a free tile beside the corpse rather than on it, so it can't land on
      // top of the player or another minion.
      if (!free_adjacent_tile(level.map, level.monsters, gs.target_x, gs.target_y, nx, ny)) {
        add_message(gs, "There's no room beside the corpse.");
        gs.casting_actor_id = -1;
        gs.mode = Mode::Playing;
        return;
      }
      int tmpl_index = level.corpses[static_cast<size_t>(ci)].monster_template_index;
      level.corpses.erase(level.corpses.begin() + ci);  // consumed either way
      Actor raised = spawn_reanimated(tmpl_index, nx, ny);
      // A fresh raise joins whatever the pack is already doing, same rule a fresh summon
      // follows, so a mid-fight raise doesn't stand around.
      raised.order = MinionOrder::Aggressive;
      add_message(gs, "The " + raised.name + " rises to serve you.");
      level.monsters.push_back(raised);
      caster.mana -= spell.mana_cost;
      gs.casting_actor_id = -1;
      gs.mode = Mode::Playing;
      end_turn(gs);
      return;
    }

    // A targeted debuff resolves right here: no projectile, no dodge roll. Needs a live
    // hostile under the cursor, in range and with a clear line — the same three
    // conditions the preview colors the cursor by.
    if (spell.is_debuff) {
      int victim = hostile_monster_at(level.monsters, gs.target_x, gs.target_y);
      if (victim < 0 || !line_clear(caster.x, caster.y, gs.target_x, gs.target_y, level.map)) {
        add_message(gs, "There's nothing there to curse.");
        gs.casting_actor_id = -1;
        gs.mode = Mode::Playing;  // free cancel, same as Esc
        return;
      }
      caster.mana -= spell.mana_cost;
      add_message(gs, actor_subject(caster) + actor_verb(caster, " cast") + " " + spell.name + ".");
      apply_debuff(gs, level.monsters[static_cast<size_t>(victim)], spell);
      gs.casting_actor_id = -1;
      gs.mode = Mode::Playing;
      end_turn(gs);
      return;
    }

    if (spell.is_swap) {
      int minion_index = own_minion_at(level.monsters, gs.target_x, gs.target_y);
      if (minion_index < 0) {
        add_message(gs, "There's no minion there to swap places with.");
        gs.mode = Mode::Playing;  // free cancel, same as Esc — no turn spent
        return;
      }
      // A wall blocks the swap, the same way it blocks every other shot in the game.
      // Without this you could post a minion outside a room and teleport out of any
      // fight through solid rock — a guaranteed, undodgeable escape that no amount of
      // mana cost balances. You still always *know* where your minions are (they stay
      // drawn out of sight); you just can't swap to one you can't see.
      if (!line_clear(gs.player.x, gs.player.y, gs.target_x, gs.target_y, level.map)) {
        add_message(gs, "You can't see your minion from here.");
        gs.mode = Mode::Playing;  // free cancel, same as Esc — no turn spent
        return;
      }
      Actor& minion = level.monsters[static_cast<size_t>(minion_index)];
      std::swap(gs.player.x, minion.x);
      std::swap(gs.player.y, minion.y);
      // Not an incremental step, so (unlike normal movement) FOV needs an
      // explicit recompute — same as the Potion of Teleportation's effect.
      level.map.update_fov(gs.player.x, gs.player.y, FOV_RADIUS);
      gs.player.mana -= spell.mana_cost;
      add_message(gs, "You swap places with your " + minion.name + ".");
      gs.mode = Mode::Playing;
      end_turn(gs);
      return;
    }

    // Remember what's under the cursor now, before firing, so the next time
    // Targeting/RangedAttack opens it re-aims at the same monster (see
    // auto_target_hostile()). Left unchanged if the shot is aimed at empty
    // ground (e.g. an AoE spell dropped on open floor).
    int hit_index = hostile_monster_at(level.monsters, gs.target_x, gs.target_y);
    if (hit_index >= 0) gs.last_target_id = level.monsters[static_cast<size_t>(hit_index)].id;

    // Any tile is a legal target now: the spell travels and resolves against
    // whatever (if anything) it actually reaches, not necessarily the cursor tile.
    Projectile proj;
    proj.path = trace_path(caster.x, caster.y, gs.target_x, gs.target_y);
    proj.speed = spell.speed;
    proj.dice_count = spell.dice_count;
    proj.dice_sides = spell.dice_sides;
    proj.hit_dice_count = spell.hit_dice_count;
    proj.hit_dice_sides = spell.hit_dice_sides;
    proj.aoe_radius = spell.aoe_radius;
    proj.pierces = spell.pierces;
    proj.prev_x = caster.x;  // seeds the "last open tile" for an immediate wall hit
    proj.prev_y = caster.y;
    // Locked in now, not re-read when it lands. Temporary INT (from a Potion of
    // Intelligence) boosts this the same as permanent INT would — only spell
    // *unlocking* (known_spell_indices, above) ignores the temporary bonus.
    proj.bonus = (caster.intelligence + caster.temp_int_bonus) / 3;
    // A spell's accuracy is built the same way a weapon swing's is: the caster's
    // Dexterity term plus the spell's own hit-dice, rolled on impact. Locking the
    // Dexterity half in here (rather than reading it when the projectile lands
    // several turns later) is what makes a slow Fireball as accurate as the moment
    // it was thrown.
    proj.accuracy_bonus = (caster.dexterity + caster.temp_dex_bonus) * kAccuracyPerDexPoint;
    proj.name = spell.name;
    proj.glyph = spell.glyph;
    proj.color = spell.color;
    // Matches Projectile's defaults, but set explicitly: now that a monster can
    // fire one too, "who owns this" shouldn't be something a reader has to infer
    // from a default.
    proj.owner_allegiance = Allegiance::Player;  // the player's side either way
    proj.owner_is_player = caster.is_player;
    proj.owner_name = caster.name;
    level.projectiles.push_back(proj);
    caster.mana -= spell.mana_cost;

    add_message(gs, actor_subject(caster) + actor_verb(caster, " cast") + " " + spell.name + ".");
    gs.casting_actor_id = -1;
    gs.mode = Mode::Playing;
    end_turn(gs);  // advance_projectiles() may resolve this immediately for fast spells
    return;
  }

  // Movement keys move the targeting cursor instead of the player.
  int tdx = 0;
  int tdy = 0;
  switch (event.key.key) {
    case SDLK_UP:
    case SDLK_K:
      tdy = -1;
      break;
    case SDLK_DOWN:
    case SDLK_J:
      tdy = 1;
      break;
    case SDLK_LEFT:
    case SDLK_H:
      tdx = -1;
      break;
    case SDLK_RIGHT:
    case SDLK_L:
      tdx = 1;
      break;
    case SDLK_Y:
      tdx = -1;
      tdy = -1;
      break;
    case SDLK_U:
      tdx = 1;
      tdy = -1;
      break;
    case SDLK_B:
      tdx = -1;
      tdy = 1;
      break;
    case SDLK_N:
      tdx = 1;
      tdy = 1;
      break;
    default:
      break;
  }
  if (tdx != 0 || tdy != 0) {
    int nx = gs.target_x + tdx;
    int ny = gs.target_y + tdy;
    int rdx = nx - caster.x;
    int rdy = ny - caster.y;
    bool in_range = rdx * rdx + rdy * rdy <= spell.range * spell.range;
    if (level.map.in_bounds(nx, ny) && in_range) {
      gs.target_x = nx;
      gs.target_y = ny;
    }
  }
  return;
}

void handle_ranged_input(GameState& gs, const SDL_Event& event) {
  Level& level = gs.level();
  const Weapon& weapon = gs.player.weapon;

  if (event.key.key == SDLK_ESCAPE) {
    gs.mode = Mode::Playing;
    return;
  }
  if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
    // Same shape as a spell cast (Mode::Targeting above), just sourced from the
    // weapon instead of a Spell and with no mana cost. Both bonuses come from the
    // shared helpers, so a fired Bow lands with exactly the accuracy and damage
    // that same Bow would have if a monster were shooting it at you:
    // damage_bonus_for() gives a ranged weapon the Dexterity bonus, and
    // accuracy_bonus carries the caster's Dexterity accuracy term (the weapon's
    // own hit-dice are still rolled fresh on impact).

    // Remember what's under the cursor now, before firing — see the Targeting
    // handler's identical comment above.
    int hit_index = hostile_monster_at(level.monsters, gs.target_x, gs.target_y);
    if (hit_index >= 0) gs.last_target_id = level.monsters[static_cast<size_t>(hit_index)].id;

    Projectile proj;
    proj.path = trace_path(gs.player.x, gs.player.y, gs.target_x, gs.target_y);
    proj.speed = kInstantSpellSpeed;
    proj.dice_count = weapon.dice_count;
    proj.dice_sides = weapon.dice_sides;
    proj.hit_dice_count = weapon.hit_dice_count;
    proj.hit_dice_sides = weapon.hit_dice_sides;
    proj.prev_x = gs.player.x;
    proj.prev_y = gs.player.y;
    proj.bonus = weapon.bonus + damage_bonus_for(gs.player, weapon);
    proj.accuracy_bonus = (gs.player.dexterity + gs.player.temp_dex_bonus) * kAccuracyPerDexPoint;
    proj.name = weapon.name;
    proj.glyph = '-';
    proj.color = tcod::ColorRGB{200, 170, 100};
    proj.owner_allegiance = Allegiance::Player;  // explicit, see the spell cast above
    proj.owner_is_player = true;
    level.projectiles.push_back(proj);

    add_message(gs, "You fire your " + weapon.name + ".");
    gs.mode = Mode::Playing;
    end_turn(gs);
    return;
  }

  // Movement keys move the targeting cursor instead of the player, clamped to
  // the weapon's own range — same circular-radius shape a spell's Targeting uses.
  int tdx = 0;
  int tdy = 0;
  switch (event.key.key) {
    case SDLK_UP:
    case SDLK_K:
      tdy = -1;
      break;
    case SDLK_DOWN:
    case SDLK_J:
      tdy = 1;
      break;
    case SDLK_LEFT:
    case SDLK_H:
      tdx = -1;
      break;
    case SDLK_RIGHT:
    case SDLK_L:
      tdx = 1;
      break;
    case SDLK_Y:
      tdx = -1;
      tdy = -1;
      break;
    case SDLK_U:
      tdx = 1;
      tdy = -1;
      break;
    case SDLK_B:
      tdx = -1;
      tdy = 1;
      break;
    case SDLK_N:
      tdx = 1;
      tdy = 1;
      break;
    default:
      break;
  }
  if (tdx != 0 || tdy != 0) {
    int nx = gs.target_x + tdx;
    int ny = gs.target_y + tdy;
    int rdx = nx - gs.player.x;
    int rdy = ny - gs.player.y;
    bool in_range = rdx * rdx + rdy * rdy <= weapon.attack_range * weapon.attack_range;
    if (level.map.in_bounds(nx, ny) && in_range) {
      gs.target_x = nx;
      gs.target_y = ny;
    }
  }
  return;
}

void handle_look_input(GameState& gs, const SDL_Event& event) {
  Level& level = gs.level();
  if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_X) {
    gs.mode = Mode::Playing;
    return;
  }

  // Movement keys move the cursor — no range limit and no confirm action, just
  // look around and back out with Esc/'x' (no turn spent either way).
  int tdx = 0;
  int tdy = 0;
  switch (event.key.key) {
    case SDLK_UP:
    case SDLK_K:
      tdy = -1;
      break;
    case SDLK_DOWN:
    case SDLK_J:
      tdy = 1;
      break;
    case SDLK_LEFT:
    case SDLK_H:
      tdx = -1;
      break;
    case SDLK_RIGHT:
    case SDLK_L:
      tdx = 1;
      break;
    case SDLK_Y:
      tdx = -1;
      tdy = -1;
      break;
    case SDLK_U:
      tdx = 1;
      tdy = -1;
      break;
    case SDLK_B:
      tdx = -1;
      tdy = 1;
      break;
    case SDLK_N:
      tdx = 1;
      tdy = 1;
      break;
    default:
      break;
  }
  if (tdx != 0 || tdy != 0) {
    int nx = gs.target_x + tdx;
    int ny = gs.target_y + tdy;
    if (level.map.in_bounds(nx, ny)) {
      gs.target_x = nx;
      gs.target_y = ny;
    }
  }
  return;
}

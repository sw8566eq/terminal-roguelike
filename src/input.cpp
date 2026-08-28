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
#include "spells.hpp"
#include "turn.hpp"

// Moves command focus to the next (direction=+1) or previous (direction=-1)
// living minion, in level.monsters order, wrapping around; if focused_minion_id
// doesn't currently name a living minion (nothing focused yet, or it died),
// starts from the first (next) or last (prev) instead of wrapping relative to a
// missing position. Always lands on one specific minion — never "all" — and
// points the cursor at its current position. Returns false (no-op) if there are
// no minions at all. Shared by the 'o'/'p' trigger keys from Mode::Playing and
// by the same keys working *inside* Mode::MinionFocus too, so you can tab
// straight from planning one minion's order to the next without dropping back
// to normal play in between — the "turn planner" feel this whole system is for.
bool cycle_minion_focus(GameState& gs, int direction) {
  Level& level = gs.level();
  std::vector<int> minion_ids;
  for (const auto& m : level.monsters) {
    if (m.allegiance == Allegiance::Player && m.is_alive()) minion_ids.push_back(m.id);
  }
  if (minion_ids.empty()) return false;
  int current = -1;
  for (size_t i = 0; i < minion_ids.size(); ++i) {
    if (minion_ids[i] == gs.focused_minion_id) {
      current = static_cast<int>(i);
      break;
    }
  }
  int next_index;
  if (current < 0) {
    next_index = direction >= 0 ? 0 : static_cast<int>(minion_ids.size()) - 1;
  } else {
    next_index = (current + direction + static_cast<int>(minion_ids.size())) %
                 static_cast<int>(minion_ids.size());
  }
  gs.focused_minion_id = minion_ids[static_cast<size_t>(next_index)];
  gs.commanding_all_minions = false;
  int fi = actor_index_by_id(level.monsters, gs.focused_minion_id);
  gs.target_x = level.monsters[static_cast<size_t>(fi)].x;
  gs.target_y = level.monsters[static_cast<size_t>(fi)].y;
  return true;
}

namespace {

// The side-passage stop condition, evaluated from the tile the player is standing on and
// looking one step ahead: true when a wall running alongside the direction of travel is
// about to open up.
//
// Each side (the two tiles perpendicular to travel) is tested on its own, and fires when
// that side is a wall *here* but open at the corresponding front diagonal — the wall
// you've been following ends one step ahead. Reporting it from the tile before the
// junction rather than on it is the point: the run stops with the opening in front of the
// player instead of carrying them into the middle of it.
//
// Testing the two sides independently, rather than "a wall on either side AND an opening
// on either side", is what keeps a corridor that merely happens to be more than one tile
// wide from reading as a fork on every tile along it: its open side has no wall to end,
// and its walled side stays walled, so neither clause ever completes. Only a real opening
// flips one side from wall to floor.
//
// Deliberately *not* gated on is_in_room() — the rule is self-limiting, and gating it was
// a bug: open room floor has no wall to either side so nothing fires there anyway, while
// running along a room's edge past an exit cut through that wall does fire, which is
// exactly the branch a player crossing a room cares about.
//
// Diagonal travel is skipped: corridors here are always straight horizontal/vertical
// segments (Map::dig_corridor_h/v), so the front-diagonal geometry has no corridor case
// left to describe.
bool side_opening_ahead(const Map& map, int x, int y, int dx, int dy) {
  if (dx != 0 && dy != 0) return false;
  const int px = -dy, py = dx;  // one perpendicular unit vector; the other is its negation
  auto wall_here_open_ahead = [&](int ox, int oy) {
    return !map.is_walkable(x + ox, y + oy) && map.is_walkable(x + dx + ox, y + dy + oy);
  };
  return wall_here_open_ahead(px, py) || wall_here_open_ahead(-px, -py);
}

// Side-effect-free approximation of "would a hostile be visible from (x, y)" — used to
// peek at the *prospective* tile before the player actually steps onto it (see below).
// Deliberately not Map::update_fov(): that recomputes the map's one shared FOV state,
// which also permanently marks every tile it lights as explored (Map::update_fov()) —
// fine when the player is really moving there, wrong for a tile they might not step
// onto at all, since it would leak map knowledge for a step never taken. A
// distance+line_clear() check instead — the same raycast-based line-of-sight primitive
// already used for ranged attacks/casts — has no such state to leak.
bool would_see_hostile_from(const std::vector<Actor>& monsters, const Map& map, int x, int y) {
  for (const auto& m : monsters) {
    if (m.allegiance != Allegiance::Hostile || !m.is_alive()) continue;
    int mdx = m.x - x, mdy = m.y - y;
    if (mdx * mdx + mdy * mdy > FOV_RADIUS * FOV_RADIUS) continue;  // same circular clamp is_in_fov() uses
    if (line_clear(x, y, m.x, m.y, map)) return true;
  }
  return false;
}

// Shift+direction: quicker navigation than tapping the same key over and over. Repeats
// the plain movement step in one direction, a full real turn at a time — monster AI runs
// on every step, exactly as it would if the key were pressed that many times by hand.
//
// There are two kinds of stop, and keeping them apart is what stops travel deadlocking:
//
//   *Hard* — the next tile is a wall, or something is standing on it (a monster or the
//   player's own minion). Checked on every step, the first included. Travel is a "get me
//   somewhere else" gesture, not a way to bump-attack or minion-swap on autopilot, so it
//   stops short rather than resolving either; plain unshifted movement still does both,
//   so nothing is unreachable.
//
//   *Soft* — a room threshold, a side opening (side_opening_ahead()), or a hostile
//   coming into view. These end a run already in progress, but are skipped on its first
//   step, so a run started from a tile satisfying one still moves. That exemption is
//   load-bearing rather than a courtesy: each of these is a property of the tile the
//   player is *standing on*, so without it the very tile a run stopped on would refuse
//   to start the next run from that spot — leaving the player unable to travel out of
//   it at all, permanently. (That was a real bug, not a hypothetical: every soft stop
//   became a wall, and a single visible monster froze travel outright.) Pressing the key
//   again from a tile travel deliberately stopped on is the player saying they've seen
//   what it stopped for.
//
// Soft stops all land the player one tile early by construction, able to see whatever
// stopped them instead of standing in it. The one exception is dying mid-run: a hostile
// still out of view can land a hit on the very step that reveals it, since the end_turn()
// that reveals it is the same one that let it act.
//
// kRunMaxSteps is a defensive cap, not a real limit — a straight line in one direction
// always meets a wall well before it, on any map this size.
constexpr int kRunMaxSteps = 200;

void run_in_direction(GameState& gs, int dx, int dy) {
  for (int step = 0; step < kRunMaxSteps; ++step) {
    Level& level = gs.level();  // re-fetched every step; end_turn() never reallocates
                                 // levels itself, but staying consistent with every
                                 // other call site that re-fetches after a turn passes
                                 // costs nothing and can't go stale.
    const int new_x = gs.player.x + dx;
    const int new_y = gs.player.y + dy;

    if (monster_at(level.monsters, new_x, new_y) >= 0) return;
    if (!level.map.is_walkable(new_x, new_y)) return;

    if (step > 0) {
      bool entering_room = !level.map.is_in_room(gs.player.x, gs.player.y) && level.map.is_in_room(new_x, new_y);
      if (entering_room) return;
      if (side_opening_ahead(level.map, gs.player.x, gs.player.y, dx, dy)) return;
      if (would_see_hostile_from(level.monsters, level.map, new_x, new_y)) return;
    }

    gs.player.x = new_x;
    gs.player.y = new_y;
    level.map.update_fov(gs.player.x, gs.player.y, FOV_RADIUS);
    end_turn(gs);
    if (gs.mode != Mode::Playing) return;  // e.g. died mid-run
  }
}

void handle_playing_input(GameState& gs, const SDL_Event& event) {
  Level& level = gs.level();
  if (event.key.key == SDLK_ESCAPE) {
    gs.running = false;
    return;
  }

  if (event.key.key == SDLK_W) {
    gs.mode = Mode::WeaponMenu;
    return;
  }
  if (event.key.key == SDLK_A) {
    gs.mode = Mode::ArmorMenu;
    return;
  }
  if (event.key.key == SDLK_D) {
    gs.mode = Mode::Drop;
    return;
  }
  if (event.key.key == SDLK_Q) {
    gs.mode = Mode::PotionMenu;
    return;
  }
  if (event.key.key == SDLK_G) {
    // No auto-pickup on step, so this is the only way loot leaves the floor.
    //
    // One item is taken immediately — a menu to confirm a single obvious choice is
    // friction. Two or more opens Mode::Pickup, since a dead monster drops its whole
    // pack on one tile (an Orc Archer leaves both a Short Bow and a Short Sword) and
    // taking all of it is often not what you want.
    auto slots = ground_slots_at(level, gs.player.x, gs.player.y);
    if (slots.empty()) {
      add_message(gs, "There's nothing here to pick up.");
      return;
    }
    if (slots.size() == 1) {
      pick_up_ground_items(gs, slots);
      end_turn(gs);
      return;
    }
    // Everything starts checked, so g-then-Enter reproduces the old "take it all"
    // behavior in two keystrokes and deselecting is the deliberate act.
    gs.pickup_selected.assign(slots.size(), true);
    gs.mode = Mode::Pickup;
    return;
  }
  if (event.key.key == SDLK_Z) {
    gs.mode = Mode::SpellMenu;
    return;
  }
  if (event.key.key == SDLK_F) {
    // Fire the equipped weapon at range — only meaningful for a ranged weapon
    // (Weapon::attack_range > 1, e.g. Bow); a melee weapon still only attacks by
    // bumping into an adjacent monster.
    if (gs.player.weapon.attack_range <= 1) {
      add_message(gs, "Your " + gs.player.weapon.name + " isn't a ranged weapon.");
    } else {
      // Same auto-aim as SpellMenu -> Targeting above, see auto_target_hostile().
      int auto_id = auto_target_hostile(level.monsters, gs.player, level.map, gs.last_target_id,
                                         gs.player.weapon.attack_range);
      int auto_idx = actor_index_by_id(level.monsters, auto_id);
      if (auto_idx >= 0) {
        gs.target_x = level.monsters[static_cast<size_t>(auto_idx)].x;
        gs.target_y = level.monsters[static_cast<size_t>(auto_idx)].y;
      } else {
        gs.target_x = gs.player.x;
        gs.target_y = gs.player.y;
      }
      gs.mode = Mode::RangedAttack;
    }
    return;
  }
  if (event.key.key == SDLK_M) {
    if (count_minions(level.monsters) == 0) {
      add_message(gs, "You have no minions to command.");
    } else {
      gs.mode = Mode::MinionRoster;
    }
    return;
  }
  // 'o'/'p' cycle command focus straight to the next/previous minion (skipping
  // the roster menu — a faster path for the same thing), landing in
  // Mode::MinionFocus with the cursor on that minion. Shift+P resets focus
  // without opening anything — see Mode::MinionFocus's own handling of these
  // same keys for tabbing between minions without leaving that mode in between.
  if (event.key.key == SDLK_O || event.key.key == SDLK_P) {
    bool shift_held = (event.key.mod & SDL_KMOD_SHIFT) != 0;
    if (event.key.key == SDLK_P && shift_held) {
      gs.focused_minion_id = -1;
      return;
    }
    if (!cycle_minion_focus(gs, event.key.key == SDLK_O ? 1 : -1)) {
      add_message(gs, "You have no minions to command.");
    } else {
      gs.mode = Mode::MinionFocus;
    }
    return;
  }
  if (event.key.key == SDLK_RIGHTBRACKET) {
    gs.mode = Mode::MessageLog;
    gs.log_scroll = 0;  // always open showing the most recent messages
    return;
  }
  if (event.key.key == SDLK_X) {
    // Starts the look cursor on the player's own tile, same as Targeting/
    // MinionFocus do — free to open/close, no turn spent either way.
    gs.mode = Mode::Look;
    gs.target_x = gs.player.x;
    gs.target_y = gs.player.y;
    return;
  }
  // '?' is Shift+/ on a US layout, so check both the dedicated keycode and the
  // unshifted one with the modifier set — same pattern the stairs keys use below.
  if (event.key.key == SDLK_QUESTION ||
      (event.key.key == SDLK_SLASH && (event.key.mod & SDL_KMOD_SHIFT))) {
    gs.mode = Mode::Help;
    return;
  }
  // SDL reports keycodes for the *unshifted* key on a US layout, so Shift+Period
  // arrives as SDLK_PERIOD with the shift modifier set, not SDLK_GREATER — check
  // both forms so '>' / '<' work regardless of how the layout reports it.
  bool pressed_stairs_down =
      event.key.key == SDLK_GREATER || (event.key.key == SDLK_PERIOD && (event.key.mod & SDL_KMOD_SHIFT));
  bool pressed_stairs_up =
      event.key.key == SDLK_LESS || (event.key.key == SDLK_COMMA && (event.key.mod & SDL_KMOD_SHIFT));

  // Taking stairs costs a turn like any other action. end_turn(gs) runs *before* the
  // transition, so the floor you're leaving gets one parting action — anything
  // adjacent to the stairs gets a swing in as you go, rather than the stairs being a
  // free escape from a losing fight. That also means you can die on the way out,
  // hence the mode check before actually moving floors.
  //
  // Deliberately called here rather than inside descend()/ascend(): the --floor=N
  // debug flag replays descend() in a loop at startup, and running a full turn of
  // monster AI on every intermediate floor before the game even opens would be
  // wrong.
  if (pressed_stairs_down) {
    if (level.has_stairs_down && gs.player.x == level.stairs_down_x && gs.player.y == level.stairs_down_y) {
      end_turn(gs);
      if (!is_game_over(gs)) descend(gs);
    } else if (!level.has_stairs_down) {
      add_message(gs, "This is the deepest floor of the dungeon.");
    } else {
      add_message(gs, "There are no stairs down here.");
    }
    return;
  }
  if (pressed_stairs_up) {
    if (level.has_stairs_up && gs.player.x == level.entry_x && gs.player.y == level.entry_y) {
      end_turn(gs);
      if (!is_game_over(gs)) ascend(gs);
    } else {
      add_message(gs, "There are no stairs up here.");
    }
    return;
  }
  // Plain '.' (no shift, which is claimed above for '>') passes the turn without
  // moving or attacking — handy for watching what monsters do on their own.
  if (event.key.key == SDLK_PERIOD && !(event.key.mod & SDL_KMOD_SHIFT)) {
    add_message(gs, "You wait.");
    end_turn(gs);
    return;
  }

  int dx = 0;
  int dy = 0;
  switch (event.key.key) {
    case SDLK_UP:
    case SDLK_K:
      dy = -1;
      break;
    case SDLK_DOWN:
    case SDLK_J:
      dy = 1;
      break;
    case SDLK_LEFT:
    case SDLK_H:
      dx = -1;
      break;
    case SDLK_RIGHT:
    case SDLK_L:
      dx = 1;
      break;
    // Vim-style diagonals: y/u/b/n for up-left/up-right/down-left/down-right.
    case SDLK_Y:
      dx = -1;
      dy = -1;
      break;
    case SDLK_U:
      dx = 1;
      dy = -1;
      break;
    case SDLK_B:
      dx = -1;
      dy = 1;
      break;
    case SDLK_N:
      dx = 1;
      dy = 1;
      break;
    default:
      break;
  }
  if (dx == 0 && dy == 0) return;

  if (event.key.mod & SDL_KMOD_SHIFT) {
    run_in_direction(gs, dx, dy);
    return;
  }

  int new_x = gs.player.x + dx;
  int new_y = gs.player.y + dy;

  // monster_at() rather than a hand-rolled scan, because it filters on is_alive(). A
  // corpse isn't erased until sweep_dead() at the end of end_turn(), and end_turn()
  // returns *before* that on a free action — so a hasted player who kills something with
  // their first action still has the body in level.monsters for their second. Walking
  // into it used to resolve a fresh attack against the dead Actor, which re-ran
  // on_actor_killed() and granted its XP and dropped its gear a second time.
  int target_index = monster_at(level.monsters, new_x, new_y);

  if (target_index >= 0 && level.monsters[static_cast<size_t>(target_index)].allegiance == Allegiance::Player) {
    // Bump into your own minion: swap places instead of attacking it — you're
    // squeezing past an ally, not fighting one.
    Actor& minion = level.monsters[static_cast<size_t>(target_index)];
    std::swap(gs.player.x, minion.x);
    std::swap(gs.player.y, minion.y);
    level.map.update_fov(gs.player.x, gs.player.y, FOV_RADIUS);
    end_turn(gs);
  } else if (target_index >= 0) {
    // Bump attack: walking into a monster attacks it instead of moving. Exactly the
    // same call a monster makes when it swings at you — the dodge roll, the armor
    // reduction, the XP and the loot drop all live in resolve_attack(), not here.
    resolve_attack(gs, gs.player, level.monsters[static_cast<size_t>(target_index)], gs.player.weapon);
    end_turn(gs);  // any monster(s) still adjacent (including the one just hit) get to act
  } else if (level.map.is_walkable(new_x, new_y)) {
    gs.player.x = new_x;
    gs.player.y = new_y;
    level.map.update_fov(gs.player.x, gs.player.y, FOV_RADIUS);
    end_turn(gs);
  }
}

}  // namespace

void handle_event(GameState& gs, const SDL_Event& event) {
  // Each mode is exclusive: whichever screen is up consumes the key completely, and a
  // key it doesn't handle is swallowed rather than falling through to normal play.
  switch (gs.mode) {
    case Mode::StartMenu:    handle_start_menu_input(gs, event);    return;
    case Mode::SetSeed:      handle_set_seed_input(gs, event);      return;
    case Mode::RunHistory:   handle_run_history_input(gs, event);   return;
    case Mode::Dead:         handle_dead_input(gs, event);          return;
    case Mode::Win:          handle_win_input(gs, event);           return;
    case Mode::MessageLog:   handle_message_log_input(gs, event);   return;
    case Mode::Help:         handle_help_input(gs, event);          return;
    case Mode::LevelUp:      handle_level_up_input(gs, event);      return;
    case Mode::SchoolChoice: handle_school_choice_input(gs, event); return;
    case Mode::WeaponMenu:   handle_weapon_menu_input(gs, event);   return;
    case Mode::ArmorMenu:    handle_armor_menu_input(gs, event);    return;
    case Mode::PotionMenu:   handle_potion_menu_input(gs, event);   return;
    case Mode::SpellMenu:    handle_spell_menu_input(gs, event);    return;
    case Mode::MinionRoster: handle_minion_roster_input(gs, event); return;
    case Mode::Pickup:       handle_pickup_input(gs, event);       return;
    case Mode::MinionFocus:  handle_minion_focus_input(gs, event);  return;
    case Mode::MinionAbilityMenu: handle_minion_ability_menu_input(gs, event); return;
    case Mode::Targeting:    handle_targeting_input(gs, event);     return;
    case Mode::RangedAttack: handle_ranged_input(gs, event);        return;
    case Mode::Look:         handle_look_input(gs, event);          return;
    case Mode::Drop:         handle_drop_input(gs, event);          return;
    case Mode::Playing:      handle_playing_input(gs, event);       return;
  }
}

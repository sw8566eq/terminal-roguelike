#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <libtcod.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "actors.hpp"
#include "content.hpp"
#include "game.hpp"
#include "entity.hpp"
#include "level.hpp"
#include "map.hpp"
#include "projectile.hpp"
#include "rng.hpp"
#include "rules.hpp"
#include "spells.hpp"

// Darkens a color for the "remembered, but not currently visible" rendering tier.
tcod::ColorRGB dim_color(tcod::ColorRGB c) {
  return tcod::ColorRGB{static_cast<uint8_t>(c.r / 3), static_cast<uint8_t>(c.g / 3), static_cast<uint8_t>(c.b / 3)};
}

// Common monospace font paths, one per Linux distro this project's README documents
// setup for. Tried in order; the first one found is used. This approximates "use the
// font your terminal uses" without a fontconfig dependency or bundling a font file:
// on an unconfigured terminal (no custom font override), these paths ARE what
// fontconfig's "monospace" alias resolves to on each respective distro.
const std::vector<std::string> kPreferredFontPaths = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",         // Debian/Ubuntu
    "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",  // Fedora
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",                     // Arch
};

// Loads the first font from kPreferredFontPaths that exists on disk, rendered at
// tile_size x tile_size pixels per cell. Falls back to libtcod's built-in font (same
// one used before this project had any font-selection logic) if none of them exist.
tcod::TilesetPtr load_best_tileset(int tile_size) {
  for (const auto& path : kPreferredFontPaths) {
    if (!std::filesystem::exists(path)) continue;
    tcod::TilesetPtr tileset{TCOD_load_truetype_font_(path.c_str(), tile_size, tile_size)};
    if (tileset) return tileset;
  }
  return tcod::tileset::new_fallback_tileset({tile_size, tile_size});
}

// Hand-drawn ASCII box: corners '+', horizontal '-', vertical '|', with an optional
// title embedded in the top border (" Title "). libtcod's own tcod::print_frame() is
// deprecated upstream in favor of exactly this ("print your own banners for frames"),
// so this is the recommended shape, not a workaround. (x, y) is the box's top-left
// corner in console cells; the box is w x h cells including the border, so a panel's
// drawable interior is (x+1, y+1) through (x+w-2, y+h-2).
void draw_panel(tcod::Console& console, int x, int y, int w, int h, const std::string& title,
                 tcod::ColorRGB color = tcod::ColorRGB{120, 120, 120}) {
  for (int i = x + 1; i < x + w - 1; ++i) {
    console.at(i, y).ch = '-';
    console.at(i, y).fg = color;
    console.at(i, y + h - 1).ch = '-';
    console.at(i, y + h - 1).fg = color;
  }
  for (int j = y + 1; j < y + h - 1; ++j) {
    console.at(x, j).ch = '|';
    console.at(x, j).fg = color;
    console.at(x + w - 1, j).ch = '|';
    console.at(x + w - 1, j).fg = color;
  }
  console.at(x, y).ch = '+';
  console.at(x, y).fg = color;
  console.at(x + w - 1, y).ch = '+';
  console.at(x + w - 1, y).fg = color;
  console.at(x, y + h - 1).ch = '+';
  console.at(x, y + h - 1).fg = color;
  console.at(x + w - 1, y + h - 1).ch = '+';
  console.at(x + w - 1, y + h - 1).fg = color;
  if (!title.empty()) {
    tcod::print(console, {x + 2, y}, " " + title + " ", tcod::ColorRGB{255, 255, 255}, std::nullopt);
  }
}

int main(int argc, char* argv[]) {
  // Pre-scan for the debug flags that have to be known before any level is generated
  // — start_new_game() below builds floor 1, and --floor=N replays descend(), both well
  // before the ordinary argv loop further down. See g_debug_fast_monsters, and seed_rng()
  // in rng.hpp: seeding after floor 1 already exists would defeat the whole point.
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--fast-monsters") g_debug_fast_monsters = true;
    const std::string seed_prefix = "--seed=";
    if (arg.rfind(seed_prefix, 0) == 0) {
      seed_rng(static_cast<unsigned int>(std::stoul(arg.substr(seed_prefix.size()))));
    }
  }

  // Sectioned HUD layout: a context-prompt row, then a map panel (left) and a
  // stats/enemies/minions "at a glance" sidebar (right) side by side, then a
  // message-log panel spanning the full width along the bottom. Each panel is a
  // hand-drawn ASCII box (see draw_panel()) — libtcod's own tcod::print_frame() is
  // deprecated upstream ("print your own banners for frames"), so this matches the
  // rest of the file's manual console.at()-based drawing rather than pulling in a
  // discouraged helper. MAP_ORIGIN_X/Y is where the *camera's* top-left corner lands
  // on screen; every map-drawing call site offsets by these minus the camera position
  // (see camera_x/camera_y below) instead of the old HUD_HEIGHT.
  //
  // The dungeon itself (MAP_WIDTH x MAP_HEIGHT, what generate_level() builds) is
  // bigger than what's ever shown at once — MAP_VIEW_W x MAP_VIEW_H is the map panel's
  // actual on-screen viewport, a scrolling window that follows the player (or the
  // aim cursor while targeting/commanding — see camera_x/camera_y) so the whole window
  // fits on a normal screen instead of rendering the entire floor at 1:1.
  constexpr int MAP_VIEW_W = 50;
  constexpr int MAP_VIEW_H = 24;
  constexpr int MESSAGE_ROWS = 5;  // message-log panel's visible rows, oldest on top

  constexpr int CONTEXT_ROW = 0;  // transient LevelUp/Targeting/MinionFocus prompt

  constexpr int MAP_PANEL_X = 0;
  constexpr int MAP_PANEL_Y = CONTEXT_ROW + 1;
  constexpr int MAP_PANEL_W = MAP_VIEW_W + 2;   // +2 for left/right border
  constexpr int MAP_PANEL_H = MAP_VIEW_H + 2;   // +2 for top/bottom border
  constexpr int MAP_ORIGIN_X = MAP_PANEL_X + 1;
  constexpr int MAP_ORIGIN_Y = MAP_PANEL_Y + 1;

  constexpr int SIDEBAR_X = MAP_PANEL_X + MAP_PANEL_W;
  constexpr int SIDEBAR_Y = MAP_PANEL_Y;
  constexpr int SIDEBAR_W = 28;
  constexpr int SIDEBAR_H = MAP_PANEL_H;

  constexpr int LOG_PANEL_X = MAP_PANEL_X;
  constexpr int LOG_PANEL_Y = MAP_PANEL_Y + MAP_PANEL_H;
  constexpr int LOG_PANEL_W = MAP_PANEL_W + SIDEBAR_W;
  constexpr int LOG_PANEL_H = MESSAGE_ROWS + 2;  // +2 for top/bottom border

  constexpr int SCREEN_WIDTH = LOG_PANEL_W;
  constexpr int SCREEN_HEIGHT = LOG_PANEL_Y + LOG_PANEL_H;

  constexpr int TILE_SIZE = 18;  // pixels per cell; square, so tiles aren't stretched

  auto console = tcod::Console{SCREEN_WIDTH, SCREEN_HEIGHT};  // Main console.

  // Explicitly pick a font instead of leaving tileset null: with none set, libtcod
  // tries to load a "terminal.png" from disk (which this project doesn't ship, hence
  // the "Error loading font image" warning at startup), then silently falls back to
  // its built-in font anyway, but at whatever tiny default size it picks. Doing it
  // ourselves skips the failed disk lookup, lets us pick a real size, and tries to
  // match the font your terminal would normally show text in.
  auto tileset = load_best_tileset(TILE_SIZE);

  // Configure the context.
  auto params = TCOD_ContextParams{};
  params.console = console.get();  // Derive the window size from the console size.
  params.tileset = tileset.get();
  params.window_title = "Terminal Roguelike";
  params.sdl_window_flags = SDL_WINDOW_RESIZABLE;
  params.vsync = true;
  params.argc = argc;
  params.argv = argv;

  auto context = tcod::Context(params);

  // The entire mutable state of the run. Everything below operates on this rather than
  // on a pile of locals, which is what lets the turn loop, the renderer and the input
  // handlers live in their own translation units (see game.hpp).
  GameState gs;
  gs.player.glyph = '@';
  gs.player.color = tcod::ColorRGB{255, 255, 0};
  gs.player.name = "Player";
  gs.player.is_player = true;
  gs.player.id = allocate_actor_id();

  // Runs after the player's turn: every living monster still adjacent to the player
  // gets to attack. (Movement/chasing AI will plug into this same turn boundary later.)
  auto end_turn = [&]() {
    Level& level = gs.level();

    // Extra actions (Haste, or a boss-style Actor::extra_actions): this is where the
    // player spends theirs. end_turn() is the single hook every turn-consuming player
    // action funnels through, so "act twice per world turn" is exactly "return without
    // advancing the world, every other action" — no per-action-site changes anywhere.
    //
    // A monster spends its own extra actions differently, by running its AI-loop body
    // more than once (see the two loops below); the plumbing differs because the
    // player's turn is input-driven and a monster's is a loop iteration, but
    // total_actions_for() is the one shared source of how many either of them gets.
    //
    // Only hit-scan projectiles resolve on a free action — see advance_projectiles()'s
    // instant_only parameter for why. Nothing else runs: no upkeep (so buff timers count
    // *world* turns, meaning Haste's 8 turns is 16 actions), no aura tick, no AI.
    if (gs.free_actions_used < total_actions_for(gs.player) - 1 && gs.mode != Mode::Dead) {
      ++gs.free_actions_used;
      advance_projectiles(gs, /*instant_only=*/true);
      return;
    }
    gs.free_actions_used = 0;

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
    auto tick_upkeep = [&](Actor& actor) {
      if (!actor.is_alive()) return;

      if (actor.hp_regen_turns > 0 && actor.hp < actor.max_hp) {
        actor.hp_regen_accumulator += static_cast<float>(actor.max_hp) / static_cast<float>(actor.hp_regen_turns);
        while (actor.hp_regen_accumulator >= 1.0f && actor.hp < actor.max_hp) {
          actor.hp_regen_accumulator -= 1.0f;
          actor.hp += 1;
        }
      }
      if (actor.mana_regen_turns > 0 && actor.mana < actor.max_mana) {
        actor.mana_regen_accumulator += static_cast<float>(actor.max_mana) / static_cast<float>(kManaRegenTurns);
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
    };

    tick_upkeep(gs.player);
    for (auto& actor : level.monsters) tick_upkeep(actor);

    advance_projectiles(gs);

    // Minion duration: a timed minion (duration_turns > 0, see MinionTemplate) expires
    // on its own once it hits 0, same "tick down, then resolve" shape as the temp stat
    // buffs above. A permanent minion (duration_turns <= 0, the default) never enters
    // this countdown at all. Marked via hp = 0 rather than erased here — same deferred-
    // sweep reasoning as the AI loops below, and it also means an expiring minion
    // doesn't get to act this same turn (is_alive() already gates both AI loops).
    for (auto& m : level.monsters) {
      if (m.allegiance != Allegiance::Player || !m.is_alive() || m.duration_turns <= 0) continue;
      m.duration_turns -= 1;
      if (m.duration_turns == 0) {
        add_message(gs, "Your " + m.name + " collapses into dust.");
        m.hp = 0;
        drop_actor_gear(level, m);  // anything it was carrying outlives it, same as a kill
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
    if (gs.active_toggle_spell >= 0 && gs.mode != Mode::Dead) {
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

    // Tries to step a monster by (step_dx, step_dy); does nothing and returns false if
    // that tile is a wall, already has another living monster on it, or is the
    // player's own tile (the player isn't in level.monsters, so that needs its own
    // check — no monster/minion ever displaces the player by walking into them; the
    // player initiates all bump-to-attack contact, never the other way around).
    auto try_monster_step = [&](Actor& m, int step_dx, int step_dy) -> bool {
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
    };

    // Chebyshev distance, the "how far apart are these two" measure used everywhere
    // reach is decided (attack range, aura radius, weapon selection).
    auto distance_between = [](const Actor& a, const Actor& b) {
      return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
    };

    for (auto& monster : level.monsters) {
      if (gs.mode == Mode::Dead) break;  // player already died to an earlier monster this turn
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
        // monster's first swing, and nothing should get a second one after that.
        if (gs.mode == Mode::Dead || !monster.is_alive()) break;

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

        // Gear and consumables, decided before anything else this turn and using the same
        // code the player's own menus drive. Swapping to whichever carried weapon suits
        // the current distance is free (it's a draw, not a turn); actually drinking
        // something costs the turn, exactly as it does for the player.
        equip_best_weapon_for_range(monster, best_dist);
        if (try_actor_use_potion(gs, monster, /*enemy_near=*/best_dist <= kAiBuffPotionRange)) continue;

        // Spellcasting, for a monster whose row gave it a spell (Actor::spell_index — the
        // Goblin Shaman today). Deliberately built from the same pieces the player's own
        // cast is: it pays kSpellTable's mana_cost out of a real pool, and it pushes a
        // real Projectile onto level.projectiles that travels, is drawn on the map, and
        // rolls dodge/damage in advance_projectiles() like any other shot. Nothing here
        // resolves damage — that's the whole difference from a Goblin Slinger's Rock,
        // which lands instantly inside this loop with nothing to see or react to.
        //
        // Preferred over melee whenever it's affordable and the shot is available, so a
        // Shaman opens at range and only reverts to swinging Claws once the pool is dry.
        // A hit-scan spell resolves in this same turn, via the advance_projectiles() call
        // just after this loop — see the comment there. It never appears on the map, for
        // the same reason the player's own Magic Dart doesn't: instant means instant.
        if (monster.spell_index >= 0) {
          const Spell& spell = kSpellTable[static_cast<size_t>(monster.spell_index)];
          // The is_in_fov() term keeps a caster from sniping out of the dark. Range is
          // Chebyshev here (like every other AI reach check) while FOV_RADIUS is radial,
          // so a diagonal Shaman at range 8 would otherwise sit outside the player's
          // sight while shooting into it — which would undercut the whole point of
          // making monster fire visible. The existing Goblin Slinger has this property
          // for free at range 5 (its furthest diagonal is still inside FOV_RADIUS); this
          // just makes the longer-ranged caster match it rather than get an exception.
          // Reuses the player's own FOV as the mutual-visibility proxy, the same stand-in
          // for per-monster sight used everywhere else in this loop.
          bool can_cast = monster.mana >= spell.mana_cost && best_dist > 0 && best_dist <= spell.range &&
                          level.map.is_in_fov(monster.x, monster.y) &&
                          line_clear(monster.x, monster.y, target->x, target->y, level.map);
          if (can_cast) {
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
        }

        // "In range" is just the equipped weapon's reach — the same field that decides
        // whether the player can fire what they're holding. A wall between the two still
        // blocks it (line_clear()), which at range 1 is always trivially true, so melee is
        // unaffected by that check. Once melee_engaged (see Actor), a ranged monster's
        // reach permanently collapses to 1: it snipes right up until its target reaches
        // it, then fights like any other melee monster for good.
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
        // currently visible — for the player specifically, "visible" still means the
        // FOV-reciprocity check below (can_see_player), same as always; a minion target
        // was already required to be in_fov to be picked as the target at all, above.
        // Otherwise, if the monster still remembers where it last saw the player
        // specifically, head there instead of immediately giving up — once it arrives
        // and the player isn't there, the memory clears and it falls back to idle
        // wandering. Movement follows a real A* path (Map::find_path(), libtcod's
        // TCODPath) recomputed fresh every turn — cheap enough at this map size that
        // there's no need to cache it turn-to-turn — so a monster routes around a wall
        // segment instead of pacing against it.
        bool can_see_player = level.map.is_in_fov(monster.x, monster.y);
        if (can_see_player) {
          monster.last_seen_player_x = gs.player.x;
          monster.last_seen_player_y = gs.player.y;
        }

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
        if (!try_monster_step(monster, move_dx, move_dy)) {
          if (!try_monster_step(monster, move_dx, 0)) try_monster_step(monster, 0, move_dy);
        }
      }
    }

    // Resolve anything hit-scan the monsters just cast, inside the same turn they cast it.
    // This is the exact counterpart of end_turn()'s own advance_projectiles() call, which
    // runs right after the *player's* action for the same reason: an instant spell
    // (kInstantSpellSpeed) is supposed to cross the map the moment it's fired, so it has
    // to resolve while its target is still where it was aimed.
    //
    // Without this a monster's dart sat until the following turn and then flew to the
    // tile the player had already left — its path is precomputed at cast time — so simply
    // walking dodged every shot for free. Slow projectiles are deliberately excluded
    // (instant_only): those are meant to be visibly in flight across turns, and the
    // player's Fireball already behaves that way.
    //
    // Nothing below it in end_turn() invalidates the references this loop held: a kill
    // only zeroes HP and drops gear (the deferred sweep does the erasing), so
    // level.monsters can't reshuffle here. If a minion ever casts, the minion loop below
    // needs the mirror of this call.
    advance_projectiles(gs, /*instant_only=*/true);

    // Distance to the nearest living hostile, or -1 if there are none left on the floor.
    // Used to decide whether a minion should draw a melee weapon or pop a buff potion,
    // the same two questions the hostile loop above asks about its own target.
    auto nearest_hostile_distance = [&](const Actor& minion) {
      int best = -1;
      for (const auto& hostile : level.monsters) {
        if (hostile.allegiance != Allegiance::Hostile || !hostile.is_alive()) continue;
        int dist = distance_between(minion, hostile);
        if (best < 0 || dist < best) best = dist;
      }
      return best;
    };

    // Attacks the closest hostile within reach of the minion's equipped weapon
    // (line_clear()'d), if any — the "still defend yourself" half of Follow and Hold, so
    // a minion doing either isn't a free hit for anything that wanders adjacent. Returns
    // whether it attacked (the caller should skip movement for the turn if so).
    auto try_minion_auto_defend = [&](Actor& minion) -> bool {
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
    };

    // The player's minions act after every hostile monster has had its turn. Each one
    // is Following (path toward the player, ignoring FOV — a summoned minion always
    // knows where its own summoner is, unlike a hostile monster tracking the player),
    // Holding (path toward and then stand at a specific tile — "guard this spot"),
    // AttackTarget (path toward/attack one specific enemy, by id), or Aggressive
    // (Follow's proactive sibling — see the MinionOrder doc comment in entity.hpp).
    // Follow, Hold, and Aggressive all defend themselves via try_minion_auto_defend()
    // instead of moving if a hostile is already in range. An AttackTarget minion whose
    // target has died or otherwise disappeared (actor_index_by_id returns -1) reverts
    // to Follow and just holds position for the rest of this turn, picking up the
    // chase next turn.
    for (auto& minion : level.monsters) {
      if (gs.mode == Mode::Dead) break;
      if (!minion.is_alive() || minion.allegiance != Allegiance::Player) continue;
      // Same extra-actions inner loop the hostile loop above uses, for exactly the same
      // reason — a minion is an Actor, so a fast summon (MinionTemplate::extra_actions,
      // 0 on every row today) gets its extra actions through the same one function the
      // player and every monster do.
      for (int action = 0; action < total_actions_for(minion); ++action) {
        if (gs.mode == Mode::Dead || !minion.is_alive()) break;

        // Same gear/consumable upkeep the hostile loop runs, through the same helpers —
        // a minion carrying a spare weapon or a potion uses it on exactly the same terms
        // a monster does.
        int hostile_dist = nearest_hostile_distance(minion);
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
            if (!try_monster_step(minion, move_dx, move_dy)) {
              if (!try_monster_step(minion, move_dx, 0)) try_monster_step(minion, 0, move_dy);
            }
          }
          continue;
        }

        if (minion.order == MinionOrder::Hold) {
          if (try_minion_auto_defend(minion)) continue;
          if (minion.x == minion.hold_x && minion.y == minion.hold_y) continue;  // already there
          auto path = level.map.find_path(minion.x, minion.y, minion.hold_x, minion.hold_y);
          if (!path.empty()) {
            int move_dx = path[0].first - minion.x;
            int move_dy = path[0].second - minion.y;
            if (!try_monster_step(minion, move_dx, move_dy)) {
              if (!try_monster_step(minion, move_dx, 0)) try_monster_step(minion, 0, move_dy);
            }
          }
          continue;
        }

        if (minion.order == MinionOrder::Aggressive) {
          if (try_minion_auto_defend(minion)) continue;  // already-adjacent hostiles first

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
              if (!try_monster_step(minion, move_dx, move_dy)) {
                if (!try_monster_step(minion, move_dx, 0)) try_monster_step(minion, 0, move_dy);
              }
            }
            continue;
          }
          // No hostile currently visible — fall through to the Follow movement below.
        }

        if (minion.order == MinionOrder::Follow) {
          if (try_minion_auto_defend(minion)) continue;
        }
        // Follow (also Aggressive with nothing currently visible to chase — its own
        // auto-defend attempt already happened above in that case). Close the distance
        // to the player; try_monster_step already refuses to step onto the player's own
        // tile, so this naturally stops once adjacent rather than trying to stack on them.
        auto path = level.map.find_path(minion.x, minion.y, gs.player.x, gs.player.y);
        if (!path.empty()) {
          int move_dx = path[0].first - minion.x;
          int move_dy = path[0].second - minion.y;
          try_monster_step(minion, move_dx, move_dy);
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
    for (size_t i = 0; i < level.monsters.size();) {
      if (!level.monsters[i].is_alive()) {
        level.monsters.erase(level.monsters.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
  };

  start_new_game(gs);

  // Debug convenience: `--floor=N` jumps straight to floor N at startup, so testing
  // deep floors doesn't require a long walk down through every floor above it. Not
  // meant for normal play; a missing/malformed value is just silently ignored.
  //
  // `--level=N` spawns the player already at level N, forcing the same Mode::LevelUp
  // prompt (N-1) times in a row so you allocate every point yourself, same as leveling
  // up for real — it calls level_up_once() directly rather than faking XP, so it's the
  // exact core path a big XP reward would also drive if it crossed several thresholds
  // at once (see level_up_once's comment). Combinable with --floor=N.
  //
  // `--reveal` shows every tile/monster/item on the current floor regardless of
  // exploration or FOV (still dimmed if not actually in current sight, matching the
  // remembered-terrain/monster look) — for eyeballing spawns and loot without having
  // to walk the whole floor first. Also debug-only, off by default.
  //
  // `--dump-loot` prints every weapon/armor/potion/monster on the floor reached via
  // --floor=N (floor 1 if that flag's absent) to stdout, then exits before the window
  // ever opens — a scriptable alternative to eyeballing --reveal in the live window,
  // for checking depth-gating (kWeaponTable etc.) actually filters as intended.
  //
  // `--give=<name>[,<name>...]` adds items straight to the carried inventory at
  // startup (not auto-equipped — same "found it, now equip it" flow as picking one up,
  // just skipping the walk), for testing a specific item without grinding to find one.
  // Comma-separated, e.g. --give="Dagger,Potion of Teleportation"; see
  // give_starting_item() above.
  //
  // `--seed=N` pins the shared RNG (see seed_rng() in rng.hpp) so a run is reproducible:
  // the same seed builds the same floors with the same monsters, gear and rolls. Handled
  // in the pre-scan at the top of main(), not here, since floor 1 is generated long
  // before this loop runs. Pairs with --dump-loot to make floor contents diffable.
  bool dump_loot = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--reveal") {
      gs.reveal_all = true;
      continue;
    }
    if (arg == "--dump-loot") {
      dump_loot = true;
      continue;
    }
    if (arg == "--fast-monsters") {
      continue;  // already handled by the pre-scan at the top of main()
    }
    if (arg.rfind("--seed=", 0) == 0) {
      continue;  // likewise — seeding has to happen before floor 1 exists
    }
    const std::string floor_prefix = "--floor=";
    if (arg.rfind(floor_prefix, 0) == 0) {
      int target_floor = std::atoi(arg.c_str() + floor_prefix.size());
      for (int f = 1; f < target_floor; ++f) descend(gs);
      continue;
    }
    const std::string level_prefix = "--level=";
    if (arg.rfind(level_prefix, 0) == 0) {
      int target_level = std::atoi(arg.c_str() + level_prefix.size());
      for (int lv = gs.player.level; lv < target_level; ++lv) level_up_once(gs);
      continue;
    }
    const std::string give_prefix = "--give=";
    if (arg.rfind(give_prefix, 0) == 0) {
      std::string names = arg.substr(give_prefix.size());
      size_t pos = 0;
      while (pos <= names.size()) {
        size_t comma = names.find(',', pos);
        std::string name = names.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!name.empty()) {
          if (give_starting_item(name, gs.player)) {
            add_message(gs, "Debug: added " + name + " to inventory.");
          } else {
            add_message(gs, "Debug: no item named \"" + name + "\" found.");
          }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
      continue;
    }
  }
  // Surfaces the LevelUp prompt if --level= queued any points, same as grant_xp does.
  if (gs.pending_attribute_points > 0) gs.mode = Mode::LevelUp;

  if (dump_loot) {
    Level& level = gs.level();
    std::cout << "Floor " << (gs.current_level + 1) << " loot:\n";
    for (const auto& item : level.items) {
      std::cout << "  weapon: " << item.weapon.name << " (" << describe_weapon(item.weapon) << ")\n";
    }
    for (const auto& armor_item : level.armor_items) {
      std::cout << "  armor: " << armor_item.armor.name << " (" << describe_armor(armor_item.armor) << ")\n";
    }
    for (const auto& ground_potion : level.potions) {
      std::cout << "  potion: " << ground_potion.potion.name << " (" << describe_potion(ground_potion.potion)
                 << ")\n";
    }
    // Monsters carry gear now, and everything non-intrinsic here is what they'll drop —
    // worth printing, since that's a real part of a floor's loot.
    std::cout << "Floor " << (gs.current_level + 1) << " monsters:\n";
    for (const auto& monster : level.monsters) {
      std::cout << "  " << monster.name << " (" << monster.hp << " HP, " << monster.weapon.name << " "
                << describe_weapon(monster.weapon) << ", STR " << monster.strength << ", DEX " << monster.dexterity
                << ", evasion " << monster.evasion;
      if (!monster.armor.is_intrinsic) std::cout << ", " << monster.armor.name;
      // A caster's spell and mana pool are as much a part of "what will this fight cost
      // you" as its weapon is — see the same line in the 'x' look panel.
      if (monster.spell_index >= 0) {
        std::cout << ", " << kSpellTable[static_cast<size_t>(monster.spell_index)].name << " " << monster.mana << "/"
                  << monster.max_mana << " MP";
      }
      std::cout << ")\n";
      for (const auto& w : monster.weapons) std::cout << "      carries weapon: " << w.name << "\n";
      for (const auto& a : monster.armors) std::cout << "      carries armor: " << a.name << "\n";
      for (const auto& p : monster.potions) std::cout << "      carries potion: " << p.name << "\n";
    }
    return 0;
  }

  while (gs.running) {
    Level& level = gs.level();

    // --- Render ---
    console.clear();

    if (gs.mode == Mode::WeaponMenu) {
      tcod::print(console, {0, 0}, "Weapons - press a letter to equip, Esc to close", tcod::ColorRGB{255, 255, 255},
                  std::nullopt);
      tcod::print(console, {0, 1}, "Equipped: " + gs.player.weapon.name + " (" + describe_weapon(gs.player.weapon) + ")",
                  tcod::ColorRGB{200, 200, 100}, std::nullopt);

      // Fists is always slot 'a', so you can always bail back to unarmed; carried
      // weapons fill 'b' onward.
      std::string fists_line = "a) Fists (" + describe_weapon(kFists) + ")";
      if (gs.player.weapon.is_intrinsic) fists_line += " [equipped]";
      tcod::print(console, {0, 3}, fists_line, tcod::ColorRGB{200, 200, 200}, std::nullopt);

      for (size_t i = 0; i < gs.player.weapons.size(); ++i) {
        std::string line = std::string(1, static_cast<char>('b' + i)) + ") " + gs.player.weapons[i].name + " (" +
                            describe_weapon(gs.player.weapons[i]) + ")";
        tcod::print(console, {0, 4 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
      }
    } else if (gs.mode == Mode::ArmorMenu) {
      tcod::print(console, {0, 0}, "Armor - press a letter to equip, Esc to close", tcod::ColorRGB{255, 255, 255},
                  std::nullopt);
      tcod::print(console, {0, 1}, "Equipped: " + gs.player.armor.name + " (" + describe_armor(gs.player.armor) + ")",
                  tcod::ColorRGB{200, 200, 100}, std::nullopt);

      // "Nothing" is always slot 'a', so you can always bail back to unarmored; carried
      // armor fills 'b' onward.
      std::string none_line = "a) " + kNoArmor.name + " (" + describe_armor(kNoArmor) + ")";
      if (gs.player.armor.is_intrinsic) none_line += " [equipped]";
      tcod::print(console, {0, 3}, none_line, tcod::ColorRGB{200, 200, 200}, std::nullopt);

      for (size_t i = 0; i < gs.player.armors.size(); ++i) {
        std::string line = std::string(1, static_cast<char>('b' + i)) + ") " + gs.player.armors[i].name + " (" +
                            describe_armor(gs.player.armors[i]) + ")";
        tcod::print(console, {0, 4 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
      }
    } else if (gs.mode == Mode::PotionMenu) {
      tcod::print(console, {0, 0}, "Potions - press a letter to drink, Esc to close", tcod::ColorRGB{255, 255, 255},
                  std::nullopt);

      if (gs.player.potions.empty()) {
        tcod::print(console, {0, 2}, "(no potions carried)", tcod::ColorRGB{120, 120, 120}, std::nullopt);
      }
      for (size_t i = 0; i < gs.player.potions.size(); ++i) {
        std::string line = std::string(1, static_cast<char>('a' + i)) + ") " + gs.player.potions[i].name + " (" +
                            describe_potion(gs.player.potions[i]) + ")";
        tcod::print(console, {0, 2 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
      }
    } else if (gs.mode == Mode::SpellMenu) {
      tcod::print(console, {0, 0}, "Spells - press a letter to cast, Esc to close", tcod::ColorRGB{255, 255, 255},
                  std::nullopt);

      auto known = known_spell_indices(gs.player.intelligence, gs.player.chosen_school);
      if (known.empty()) {
        tcod::print(console, {0, 2}, "(no spells known yet)", tcod::ColorRGB{120, 120, 120}, std::nullopt);
      }
      for (size_t i = 0; i < known.size(); ++i) {
        const Spell& s = kSpellTable[static_cast<size_t>(known[i])];
        bool is_active = gs.active_toggle_spell == known[i];
        std::string line;
        bool at_minion_cap = false;
        if (s.is_toggle) {
          line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (" +
                 std::to_string(s.tick_damage) + " dmg/turn in " + std::to_string(2 * s.aoe_radius + 1) + "x" +
                 std::to_string(2 * s.aoe_radius + 1) + ", " + std::to_string(s.tick_mana_cost) + " MP/turn) - " +
                 std::to_string(s.mana_cost) + " MP to toggle" + (is_active ? " [ACTIVE]" : "");
        } else if (s.is_summon) {
          const MinionTemplate& tmpl = kMinionTable[static_cast<size_t>(s.summon_template_index)];
          std::string duration_str =
              tmpl.duration_turns > 0 ? std::to_string(tmpl.duration_turns) + " turns" : "permanent";
          at_minion_cap = count_minions(level.monsters) >= kMaxMinions;
          line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (summons a " + tmpl.name + ", " +
                 duration_str + ") - " + std::to_string(s.mana_cost) + " MP" + (at_minion_cap ? " [AT CAP]" : "");
        } else if (s.is_melee_buff) {
          line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (+" + std::to_string(s.buff_amount) +
                 " melee damage, " + std::to_string(s.buff_turns) + " turns) - " + std::to_string(s.mana_cost) +
                 " MP";
        } else if (s.is_armor_buff) {
          line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (+" + std::to_string(s.buff_amount) +
                 " armor, " + std::to_string(s.buff_turns) + " turns) - " + std::to_string(s.mana_cost) + " MP";
        } else if (s.is_haste_buff) {
          line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (+" + std::to_string(s.buff_amount) +
                 " action/turn, " + std::to_string(s.buff_turns) + " turns) - " + std::to_string(s.mana_cost) + " MP";
        } else if (s.is_swap) {
          line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (swap places with a minion) - " +
                 std::to_string(s.mana_cost) + " MP";
        } else {
          line = std::string(1, static_cast<char>('a' + i)) + ") " + s.name + " (" + std::to_string(s.dice_count) +
                 "d" + std::to_string(s.dice_sides) + "+INT/3) - " + std::to_string(s.mana_cost) + " MP";
        }
        // Dimmed red instead of the usual grey once you can't actually afford it — a
        // currently-active toggle is always "affordable" to select again (turning it
        // off is always free) so it doesn't get the red treatment.
        bool affordable = is_active || (gs.player.mana >= s.mana_cost && !at_minion_cap);
        tcod::print(console, {0, 2 + static_cast<int>(i)}, line,
                    affordable ? tcod::ColorRGB{200, 200, 200} : tcod::ColorRGB{150, 80, 80}, std::nullopt);
      }
    } else if (gs.mode == Mode::Drop) {
      tcod::print(console, {0, 0}, "Drop - press a letter to drop, Esc to cancel", tcod::ColorRGB{255, 255, 255},
                  std::nullopt);

      auto slots = drop_slots(gs.player);
      if (slots.empty()) {
        tcod::print(console, {0, 2}, "(nothing to drop)", tcod::ColorRGB{120, 120, 120}, std::nullopt);
      }
      for (size_t i = 0; i < slots.size(); ++i) {
        char letter = static_cast<char>('a' + i);
        std::string line;
        if (slots[i].kind == ItemKind::Weapon) {
          const Weapon& w = (slots[i].index == -1) ? gs.player.weapon : gs.player.weapons[static_cast<size_t>(slots[i].index)];
          line = std::string(1, letter) + ") " + w.name + " (" + describe_weapon(w) + ")";
          if (slots[i].index == -1) line += " [equipped]";
        } else if (slots[i].kind == ItemKind::Armor) {
          const Armor& a = (slots[i].index == -1) ? gs.player.armor : gs.player.armors[static_cast<size_t>(slots[i].index)];
          line = std::string(1, letter) + ") " + a.name + " (" + describe_armor(a) + ")";
          if (slots[i].index == -1) line += " [equipped]";
        } else {
          const Potion& p = gs.player.potions[static_cast<size_t>(slots[i].index)];
          line = std::string(1, letter) + ") " + p.name + " (" + describe_potion(p) + ")";
        }
        tcod::print(console, {0, 2 + static_cast<int>(i)}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
      }
    } else if (gs.mode == Mode::Dead) {
      tcod::print(console, {0, 0}, "You died, slain by the " + gs.death_cause + ".", tcod::ColorRGB{255, 80, 80},
                  std::nullopt);
      tcod::print(console, {0, 2}, "Press any key to start a new game, or Esc to quit.", tcod::ColorRGB{200, 200, 200},
                  std::nullopt);
    } else if (gs.mode == Mode::MessageLog) {
      tcod::print(console, {0, 0}, "Message Log - j/k or arrows to scroll, ']' or Esc to close",
                  tcod::ColorRGB{255, 255, 255}, std::nullopt);

      int visible_rows = SCREEN_HEIGHT - 1;
      int total = static_cast<int>(gs.message_log.size());
      int max_scroll = std::max(0, total - visible_rows);
      gs.log_scroll = std::min(gs.log_scroll, max_scroll);  // clamp in case the log shrank (e.g. after a restart)

      // Oldest at top, newest at bottom, like a terminal scrollback — log_scroll is how
      // many lines scrolled up from the bottom (0 = showing the most recent messages).
      int end_index = total - gs.log_scroll;
      int start_index = std::max(0, end_index - visible_rows);
      for (int i = start_index; i < end_index; ++i) {
        int row = 1 + (i - start_index);
        tcod::print(console, {0, row}, gs.message_log[static_cast<size_t>(i)], tcod::ColorRGB{200, 200, 200},
                    std::nullopt);
      }
    } else if (gs.mode == Mode::Help) {
      tcod::print(console, {0, 0}, "Controls - '?' or Esc to close", tcod::ColorRGB{255, 255, 255}, std::nullopt);
      static const std::vector<std::string> kHelpLines = {
          "",
          "Arrows / hjkl / yubn (diagonals)  Move; walks into an enemy to attack, or",
          "                                  swaps places with your own minion",
          ".                                 Wait a turn",
          ">  <                              Stairs down/up (must be standing on them)",
          "g                                 Pick up everything on your tile",
          "w  a  q                           Weapon / Armor / Potion menu (equip or drink)",
          "d                                 Drop a weapon, armor, or potion",
          "f                                 Fire the equipped ranged weapon (move to target,",
          "                                  Enter to loose it, Esc to cancel)",
          "z                                 Cast a known spell",
          "m                                 Command a minion or all of them (roster menu; Shift+A",
          "                                  there jumps straight to All)",
          "o  p                              Cycle command focus to the next/previous minion",
          "Shift+P                           Return focus to yourself",
          "f  g  Enter                       While focused on a minion instead: Follow / go",
          "                                  Aggressive / confirm Attack or Hold (sidebar shows",
          "                                  each minion's order as [F]ollow / [G]o aggressive /",
          "                                  [H]old / [A]ttack)",
          "x                                 Look around (move the cursor, side panel shows",
          "                                  details); x or Esc to close",
          "]                                 Message log (full scrollback)",
          "Shift+S  Shift+D  Shift+I         On level up: spend the point on STR/DEX/INT",
          "Shift+C  Shift+U  Shift+M         At Intelligence 4: choose Caster, Summoner, or",
          "                                  Combat Mage (once, permanent)",
          "?                                 This screen",
          "Esc                               Quit (or close the current menu)",
      };
      for (size_t i = 0; i < kHelpLines.size(); ++i) {
        tcod::print(console, {0, 1 + static_cast<int>(i)}, kHelpLines[i], tcod::ColorRGB{200, 200, 200},
                    std::nullopt);
      }
    } else if (gs.mode == Mode::MinionRoster) {
      tcod::print(console, {0, 0}, "Command a minion - press a letter, Esc to close", tcod::ColorRGB{255, 255, 255},
                  std::nullopt);
      // "All" is a fixed hotkey (Shift+A) pinned above the roster rather than a letter
      // tacked onto the end of it — a trailing letter shifts around as the pack's size
      // changes (and got long enough with a real attack target named to run off the
      // sidebar in the equivalent per-minion list, see minion_order_flag() above), so
      // anchoring it first keeps the ordering predictable regardless of pack size.
      tcod::print(console, {0, 2}, "Shift+A) All minions at once", tcod::ColorRGB{200, 200, 200}, std::nullopt);
      // Each living minion then gets its own letter, in level.monsters order (stable
      // turn to turn barring a death).
      int row = 4;
      char letter = 'a';
      for (const auto& m : level.monsters) {
        if (m.allegiance != Allegiance::Player || !m.is_alive()) continue;
        std::string line =
            std::string(1, letter) + ") " + m.name + " (" + describe_minion_order(m, level.monsters) + ")";
        tcod::print(console, {0, row}, line, tcod::ColorRGB{200, 200, 200}, std::nullopt);
        ++row;
        ++letter;
      }
    } else if (gs.mode == Mode::SchoolChoice) {
      // Full-screen forced prompt, same shape as MinionRoster above rather than
      // LevelUp's one-line CONTEXT_ROW style — this needs room to explain all three
      // paths, since it's a permanent, run-defining choice rather than a quick stat bump.
      tcod::print(console, {0, 0},
                  "You have grown wise enough to specialize your magic. Choose a path - this choice is permanent.",
                  tcod::ColorRGB{255, 255, 255}, std::nullopt);
      tcod::print(console, {0, 2},
                  "Shift+C) Caster      -- offensive magic: Fireball, Sandstorm, Lightning Bolt",
                  tcod::ColorRGB{200, 200, 200}, std::nullopt);
      tcod::print(console, {0, 3},
                  "Shift+U) Summoner    -- conjury and command: Raise Skeleton, Place Swap, Summon Demon",
                  tcod::ColorRGB{200, 200, 200}, std::nullopt);
      tcod::print(console, {0, 4},
                  "Shift+M) Combat Mage -- self-buffs: Battle Fury, Iron Skin, Haste",
                  tcod::ColorRGB{200, 200, 200}, std::nullopt);
      tcod::print(console, {0, 6}, "Magic Dart and Energy Lance stay available whichever path you pick.",
                  tcod::ColorRGB{120, 120, 120}, std::nullopt);
    } else {
      update_monster_memory(level);

      // Row CONTEXT_ROW: a transient action prompt (level-up / spell targeting /
      // minion command) when one of those modes is active. Deliberately separate from
      // the message-log panel below — a long-running prompt shouldn't crowd out or get
      // crowded out by ordinary log messages.
      if (gs.mode == Mode::LevelUp) {
        std::string prompt = "*** LEVEL UP (now level " + std::to_string(gs.player.level) +
                              ")! Press Shift+S/D/I to raise Strength/Dexterity/Intelligence. ***";
        tcod::print(console, {0, CONTEXT_ROW}, prompt, tcod::ColorRGB{255, 255, 100}, std::nullopt);
      } else if (gs.mode == Mode::Targeting) {
        const Spell& casting_spell = kSpellTable[static_cast<size_t>(gs.casting_spell_index)];
        std::string prompt = "Casting " + casting_spell.name + " (" + std::to_string(casting_spell.mana_cost) +
                              " MP) - move to target, Enter to fire, Esc to cancel.";
        tcod::print(console, {0, CONTEXT_ROW}, prompt, tcod::ColorRGB{255, 255, 100}, std::nullopt);
      } else if (gs.mode == Mode::MinionFocus) {
        std::string who = "your minion";
        if (gs.commanding_all_minions) {
          who = "all minions";
        } else {
          int fi = actor_index_by_id(level.monsters, gs.focused_minion_id);
          if (fi >= 0) who = level.monsters[static_cast<size_t>(fi)].name;
        }
        std::string prompt = "Commanding " + who +
                              " - move to a monster (attack), your own tile (follow), or elsewhere "
                              "(hold), Enter to confirm, F to follow, Esc to cancel.";
        tcod::print(console, {0, CONTEXT_ROW}, prompt, tcod::ColorRGB{255, 255, 100}, std::nullopt);
      } else if (gs.mode == Mode::Look) {
        tcod::print(console, {0, CONTEXT_ROW}, "Looking around - move the cursor to inspect, Esc to close.",
                    tcod::ColorRGB{255, 255, 100}, std::nullopt);
      } else if (gs.mode == Mode::RangedAttack) {
        std::string prompt = "Firing your " + gs.player.weapon.name + " - move to target, Enter to fire, Esc to cancel.";
        tcod::print(console, {0, CONTEXT_ROW}, prompt, tcod::ColorRGB{255, 255, 100}, std::nullopt);
      }

      // --- Sidebar: character stats, then who's around ("at a glance") ---
      draw_panel(console, SIDEBAR_X, SIDEBAR_Y, SIDEBAR_W, SIDEBAR_H, "Status");
      int sb_x = SIDEBAR_X + 1;
      int sb_row = SIDEBAR_Y + 1;
      int sb_bottom = SIDEBAR_Y + SIDEBAR_H - 2;  // last usable interior row
      // Appends one line and advances sb_row, silently dropping anything past the
      // panel's interior instead of overflowing into its border. Also clips the text
      // itself to the panel's interior width — the sidebar sits flush against the
      // console's right edge with nothing beyond it, so an unclipped long line (e.g. a
      // minion's status naming a long monster name) would run right off the window
      // instead of just looking crowded.
      int sb_max_width = SIDEBAR_W - 2;
      auto sb_print = [&](const std::string& text, tcod::ColorRGB color) {
        if (sb_row > sb_bottom) return;
        std::string clipped = text;
        if (static_cast<int>(clipped.size()) > sb_max_width) {
          clipped = clipped.substr(0, static_cast<size_t>(std::max(0, sb_max_width - 3))) + "...";
        }
        tcod::print(console, {sb_x, sb_row}, clipped, color, std::nullopt);
        ++sb_row;
      };

      sb_print("HP: " + std::to_string(gs.player.hp) + "/" + std::to_string(gs.player.max_hp),
               tcod::ColorRGB{255, 255, 255});
      sb_print("MP: " + std::to_string(gs.player.mana) + "/" + std::to_string(gs.player.max_mana),
               tcod::ColorRGB{255, 255, 255});
      sb_print("Lvl: " + std::to_string(gs.player.level) + "  Floor: " + std::to_string(gs.current_level + 1),
               tcod::ColorRGB{255, 255, 255});

      // Appends "+N" to a stat only while its temp buff is active, so the HUD reflects
      // Potion of Strength/Dexterity/Intelligence without a separate buff tracker.
      auto stat_str = [](int base, int bonus) {
        return std::to_string(base) + (bonus > 0 ? "+" + std::to_string(bonus) : "");
      };
      sb_print("STR: " + stat_str(gs.player.strength, gs.player.temp_str_bonus), tcod::ColorRGB{200, 200, 200});
      sb_print("DEX: " + stat_str(gs.player.dexterity, gs.player.temp_dex_bonus), tcod::ColorRGB{200, 200, 200});
      sb_print("INT: " + stat_str(gs.player.intelligence, gs.player.temp_int_bonus), tcod::ColorRGB{200, 200, 200});
      // Evasion is a real, comparable number now — the same rating a monster's table row
      // authors — so it's worth showing rather than leaving DEX's effect implicit.
      sb_print("Eva: " + std::to_string(gs.player.evasion), tcod::ColorRGB{200, 200, 200});
      sb_print("Wpn: " + gs.player.weapon.name, tcod::ColorRGB{200, 200, 200});
      sb_print("  (" + describe_weapon(gs.player.weapon) + ")", tcod::ColorRGB{150, 150, 150});
      sb_print("Arm: " + gs.player.armor.name, tcod::ColorRGB{200, 200, 200});
      sb_print("  (" + describe_armor(gs.player.armor) + ")", tcod::ColorRGB{150, 150, 150});
      // A running toggle spell (e.g. Sandstorm) has no other on-screen presence besides
      // its aura tile overlay — this tag is the only text indicator it's still active.
      if (gs.active_toggle_spell >= 0) {
        sb_print("[" + kSpellTable[static_cast<size_t>(gs.active_toggle_spell)].name + "]",
                 tcod::ColorRGB{255, 255, 100});
      }

      // Every active temporary buff, with what it's worth and how many turns are left.
      // The STR/DEX/INT lines above already show their potion bonuses inline as "+N", but
      // that says nothing about how much longer they last, and the three spell buffs
      // (Battle Fury / Iron Skin / Haste) had no on-screen presence at all before this —
      // Haste in particular needs one, since "the world didn't move" is otherwise
      // indistinguishable from a dropped keypress. One list, so there's a single place to
      // look and a single place to add the next buff.
      //
      // Sits above the Enemies/Minions lists for the same reason the Look block does:
      // sb_print silently drops anything past the panel's bottom, and a timer you're
      // playing around matters more than the tail of a long enemy list. Rows only exist
      // while something is actually active, so the usual case costs nothing.
      {
        auto buff_row = [&](const std::string& label, int bonus, int turns) {
          if (turns <= 0) return;
          sb_print("  " + label + " +" + std::to_string(bonus) + " (" + std::to_string(turns) + ")",
                   tcod::ColorRGB{160, 255, 160});
        };
        bool any_buff = gs.player.temp_str_turns > 0 || gs.player.temp_dex_turns > 0 || gs.player.temp_int_turns > 0 ||
                        gs.player.temp_melee_damage_turns > 0 || gs.player.temp_armor_turns > 0 ||
                        gs.player.temp_extra_actions_turns > 0;
        if (any_buff) {
          sb_print("Buffs:", tcod::ColorRGB{200, 255, 200});
          buff_row("STR", gs.player.temp_str_bonus, gs.player.temp_str_turns);
          buff_row("DEX", gs.player.temp_dex_bonus, gs.player.temp_dex_turns);
          buff_row("INT", gs.player.temp_int_bonus, gs.player.temp_int_turns);
          buff_row("Melee dmg", gs.player.temp_melee_damage_bonus, gs.player.temp_melee_damage_turns);
          buff_row("Armor", gs.player.temp_armor_bonus, gs.player.temp_armor_turns);
          buff_row("Actions", gs.player.temp_extra_actions_bonus, gs.player.temp_extra_actions_turns);
        }
      }

      // Placed ahead of Enemies/Minions (rather than after) so it can never get
      // silently dropped by sb_print's bottom-of-panel clamp when the pack/enemy
      // lists below run long — the thing you're actively examining is the more
      // important thing to keep on screen right now.
      if (gs.mode == Mode::Look) {
        ++sb_row;
        sb_print("Looking:", tcod::ColorRGB{200, 200, 255});
        bool explored = level.map.is_explored(gs.target_x, gs.target_y);
        if (!explored && !gs.reveal_all) {
          sb_print("  (unexplored)", tcod::ColorRGB{120, 120, 120});
        } else {
          bool tile_in_fov = level.map.is_in_fov(gs.target_x, gs.target_y);
          bool is_stairs_down = (gs.target_x == level.stairs_down_x && gs.target_y == level.stairs_down_y);
          bool is_stairs_up = level.has_stairs_up && (gs.target_x == level.entry_x && gs.target_y == level.entry_y);
          std::string terrain = is_stairs_down                             ? "Stairs down"
                                 : is_stairs_up                             ? "Stairs up"
                                 : level.map.at(gs.target_x, gs.target_y).is_hole ? "Hole"
                                 : level.map.is_walkable(gs.target_x, gs.target_y) ? "Floor"
                                                                              : "Wall";
          sb_print("  " + terrain, tcod::ColorRGB{200, 200, 200});

          if (!tile_in_fov && !gs.reveal_all) {
            // Remembered terrain layout is fine to show, but not live occupant
            // details — same rule the map rendering itself already follows (items/
            // monsters only ever show up while actually in view).
            sb_print("  (out of view)", tcod::ColorRGB{120, 120, 120});
          } else {
            bool found_anything = false;
            int mi = monster_at(level.monsters, gs.target_x, gs.target_y);
            if (mi >= 0) {
              const Actor& m = level.monsters[static_cast<size_t>(mi)];
              sb_print("  " + m.name, m.color);
              sb_print("    HP: " + std::to_string(m.hp) + "/" + std::to_string(m.max_hp),
                       tcod::ColorRGB{200, 200, 200});
              sb_print("    Wpn: " + m.weapon.name + " (" + describe_weapon(m.weapon) + ")",
                       tcod::ColorRGB{200, 200, 200});
              // Monsters wear armor and carry packs now, so 'x' is the way to see what
              // a fight is actually going to cost you — and what it'll drop.
              if (!m.armor.is_intrinsic) {
                sb_print("    Arm: " + m.armor.name + " (" + describe_armor(m.armor) + ")",
                         tcod::ColorRGB{200, 200, 200});
              }
              sb_print("    Eva: " + std::to_string(m.evasion) + "  STR: " + std::to_string(m.strength),
                       tcod::ColorRGB{150, 150, 150});
              // A caster's remaining mana is the single most useful thing to know about
              // it — the Goblin Shaman's whole design is a finite dart budget, so being
              // able to check how much of it is left is what makes waiting it out a real
              // decision rather than a guess. Only shown for something that actually
              // casts; every other monster has max_mana 0 and would just print "0/0".
              if (m.spell_index >= 0) {
                sb_print("    MP: " + std::to_string(m.mana) + "/" + std::to_string(m.max_mana) + " (" +
                             kSpellTable[static_cast<size_t>(m.spell_index)].name + ")",
                         tcod::ColorRGB{150, 150, 220});
              }
              for (const auto& carried : m.weapons) sb_print("    - " + carried.name, tcod::ColorRGB{170, 170, 200});
              for (const auto& carried : m.potions) sb_print("    - " + carried.name, carried.color);
              if (m.allegiance == Allegiance::Player) {
                sb_print("    " + describe_minion_order(m, level.monsters), tcod::ColorRGB{200, 200, 200});
              }
              found_anything = true;
            }
            for (const auto& gi : level.items) {
              if (gi.x != gs.target_x || gi.y != gs.target_y) continue;
              sb_print("  " + gi.weapon.name + " (" + describe_weapon(gi.weapon) + ")",
                       tcod::ColorRGB{200, 200, 255});
              found_anything = true;
            }
            for (const auto& ga : level.armor_items) {
              if (ga.x != gs.target_x || ga.y != gs.target_y) continue;
              sb_print("  " + ga.armor.name + " (" + describe_armor(ga.armor) + ")", tcod::ColorRGB{180, 220, 200});
              found_anything = true;
            }
            for (const auto& gp : level.potions) {
              if (gp.x != gs.target_x || gp.y != gs.target_y) continue;
              sb_print("  " + gp.potion.name + " (" + describe_potion(gp.potion) + ")", gp.potion.color);
              found_anything = true;
            }
            if (!found_anything) sb_print("  (nothing else here)", tcod::ColorRGB{120, 120, 120});
          }
        }
        ++sb_row;
      }

      sb_print("Enemies:", tcod::ColorRGB{255, 150, 150});
      bool any_enemy = false;
      for (const auto& m : level.monsters) {
        if (m.allegiance != Allegiance::Hostile || !m.is_alive()) continue;
        if (!level.map.is_in_fov(m.x, m.y)) continue;
        sb_print("  " + m.name + " (" + std::to_string(m.hp) + "/" + std::to_string(m.max_hp) + ")", m.color);
        any_enemy = true;
      }
      if (!any_enemy) sb_print("  (none in view)", tcod::ColorRGB{120, 120, 120});

      ++sb_row;
      sb_print("Minions:", tcod::ColorRGB{150, 220, 255});
      bool any_minion = false;
      for (const auto& m : level.monsters) {
        if (m.allegiance != Allegiance::Player || !m.is_alive()) continue;
        bool focused = !gs.commanding_all_minions && m.id == gs.focused_minion_id;
        tcod::ColorRGB color = focused ? tcod::ColorRGB{255, 255, 100} : m.color;
        // [F]ollow / [H]old / [A]ttack — a flag instead of describe_minion_order()'s
        // full sentence, which could run past the sidebar's width once it named an
        // attack target (e.g. "attacking the Goblin Slinger").
        sb_print("  " + std::string(focused ? "*" : " ") + m.name + " [" + minion_order_flag(m) + "]", color);
        any_minion = true;
      }
      if (!any_minion) sb_print("  (none)", tcod::ColorRGB{120, 120, 120});

      // --- Message log panel: always exactly the last MESSAGE_ROWS distinct
      // messages, oldest on top, one per line — never wrapped or combined, even if
      // several things happened on the same turn. (']' opens full scrollback.)
      draw_panel(console, LOG_PANEL_X, LOG_PANEL_Y, LOG_PANEL_W, LOG_PANEL_H, "Log");
      int log_total = static_cast<int>(gs.message_log.size());
      for (int row = 0; row < MESSAGE_ROWS; ++row) {
        int idx = log_total - MESSAGE_ROWS + row;
        if (idx < 0) continue;
        tcod::print(console, {LOG_PANEL_X + 1, LOG_PANEL_Y + 1 + row}, gs.message_log[static_cast<size_t>(idx)],
                    tcod::ColorRGB{255, 255, 100}, std::nullopt);
      }

      // --- Map panel ---
      // Camera: centers on the player, except while aiming or looking around
      // (Targeting/RangedAttack/MinionFocus/Look), where it centers on the cursor
      // instead so a cursor that has wandered away from the player (MinionFocus and
      // Look have no range limit, unlike a spell's Targeting or a weapon's
      // RangedAttack) never drifts off-screen. Clamped so the viewport never scrolls
      // past the map's edge — same clamp-to-bounds shape regardless of which point
      // it's following.
      int camera_focus_x = gs.player.x;
      int camera_focus_y = gs.player.y;
      if (gs.mode == Mode::Targeting || gs.mode == Mode::MinionFocus || gs.mode == Mode::Look ||
          gs.mode == Mode::RangedAttack) {
        camera_focus_x = gs.target_x;
        camera_focus_y = gs.target_y;
      }
      int camera_x = std::clamp(camera_focus_x - MAP_VIEW_W / 2, 0, std::max(0, level.map.width() - MAP_VIEW_W));
      int camera_y = std::clamp(camera_focus_y - MAP_VIEW_H / 2, 0, std::max(0, level.map.height() - MAP_VIEW_H));
      // True for any dungeon tile currently inside the scrolled viewport — every
      // entity draw below skips (rather than writes out of the map panel's bounds,
      // or into the sidebar/log next to it) anything that fails this.
      auto in_view = [&](int x, int y) {
        return x >= camera_x && x < camera_x + MAP_VIEW_W && y >= camera_y && y < camera_y + MAP_VIEW_H;
      };

      draw_panel(console, MAP_PANEL_X, MAP_PANEL_Y, MAP_PANEL_W, MAP_PANEL_H,
                 "Floor " + std::to_string(gs.current_level + 1));

      int view_x_end = std::min(camera_x + MAP_VIEW_W, level.map.width());
      int view_y_end = std::min(camera_y + MAP_VIEW_H, level.map.height());
      for (int y = camera_y; y < view_y_end; ++y) {
        for (int x = camera_x; x < view_x_end; ++x) {
          // Never seen and not revealing: leave blank.
          if (!level.map.is_explored(x, y) && !gs.reveal_all) continue;

          bool walkable = level.map.is_walkable(x, y);
          bool is_hole = level.map.at(x, y).is_hole;
          bool visible = level.map.is_in_fov(x, y);
          bool is_stairs_down = (x == level.stairs_down_x && y == level.stairs_down_y);
          bool is_stairs_up = level.has_stairs_up && (x == level.entry_x && y == level.entry_y);

          auto& cell = console.at(MAP_ORIGIN_X + x - camera_x, MAP_ORIGIN_Y + y - camera_y);
          if (is_stairs_down) {
            cell.ch = '>';
          } else if (is_stairs_up) {
            cell.ch = '<';
          } else if (is_hole) {
            cell.ch = '^';
          } else {
            cell.ch = walkable ? '.' : '#';
          }

          if (visible) {
            if (is_stairs_down || is_stairs_up) {
              cell.fg = tcod::ColorRGB{255, 255, 150};
            } else if (is_hole) {
              cell.fg = tcod::ColorRGB{180, 90, 40};
            } else {
              cell.fg = walkable ? tcod::ColorRGB{160, 160, 160} : tcod::ColorRGB{90, 90, 90};
            }
          } else {
            // Remembered but currently out of sight: dimmed fog-of-war shading.
            if (is_stairs_down || is_stairs_up) {
              cell.fg = tcod::ColorRGB{110, 110, 70};
            } else if (is_hole) {
              cell.fg = tcod::ColorRGB{70, 35, 15};
            } else {
              cell.fg = walkable ? tcod::ColorRGB{60, 60, 60} : tcod::ColorRGB{35, 35, 35};
            }
          }
        }
      }

      // Remembered monster sightings: dimmed, drawn only where we can't currently see
      // (the live loop below draws anything actually visible, on top, at full brightness).
      for (const auto& remembered : level.remembered_monsters) {
        if (level.map.is_in_fov(remembered.x, remembered.y)) continue;
        if (!in_view(remembered.x, remembered.y)) continue;
        auto& cell = console.at(MAP_ORIGIN_X + remembered.x - camera_x, MAP_ORIGIN_Y + remembered.y - camera_y);
        cell.ch = remembered.glyph;
        cell.fg = dim_color(remembered.color);
      }

      // Items/monsters only show up while actually in view, unlike remembered terrain
      // — unless --reveal is forcing them on, in which case out-of-fov ones are drawn
      // dimmed, same tier as remembered terrain/monsters.
      for (const auto& item : level.items) {
        bool visible = level.map.is_in_fov(item.x, item.y);
        if (!visible && !gs.reveal_all) continue;
        if (!in_view(item.x, item.y)) continue;
        auto& cell = console.at(MAP_ORIGIN_X + item.x - camera_x, MAP_ORIGIN_Y + item.y - camera_y);
        cell.ch = '/';
        tcod::ColorRGB color{200, 200, 255};
        cell.fg = visible ? color : dim_color(color);
      }

      for (const auto& armor_item : level.armor_items) {
        bool visible = level.map.is_in_fov(armor_item.x, armor_item.y);
        if (!visible && !gs.reveal_all) continue;
        if (!in_view(armor_item.x, armor_item.y)) continue;
        auto& cell = console.at(MAP_ORIGIN_X + armor_item.x - camera_x, MAP_ORIGIN_Y + armor_item.y - camera_y);
        cell.ch = '[';
        tcod::ColorRGB color{180, 220, 200};
        cell.fg = visible ? color : dim_color(color);
      }

      for (const auto& ground_potion : level.potions) {
        bool visible = level.map.is_in_fov(ground_potion.x, ground_potion.y);
        if (!visible && !gs.reveal_all) continue;
        if (!in_view(ground_potion.x, ground_potion.y)) continue;
        auto& cell =
            console.at(MAP_ORIGIN_X + ground_potion.x - camera_x, MAP_ORIGIN_Y + ground_potion.y - camera_y);
        cell.ch = ground_potion.potion.glyph;
        cell.fg = visible ? ground_potion.potion.color : dim_color(ground_potion.potion.color);
      }

      for (const auto& monster : level.monsters) {
        bool visible = level.map.is_in_fov(monster.x, monster.y);
        if (!visible && !gs.reveal_all) continue;
        if (!in_view(monster.x, monster.y)) continue;
        auto& cell = console.at(MAP_ORIGIN_X + monster.x - camera_x, MAP_ORIGIN_Y + monster.y - camera_y);
        cell.ch = monster.glyph;
        cell.fg = visible ? monster.color : dim_color(monster.color);
      }

      // Spells currently in flight (only visible ones matter, same as monsters/items).
      for (const auto& proj : level.projectiles) {
        if (proj.path_index == 0 || proj.path_index > proj.path.size()) continue;
        auto [px, py] = proj.path[proj.path_index - 1];
        if (!level.map.is_in_fov(px, py)) continue;
        if (!in_view(px, py)) continue;
        auto& cell = console.at(MAP_ORIGIN_X + px - camera_x, MAP_ORIGIN_Y + py - camera_y);
        cell.ch = proj.glyph;
        cell.fg = proj.color;
      }

      // The player is always inside the viewport by construction (the camera clamp
      // keeps whatever it's centered on in view), so this is drawn unconditionally.
      console.at(MAP_ORIGIN_X + gs.player.x - camera_x, MAP_ORIGIN_Y + gs.player.y - camera_y).ch = gs.player.glyph;
      console.at(MAP_ORIGIN_X + gs.player.x - camera_x, MAP_ORIGIN_Y + gs.player.y - camera_y).fg = gs.player.color;

      // A running toggle spell (e.g. Sandstorm) gets a persistent highlight around the
      // player showing its current radius, recentered every frame since the aura
      // follows the player rather than sitting still — same recolor-not-overwrite
      // treatment as the AoE targeting preview below, so monsters/terrain inside it
      // stay visible. Uses the spell's own color so different toggle spells (if more
      // are ever added) read as visually distinct auras.
      if (gs.active_toggle_spell >= 0) {
        const Spell& storm = kSpellTable[static_cast<size_t>(gs.active_toggle_spell)];
        for (int by = gs.player.y - storm.aoe_radius; by <= gs.player.y + storm.aoe_radius; ++by) {
          for (int bx = gs.player.x - storm.aoe_radius; bx <= gs.player.x + storm.aoe_radius; ++bx) {
            if (bx < 0 || by < 0 || bx >= level.map.width() || by >= level.map.height()) continue;
            if (!level.map.is_explored(bx, by) && !gs.reveal_all) continue;
            if (!in_view(bx, by)) continue;
            console.at(MAP_ORIGIN_X + bx - camera_x, MAP_ORIGIN_Y + by - camera_y).fg = storm.color;
          }
        }
        // Re-mark the player's own tile on top so they stay visible inside the tint.
        console.at(MAP_ORIGIN_X + gs.player.x - camera_x, MAP_ORIGIN_Y + gs.player.y - camera_y).fg = gs.player.color;
      }

      if (gs.mode == Mode::Targeting) {
        const Spell& previewed_spell = kSpellTable[static_cast<size_t>(gs.casting_spell_index)];

        if (previewed_spell.is_swap) {
          // No projectile/line to preview for a swap — just mark the target tile,
          // colored by whether there's actually a minion there to swap with (matches
          // the Enter-fire check in own_minion_at()).
          if (in_view(gs.target_x, gs.target_y)) {
            bool has_minion = own_minion_at(level.monsters, gs.target_x, gs.target_y) >= 0;
            auto& cell = console.at(MAP_ORIGIN_X + gs.target_x - camera_x, MAP_ORIGIN_Y + gs.target_y - camera_y);
            cell.ch = 'X';
            cell.fg = has_minion ? tcod::ColorRGB{100, 220, 255} : tcod::ColorRGB{120, 60, 60};
          }
        } else {
          // Preview the shot: trace the same path a cast would take, and stop drawing at
          // the first tile that would actually stop it, so what you see is what you'd hit.
          auto preview = trace_path(gs.player.x, gs.player.y, gs.target_x, gs.target_y);
          for (size_t i = 0; i < preview.size(); ++i) {
            auto [x, y] = preview[i];
            bool blocked = level.map.blocks_projectile(x, y);
            bool has_monster = hostile_monster_at(level.monsters, x, y) >= 0;
            // A piercing spell's preview marks every hostile tile along the line as a
            // hit but keeps drawing the line past it — only a wall (or reaching max
            // range) actually stops it, matching advance_projectiles()'s own pierce
            // handling. Non-piercing spells are unchanged.
            bool pierce_hit = has_monster && previewed_spell.pierces;
            bool stops_here = blocked || (has_monster && !previewed_spell.pierces) || i + 1 == preview.size();
            if (in_view(x, y)) {
              auto& cell = console.at(MAP_ORIGIN_X + x - camera_x, MAP_ORIGIN_Y + y - camera_y);
              cell.ch = (stops_here || pierce_hit) ? 'X' : '*';
              cell.fg = (stops_here || pierce_hit) ? tcod::ColorRGB{255, 60, 60} : tcod::ColorRGB{150, 60, 60};
            }
            if (blocked || (has_monster && !previewed_spell.pierces)) break;
          }

          // AoE spells (Fireball etc.) also highlight the blast radius around wherever the
          // shot would actually come to rest — find_impact() applies the exact same
          // stopping rules advance_projectiles() uses, so this matches what firing now
          // would do. Recolors tiles rather than overwriting their glyph, so monsters/
          // terrain caught in the blast stay visible underneath the highlight.
          if (previewed_spell.aoe_radius > 0) {
            auto [impact_x, impact_y] = find_impact(preview, gs.player.x, gs.player.y, level.map, level.monsters);
            int radius = previewed_spell.aoe_radius;
            for (int by = impact_y - radius; by <= impact_y + radius; ++by) {
              for (int bx = impact_x - radius; bx <= impact_x + radius; ++bx) {
                if (bx < 0 || by < 0 || bx >= level.map.width() || by >= level.map.height()) continue;
                if (!level.map.is_explored(bx, by) && !gs.reveal_all) continue;
                if (!in_view(bx, by)) continue;
                console.at(MAP_ORIGIN_X + bx - camera_x, MAP_ORIGIN_Y + by - camera_y).fg = tcod::ColorRGB{255, 140, 60};
              }
            }
            // Re-mark the impact tile on top so the center stays visually distinct.
            if (in_view(impact_x, impact_y)) {
              auto& impact_cell = console.at(MAP_ORIGIN_X + impact_x - camera_x, MAP_ORIGIN_Y + impact_y - camera_y);
              impact_cell.ch = 'X';
              impact_cell.fg = tcod::ColorRGB{255, 60, 60};
            }
          }
        }
      }

      if (gs.mode == Mode::RangedAttack) {
        // Same aim-preview line as Mode::Targeting above, minus the AoE step — no
        // player weapon has a blast radius today, so there's nothing extra to predict.
        auto preview = trace_path(gs.player.x, gs.player.y, gs.target_x, gs.target_y);
        for (size_t i = 0; i < preview.size(); ++i) {
          auto [x, y] = preview[i];
          bool blocked = level.map.blocks_projectile(x, y);
          bool has_monster = hostile_monster_at(level.monsters, x, y) >= 0;
          bool stops_here = blocked || has_monster || i + 1 == preview.size();
          if (in_view(x, y)) {
            auto& cell = console.at(MAP_ORIGIN_X + x - camera_x, MAP_ORIGIN_Y + y - camera_y);
            cell.ch = stops_here ? 'X' : '*';
            cell.fg = stops_here ? tcod::ColorRGB{255, 60, 60} : tcod::ColorRGB{150, 60, 60};
          }
          if (blocked || has_monster) break;
        }
      }

      if (gs.mode == Mode::MinionFocus) {
        // Highlights whichever minion(s) are currently being commanded — once the
        // cursor wanders away from a minion's own tile there's otherwise no way to
        // tell who you're still aiming for. Recolors the glyph (keeps it, rather than
        // overwriting with a marker) so it still reads as "that minion", just lit up.
        for (const auto& m : level.monsters) {
          if (m.allegiance != Allegiance::Player || !m.is_alive()) continue;
          if (!gs.commanding_all_minions && m.id != gs.focused_minion_id) continue;
          bool visible = level.map.is_in_fov(m.x, m.y);
          if (!visible && !gs.reveal_all) continue;  // not drawn at all this frame either way
          if (!in_view(m.x, m.y)) continue;
          console.at(MAP_ORIGIN_X + m.x - camera_x, MAP_ORIGIN_Y + m.y - camera_y).fg = tcod::ColorRGB{255, 255, 100};
        }

        // If any commanded minion currently has an AttackTarget order — or an
        // Aggressive one that's actively chasing something — highlight that monster
        // too, in a color distinct from the minion tint above — with more than one of
        // the same monster type in view (two Goblins, say) there's otherwise no way to
        // tell which one is actually assigned versus just standing nearby.
        for (const auto& m : level.monsters) {
          if (m.allegiance != Allegiance::Player || !m.is_alive()) continue;
          if (!gs.commanding_all_minions && m.id != gs.focused_minion_id) continue;
          if (m.order != MinionOrder::AttackTarget && m.order != MinionOrder::Aggressive) continue;
          int ti = actor_index_by_id(level.monsters, m.attack_target_id);
          if (ti < 0) continue;  // no current target — AttackTarget will revert to Follow on its own,
                                  // Aggressive just has nothing in view to chase yet
          const Actor& target = level.monsters[static_cast<size_t>(ti)];
          bool target_visible = level.map.is_in_fov(target.x, target.y);
          if (!target_visible && !gs.reveal_all) continue;
          if (!in_view(target.x, target.y)) continue;
          console.at(MAP_ORIGIN_X + target.x - camera_x, MAP_ORIGIN_Y + target.y - camera_y).fg =
              tcod::ColorRGB{255, 60, 255};
        }

        // No line trace or AoE like a spell — confirming here either attacks (a
        // hostile monster under the cursor) or holds (any other walkable tile), see
        // the Enter handler below, so the cursor color previews which one: red for
        // attack (the monster's own glyph stays visible, just tinted, same as the
        // spell-targeting cursor above), green for hold, dim grey for an invalid tile
        // (a wall, or something already standing there that isn't a valid target). The
        // camera follows this cursor (see camera_focus_x/y above) so it's always in
        // view, unlike a spell's range-limited targeting cursor.
        if (in_view(gs.target_x, gs.target_y)) {
          auto& cell = console.at(MAP_ORIGIN_X + gs.target_x - camera_x, MAP_ORIGIN_Y + gs.target_y - camera_y);
          bool walkable = level.map.is_walkable(gs.target_x, gs.target_y);
          int hostile_hit = hostile_monster_at(level.monsters, gs.target_x, gs.target_y);
          if (hostile_hit >= 0) {
            cell.fg = tcod::ColorRGB{255, 60, 60};
          } else if (walkable && monster_at(level.monsters, gs.target_x, gs.target_y) < 0) {
            cell.ch = 'X';
            cell.fg = tcod::ColorRGB{100, 220, 140};
          } else {
            cell.ch = 'X';
            cell.fg = tcod::ColorRGB{120, 120, 120};
          }
        }
      }

      if (gs.mode == Mode::Look) {
        // Plain recolor, no glyph override — unlike Targeting/MinionFocus there's no
        // action being previewed here, just "this is what the cursor is on", so
        // whatever's actually there (terrain/item/monster) should stay fully visible.
        if (in_view(gs.target_x, gs.target_y)) {
          console.at(MAP_ORIGIN_X + gs.target_x - camera_x, MAP_ORIGIN_Y + gs.target_y - camera_y).fg =
              tcod::ColorRGB{255, 255, 255};
        }
      }
    }

    context.present(console);

    // --- Input / events ---
    SDL_Event event;
    SDL_WaitEvent(nullptr);  // Sleep until an event arrives (this is a turn-based game; no need to busy-loop).
    while (SDL_PollEvent(&event)) {
      context.convert_event_coordinates(event);

      if (event.type == SDL_EVENT_QUIT) {
        gs.running = false;
        continue;
      }
      if (event.type != SDL_EVENT_KEY_DOWN) continue;

      // Re-fetched fresh for every event (not reused from the outer render-time `level`
      // above): descend() can push_back onto `levels`, which may reallocate and would
      // dangle a reference held across more than one queued event in the same batch.
      Level& level = gs.level();

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
      auto cycle_minion_focus = [&](int direction) -> bool {
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
      };

      if (gs.mode == Mode::Dead) {
        if (event.key.key == SDLK_ESCAPE) {
          gs.running = false;
        } else {
          start_new_game(gs);
        }
        continue;
      }

      if (gs.mode == Mode::MessageLog) {
        if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_RIGHTBRACKET) {
          gs.mode = Mode::Playing;
        } else if (event.key.key == SDLK_K || event.key.key == SDLK_UP) {
          int visible_rows = SCREEN_HEIGHT - 1;
          int max_scroll = std::max(0, static_cast<int>(gs.message_log.size()) - visible_rows);
          gs.log_scroll = std::min(gs.log_scroll + 1, max_scroll);
        } else if (event.key.key == SDLK_J || event.key.key == SDLK_DOWN) {
          gs.log_scroll = std::max(gs.log_scroll - 1, 0);
        }
        continue;
      }

      if (gs.mode == Mode::Help) {
        // Same unshifted-keycode-plus-modifier check the stairs keys use below, since
        // '?' is Shift+/ on a US layout.
        bool pressed_question =
            event.key.key == SDLK_QUESTION || (event.key.key == SDLK_SLASH && (event.key.mod & SDL_KMOD_SHIFT));
        if (event.key.key == SDLK_ESCAPE || pressed_question) gs.mode = Mode::Playing;
        continue;
      }

      if (gs.mode == Mode::LevelUp) {
        // No menu for this on purpose: just force S/D/I directly, one point at a time.
        // Requires actual Shift+S/D/I (not the bare lowercase letter) since 'd' and 'i'
        // already mean something in normal play — a permanent stat point shouldn't be
        // one stray unshifted keypress away from being spent on the wrong thing.
        bool shift_held = (event.key.mod & SDL_KMOD_SHIFT) != 0;
        if (event.key.key == SDLK_ESCAPE) {
          gs.running = false;
        // Each of these raises the attribute and then applies that point's knock-on
        // ceiling as a delta, rather than recomputing the ceiling from the attribute —
        // the same rule apply_potion() follows, so spending a level-up point while a
        // stat potion is running doesn't quietly cancel the potion. Current HP/mana rise
        // with the ceiling here (unlike a temporary buff, which only lifts the ceiling).
        } else if (shift_held && event.key.key == SDLK_S) {
          gs.player.strength += 1;
          gs.player.max_hp += kHpPerStrength;
          gs.player.hp += kHpPerStrength;
          add_message(gs, "Strength increased to " + std::to_string(gs.player.strength) + "!");
          gs.pending_attribute_points -= 1;
        } else if (shift_held && event.key.key == SDLK_D) {
          // Dexterity is now worth accuracy on every attack as well as evasion — see
          // the combat-formula block at the top of this file.
          gs.player.dexterity += 1;
          gs.player.evasion += kDodgePerDexPoint;
          add_message(gs, "Dexterity increased to " + std::to_string(gs.player.dexterity) + "!");
          gs.pending_attribute_points -= 1;
        } else if (shift_held && event.key.key == SDLK_I) {
          auto known_before = known_spell_indices(gs.player.intelligence, gs.player.chosen_school);
          int mana_delta =
              max_mana_for_intelligence(gs.player.intelligence + 1) - max_mana_for_intelligence(gs.player.intelligence);
          gs.player.intelligence += 1;
          auto known_after = known_spell_indices(gs.player.intelligence, gs.player.chosen_school);
          gs.player.max_mana += mana_delta;
          gs.player.mana += mana_delta;
          add_message(gs, "Intelligence increased to " + std::to_string(gs.player.intelligence) + "!");
          for (int spell_idx : known_after) {
            bool already_known = std::find(known_before.begin(), known_before.end(), spell_idx) != known_before.end();
            if (!already_known) add_message(gs, "You can now cast " + kSpellTable[static_cast<size_t>(spell_idx)].name + "!");
          }
          gs.pending_attribute_points -= 1;
          // The first time Intelligence reaches 4, interrupt with the forced
          // Caster/Summoner pick (Mode::SchoolChoice) instead of falling straight back
          // into Mode::Playing/another LevelUp prompt — known_spell_indices() above
          // deliberately can't have surfaced any class-gated spell yet, since
          // chosen_school is still None at this point (only the shared Magic Dart-style
          // spells show up from this diff; the school's own entry spell is announced
          // from Mode::SchoolChoice's handler once a path is actually picked).
          if (gs.player.chosen_school == SpellSchool::None && gs.player.intelligence >= 4) {
            gs.mode = Mode::SchoolChoice;
          }
        }
        // Guarded so the same keypress that just triggered Mode::SchoolChoice above
        // (the common case — a level-up grants exactly one point, so
        // pending_attribute_points usually also hits 0 right when INT crosses 4)
        // doesn't immediately stomp it back to Mode::Playing before it's ever seen.
        if (gs.mode != Mode::SchoolChoice && gs.pending_attribute_points <= 0) gs.mode = Mode::Playing;
        continue;
      }

      if (gs.mode == Mode::SchoolChoice) {
        // Mandatory, same as LevelUp's own Esc — quitting rather than silently leaving
        // chosen_school stuck at None forever, which would permanently lock out every
        // school's spells.
        bool shift_held = (event.key.mod & SDL_KMOD_SHIFT) != 0;
        if (event.key.key == SDLK_ESCAPE) {
          gs.running = false;
        } else if (shift_held && (event.key.key == SDLK_C || event.key.key == SDLK_U || event.key.key == SDLK_M)) {
          auto known_before = known_spell_indices(gs.player.intelligence, gs.player.chosen_school);
          if (event.key.key == SDLK_C) {
            gs.player.chosen_school = SpellSchool::Caster;
          } else if (event.key.key == SDLK_U) {
            gs.player.chosen_school = SpellSchool::Summoner;
          } else {
            gs.player.chosen_school = SpellSchool::CombatMage;
          }
          auto known_after = known_spell_indices(gs.player.intelligence, gs.player.chosen_school);
          if (gs.player.chosen_school == SpellSchool::Caster) {
            add_message(gs, "You have specialized in Caster magic!");
          } else if (gs.player.chosen_school == SpellSchool::Summoner) {
            add_message(gs, "You have specialized in Summoner magic!");
          } else {
            add_message(gs, "You have specialized in Combat Mage magic!");
          }
          // Same before/after diff idiom as the Shift+I handler above, reused rather
          // than duplicated, so "what's newly known" can't drift between the two call
          // sites — announces the school's own entry spell (Fireball/Raise Skeleton/
          // Battle Fury), the only thing this diff can ever surface given the choice
          // fires the instant Intelligence crosses 4.
          for (int spell_idx : known_after) {
            bool already_known = std::find(known_before.begin(), known_before.end(), spell_idx) != known_before.end();
            if (!already_known) add_message(gs, "You can now cast " + kSpellTable[static_cast<size_t>(spell_idx)].name + "!");
          }
          // Resume any level-up points still queued (e.g. mid-drain from --level=N)
          // instead of always dropping straight back to Playing.
          gs.mode = gs.pending_attribute_points > 0 ? Mode::LevelUp : Mode::Playing;
        }
        continue;
      }

      if (gs.mode == Mode::WeaponMenu) {
        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          // Slot 'a' is always fists; carried weapons fill 'b' onward.
          Weapon chosen;
          bool valid = false;
          if (idx == 0) {
            chosen = kFists;
            valid = true;
          } else if (idx - 1 < gs.player.weapons.size()) {
            chosen = gs.player.weapons[idx - 1];
            gs.player.weapons.erase(gs.player.weapons.begin() + static_cast<long>(idx - 1));
            valid = true;
          }
          if (valid) {
            // Swap the old weapon back into the pack, unless it's an intrinsic one
            // like bare fists, which isn't a real item.
            if (!gs.player.weapon.is_intrinsic) gs.player.weapons.push_back(gs.player.weapon);
            gs.player.weapon = chosen;
            add_message(gs, "You equip the " + chosen.name + ".");
            gs.mode = Mode::Playing;
            end_turn();  // fiddling with gear takes time; adjacent monsters get a free hit
          }
        }
        continue;
      }

      if (gs.mode == Mode::ArmorMenu) {
        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          // Slot 'a' is always "Nothing"; carried armor fills 'b' onward.
          Armor chosen;
          bool valid = false;
          if (idx == 0) {
            chosen = kNoArmor;
            valid = true;
          } else if (idx - 1 < gs.player.armors.size()) {
            chosen = gs.player.armors[idx - 1];
            gs.player.armors.erase(gs.player.armors.begin() + static_cast<long>(idx - 1));
            valid = true;
          }
          if (valid) {
            if (!gs.player.armor.is_intrinsic) gs.player.armors.push_back(gs.player.armor);
            gs.player.armor = chosen;
            add_message(gs, "You equip the " + chosen.name + ".");
            gs.mode = Mode::Playing;
            end_turn();  // fiddling with gear takes time; adjacent monsters get a free hit
          }
        }
        continue;
      }

      if (gs.mode == Mode::PotionMenu) {
        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          if (idx < gs.player.potions.size()) {
            // Same call an Orc Archer makes when it decides to quaff its own Heal
            // Potion — see apply_potion(), where every potion effect is defined once.
            Potion chosen = gs.player.potions[idx];
            gs.player.potions.erase(gs.player.potions.begin() + static_cast<long>(idx));
            apply_potion(gs, gs.player, chosen);
            gs.mode = Mode::Playing;
            end_turn();  // drinking takes a moment; adjacent monsters get a free hit
          }
        }
        continue;
      }

      if (gs.mode == Mode::SpellMenu) {
        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          auto known = known_spell_indices(gs.player.intelligence, gs.player.chosen_school);
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          if (idx < known.size()) {
            int spell_idx = known[idx];
            const Spell& spell = kSpellTable[static_cast<size_t>(spell_idx)];
            if (spell.is_toggle) {
              if (gs.active_toggle_spell == spell_idx) {
                // Turning off is always free — no mana cost, but still takes the turn,
                // same as every other spell-menu action.
                gs.active_toggle_spell = -1;
                add_message(gs, "Your " + spell.name + " dissipates.");
                gs.mode = Mode::Playing;
                end_turn();
              } else if (gs.player.mana < spell.mana_cost) {
                add_message(gs, "Not enough mana to cast " + spell.name + ".");
                gs.mode = Mode::Playing;  // free cancel, no turn spent
              } else {
                gs.player.mana -= spell.mana_cost;
                add_message(gs, "You summon a " + spell.name + " around yourself!");
                gs.mode = Mode::Playing;
                end_turn();  // this turn only pays the flat activation cost above
                gs.active_toggle_spell = spell_idx;  // set after end_turn(), so the
                                                   // per-turn tick starts next turn
              }
            } else if (spell.is_summon) {
              int spawn_x, spawn_y;
              if (count_minions(level.monsters) >= kMaxMinions) {
                add_message(gs, "You can't command any more minions right now.");
                gs.mode = Mode::Playing;  // free cancel, no turn spent
              } else if (gs.player.mana < spell.mana_cost) {
                add_message(gs, "Not enough mana to cast " + spell.name + ".");
                gs.mode = Mode::Playing;  // free cancel, no turn spent
              } else if (!free_adjacent_tile(level.map, level.monsters, gs.player.x, gs.player.y, spawn_x, spawn_y)) {
                add_message(gs, "There's no room to summon here!");
                gs.mode = Mode::Playing;  // free cancel, no turn spent
              } else {
                gs.player.mana -= spell.mana_cost;
                const MinionTemplate& tmpl = kMinionTable[static_cast<size_t>(spell.summon_template_index)];
                Actor minion = spawn_minion(tmpl, spawn_x, spawn_y);
                // Defaults to Aggressive — a fresh recruit engages anything hostile it
                // can see rather than passively waiting for something to wander into
                // its own reach — but joins the pack's current stance instead if an
                // existing minion is already off attacking something specific, so the
                // new recruit piles onto the same fight rather than going its own way
                // (orders are pack-wide in Phase 1, see the `m` menu).
                minion.order = MinionOrder::Aggressive;
                for (const auto& existing : level.monsters) {
                  if (existing.allegiance == Allegiance::Player && existing.is_alive() &&
                      existing.order == MinionOrder::AttackTarget) {
                    minion.order = MinionOrder::AttackTarget;
                    minion.attack_target_id = existing.attack_target_id;
                    break;
                  }
                }
                level.monsters.push_back(minion);
                add_message(gs, "You raise a " + tmpl.name + " to fight for you!");
                gs.mode = Mode::Playing;
                end_turn();
              }
            } else if (spell.is_melee_buff) {
              if (gs.player.mana < spell.mana_cost) {
                add_message(gs, "Not enough mana to cast " + spell.name + ".");
                gs.mode = Mode::Playing;  // free cancel, no turn spent
              } else {
                gs.player.mana -= spell.mana_cost;
                // Refresh-not-stack, same idiom apply_potion() uses for STR/DEX/INT.
                if (gs.player.temp_melee_damage_turns <= 0) gs.player.temp_melee_damage_bonus = spell.buff_amount;
                gs.player.temp_melee_damage_turns = spell.buff_turns;
                add_message(gs, "Your strikes grow fiercer! Melee damage +" + std::to_string(spell.buff_amount) +
                            " for " + std::to_string(spell.buff_turns) + " turns.");
                gs.mode = Mode::Playing;
                end_turn();
              }
            } else if (spell.is_armor_buff) {
              if (gs.player.mana < spell.mana_cost) {
                add_message(gs, "Not enough mana to cast " + spell.name + ".");
                gs.mode = Mode::Playing;  // free cancel, no turn spent
              } else {
                gs.player.mana -= spell.mana_cost;
                if (gs.player.temp_armor_turns <= 0) gs.player.temp_armor_bonus = spell.buff_amount;
                gs.player.temp_armor_turns = spell.buff_turns;
                add_message(gs, "Your skin hardens! Armor +" + std::to_string(spell.buff_amount) + " for " +
                            std::to_string(spell.buff_turns) + " turns.");
                gs.mode = Mode::Playing;
                end_turn();
              }
            } else if (spell.is_haste_buff) {
              if (gs.player.mana < spell.mana_cost) {
                add_message(gs, "Not enough mana to cast " + spell.name + ".");
                gs.mode = Mode::Playing;  // free cancel, no turn spent
              } else {
                gs.player.mana -= spell.mana_cost;
                add_message(gs, "You blur into motion! +" + std::to_string(spell.buff_amount) +
                            " action per turn for " + std::to_string(spell.buff_turns) + " turns.");
                gs.mode = Mode::Playing;
                // The buff is applied *after* end_turn(), so casting Haste costs a whole
                // turn like any other spell instead of immediately refunding itself as a
                // free action. Exactly the reason active_toggle_spell is set after
                // end_turn() when a toggle spell is switched on — otherwise the cheapest
                // way to use the spell would be to keep re-casting it.
                end_turn();
                if (gs.player.temp_extra_actions_turns <= 0) gs.player.temp_extra_actions_bonus = spell.buff_amount;
                gs.player.temp_extra_actions_turns = spell.buff_turns;  // refresh-not-stack, as above
              }
            } else if (spell.is_swap) {
              gs.casting_spell_index = spell_idx;
              // Auto-aim at the closest minion in range (no FOV requirement — see
              // closest_own_minion()), else fall back to the player's own tile; Enter
              // will just reject the cast with a message if nothing's actually there.
              int auto_id = closest_own_minion(level.monsters, gs.player, spell.range);
              int auto_idx = actor_index_by_id(level.monsters, auto_id);
              if (auto_idx >= 0) {
                gs.target_x = level.monsters[static_cast<size_t>(auto_idx)].x;
                gs.target_y = level.monsters[static_cast<size_t>(auto_idx)].y;
              } else {
                gs.target_x = gs.player.x;
                gs.target_y = gs.player.y;
              }
              gs.mode = Mode::Targeting;
            } else {
              gs.casting_spell_index = spell_idx;
              // Auto-aim at the most recently targeted hostile if it still qualifies,
              // else the closest qualifying one, else fall back to the player's own
              // tile (the old default) — see auto_target_hostile().
              int auto_id = auto_target_hostile(level.monsters, gs.player, level.map, gs.last_target_id, spell.range);
              int auto_idx = actor_index_by_id(level.monsters, auto_id);
              if (auto_idx >= 0) {
                gs.target_x = level.monsters[static_cast<size_t>(auto_idx)].x;
                gs.target_y = level.monsters[static_cast<size_t>(auto_idx)].y;
              } else {
                gs.target_x = gs.player.x;
                gs.target_y = gs.player.y;
              }
              gs.mode = Mode::Targeting;
            }
          }
        }
        continue;
      }

      if (gs.mode == Mode::MinionRoster) {
        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
          continue;
        }
        if (event.key.key == SDLK_A && (event.key.mod & SDL_KMOD_SHIFT) != 0) {
          // Shift+A always means "All", regardless of which letter it actually landed
          // on this frame (that shifts with the pack's current size) — a fast path so
          // you don't have to read the list to find the right letter every time.
          gs.commanding_all_minions = true;
          gs.target_x = gs.player.x;
          gs.target_y = gs.player.y;
          gs.mode = Mode::MinionFocus;
          continue;
        }
        if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          // Same ordering as the roster's render: a letter per living minion, in
          // level.monsters order ("All" is the fixed Shift+A hotkey above, not a
          // letter in this range — see the check above this one).
          std::vector<int> minion_ids;
          for (const auto& m : level.monsters) {
            if (m.allegiance == Allegiance::Player && m.is_alive()) minion_ids.push_back(m.id);
          }
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          if (idx < minion_ids.size()) {
            gs.focused_minion_id = minion_ids[idx];
            gs.commanding_all_minions = false;
            int fi = actor_index_by_id(level.monsters, gs.focused_minion_id);
            gs.target_x = level.monsters[static_cast<size_t>(fi)].x;
            gs.target_y = level.monsters[static_cast<size_t>(fi)].y;
            gs.mode = Mode::MinionFocus;
          }
        }
        continue;
      }

      if (gs.mode == Mode::MinionFocus) {
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
          continue;
        }
        if (event.key.key == SDLK_O || (event.key.key == SDLK_P && !shift_held)) {
          // Tab straight to the next/previous minion without dropping back to normal
          // play in between — plan one, tab, plan the next, same as 'o'/'p' do from
          // Mode::Playing (see cycle_minion_focus above), just without leaving this mode.
          if (!cycle_minion_focus(event.key.key == SDLK_O ? 1 : -1)) {
            add_message(gs, "You have no minions to command.");
            gs.mode = Mode::Playing;
          }
          continue;
        }
        if (event.key.key == SDLK_F) {
          int ordered = for_each_commanded_minion([](Actor& m) { m.order = MinionOrder::Follow; });
          add_message(gs, ordered == 1 ? "Your minion returns to your side." : "Your minions return to your side.");
          gs.commanding_all_minions = false;
          gs.mode = Mode::Playing;
          continue;
        }
        if (event.key.key == SDLK_G) {
          // Aggressive: same as Follow, but engages anything hostile it can see
          // instead of waiting for something to wander into its own reach — see the
          // MinionOrder doc comment in entity.hpp.
          int ordered = for_each_commanded_minion([](Actor& m) { m.order = MinionOrder::Aggressive; });
          add_message(gs, ordered == 1 ? "Your minion goes on the offensive." : "Your minions go on the offensive.");
          gs.commanding_all_minions = false;
          gs.mode = Mode::Playing;
          continue;
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
            continue;
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
            continue;
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
            continue;
          }
          add_message(gs, "You can't send them there.");
          continue;  // stay in this mode, no turn spent — try again
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
        continue;
      }

      if (gs.mode == Mode::Targeting) {
        const Spell& spell = kSpellTable[static_cast<size_t>(gs.casting_spell_index)];

        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
          continue;
        }
        if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
          if (gs.player.mana < spell.mana_cost) {
            add_message(gs, "Not enough mana to cast " + spell.name + ".");
            gs.mode = Mode::Playing;  // free cancel, same as Esc — no turn spent
            continue;
          }

          if (spell.is_swap) {
            int minion_index = own_minion_at(level.monsters, gs.target_x, gs.target_y);
            if (minion_index < 0) {
              add_message(gs, "There's no minion there to swap places with.");
              gs.mode = Mode::Playing;  // free cancel, same as Esc — no turn spent
              continue;
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
            end_turn();
            continue;
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
          proj.path = trace_path(gs.player.x, gs.player.y, gs.target_x, gs.target_y);
          proj.speed = spell.speed;
          proj.dice_count = spell.dice_count;
          proj.dice_sides = spell.dice_sides;
          proj.hit_dice_count = spell.hit_dice_count;
          proj.hit_dice_sides = spell.hit_dice_sides;
          proj.aoe_radius = spell.aoe_radius;
          proj.pierces = spell.pierces;
          proj.prev_x = gs.player.x;  // seeds the "last open tile" for an immediate wall hit
          proj.prev_y = gs.player.y;
          // Locked in now, not re-read when it lands. Temporary INT (from a Potion of
          // Intelligence) boosts this the same as permanent INT would — only spell
          // *unlocking* (known_spell_indices, above) ignores the temporary bonus.
          proj.bonus = (gs.player.intelligence + gs.player.temp_int_bonus) / 3;
          // A spell's accuracy is built the same way a weapon swing's is: the caster's
          // Dexterity term plus the spell's own hit-dice, rolled on impact. Locking the
          // Dexterity half in here (rather than reading it when the projectile lands
          // several turns later) is what makes a slow Fireball as accurate as the moment
          // it was thrown.
          proj.accuracy_bonus = (gs.player.dexterity + gs.player.temp_dex_bonus) * kAccuracyPerDexPoint;
          proj.name = spell.name;
          proj.glyph = spell.glyph;
          proj.color = spell.color;
          // Matches Projectile's defaults, but set explicitly: now that a monster can
          // fire one too, "who owns this" shouldn't be something a reader has to infer
          // from a default.
          proj.owner_allegiance = Allegiance::Player;
          proj.owner_is_player = true;
          level.projectiles.push_back(proj);
          gs.player.mana -= spell.mana_cost;

          add_message(gs, "You cast " + spell.name + ".");
          gs.mode = Mode::Playing;
          end_turn();  // advance_projectiles() may resolve this immediately for fast spells
          continue;
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
          int rdx = nx - gs.player.x;
          int rdy = ny - gs.player.y;
          bool in_range = rdx * rdx + rdy * rdy <= spell.range * spell.range;
          if (level.map.in_bounds(nx, ny) && in_range) {
            gs.target_x = nx;
            gs.target_y = ny;
          }
        }
        continue;
      }

      if (gs.mode == Mode::RangedAttack) {
        const Weapon& weapon = gs.player.weapon;

        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
          continue;
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
          end_turn();
          continue;
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
        continue;
      }

      if (gs.mode == Mode::Look) {
        if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_X) {
          gs.mode = Mode::Playing;
          continue;
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
        continue;
      }

      if (gs.mode == Mode::Drop) {
        if (event.key.key == SDLK_ESCAPE) {
          gs.mode = Mode::Playing;
        } else if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z) {
          auto slots = drop_slots(gs.player);
          size_t idx = static_cast<size_t>(event.key.key - SDLK_A);
          if (idx < slots.size()) {
            const ItemSlot& slot = slots[idx];
            std::string dropped_name;
            if (slot.kind == ItemKind::Weapon) {
              Weapon dropped;
              if (slot.index == -1) {
                dropped = gs.player.weapon;
                gs.player.weapon = kFists;
              } else {
                size_t inv_idx = static_cast<size_t>(slot.index);
                dropped = gs.player.weapons[inv_idx];
                gs.player.weapons.erase(gs.player.weapons.begin() + static_cast<long>(inv_idx));
              }
              level.items.push_back(GroundItem{gs.player.x, gs.player.y, dropped});
              dropped_name = dropped.name;
            } else if (slot.kind == ItemKind::Armor) {
              Armor dropped;
              if (slot.index == -1) {
                dropped = gs.player.armor;
                gs.player.armor = kNoArmor;
              } else {
                size_t inv_idx = static_cast<size_t>(slot.index);
                dropped = gs.player.armors[inv_idx];
                gs.player.armors.erase(gs.player.armors.begin() + static_cast<long>(inv_idx));
              }
              level.armor_items.push_back(GroundArmor{gs.player.x, gs.player.y, dropped});
              dropped_name = dropped.name;
            } else {
              size_t inv_idx = static_cast<size_t>(slot.index);
              Potion dropped = gs.player.potions[inv_idx];
              gs.player.potions.erase(gs.player.potions.begin() + static_cast<long>(inv_idx));
              level.potions.push_back(GroundPotion{gs.player.x, gs.player.y, dropped});
              dropped_name = dropped.name;
            }
            add_message(gs, "You drop the " + dropped_name + ".");
            gs.mode = Mode::Playing;
            end_turn();
          }
        }
        continue;
      }

      // Mode::Playing
      if (event.key.key == SDLK_ESCAPE) {
        gs.running = false;
        continue;
      }

      if (event.key.key == SDLK_W) {
        gs.mode = Mode::WeaponMenu;
        continue;
      }
      if (event.key.key == SDLK_A) {
        gs.mode = Mode::ArmorMenu;
        continue;
      }
      if (event.key.key == SDLK_D) {
        gs.mode = Mode::Drop;
        continue;
      }
      if (event.key.key == SDLK_Q) {
        gs.mode = Mode::PotionMenu;
        continue;
      }
      if (event.key.key == SDLK_G) {
        // Picks up *everything* on the player's current tile (no auto-pickup on step),
        // each as its own message rather than one combined line. This used to take only
        // the first item of each kind, which was fine when the floor was the only source
        // of loot — but a dead monster now drops its whole pack on one tile (an Orc
        // Archer leaves both a Short Bow and a Short Sword), and leaving half of it
        // behind with no indication anything remained was just confusing. One turn
        // total, however much is here.
        //
        // Each loop walks backwards so erasing the current element can't shift an
        // unvisited one out from under the index.
        bool picked_up_anything = false;
        for (int i = static_cast<int>(level.items.size()) - 1; i >= 0; --i) {
          const GroundItem& ground = level.items[static_cast<size_t>(i)];
          if (ground.x != gs.player.x || ground.y != gs.player.y) continue;
          add_message(gs, "You pick up a " + ground.weapon.name + ". Press 'w' to equip.");
          gs.player.weapons.push_back(ground.weapon);
          level.items.erase(level.items.begin() + i);
          picked_up_anything = true;
        }
        for (int i = static_cast<int>(level.armor_items.size()) - 1; i >= 0; --i) {
          const GroundArmor& ground = level.armor_items[static_cast<size_t>(i)];
          if (ground.x != gs.player.x || ground.y != gs.player.y) continue;
          add_message(gs, "You pick up a " + ground.armor.name + ". Press 'a' to equip.");
          gs.player.armors.push_back(ground.armor);
          level.armor_items.erase(level.armor_items.begin() + i);
          picked_up_anything = true;
        }
        for (int i = static_cast<int>(level.potions.size()) - 1; i >= 0; --i) {
          const GroundPotion& ground = level.potions[static_cast<size_t>(i)];
          if (ground.x != gs.player.x || ground.y != gs.player.y) continue;
          add_message(gs, "You pick up a " + ground.potion.name + ". Press 'q' to drink.");
          gs.player.potions.push_back(ground.potion);
          level.potions.erase(level.potions.begin() + i);
          picked_up_anything = true;
        }
        if (picked_up_anything) {
          end_turn();
        } else {
          add_message(gs, "There's nothing here to pick up.");
        }
        continue;
      }
      if (event.key.key == SDLK_Z) {
        gs.mode = Mode::SpellMenu;
        continue;
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
        continue;
      }
      if (event.key.key == SDLK_M) {
        if (count_minions(level.monsters) == 0) {
          add_message(gs, "You have no minions to command.");
        } else {
          gs.mode = Mode::MinionRoster;
        }
        continue;
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
          continue;
        }
        if (!cycle_minion_focus(event.key.key == SDLK_O ? 1 : -1)) {
          add_message(gs, "You have no minions to command.");
        } else {
          gs.mode = Mode::MinionFocus;
        }
        continue;
      }
      if (event.key.key == SDLK_RIGHTBRACKET) {
        gs.mode = Mode::MessageLog;
        gs.log_scroll = 0;  // always open showing the most recent messages
        continue;
      }
      if (event.key.key == SDLK_X) {
        // Starts the look cursor on the player's own tile, same as Targeting/
        // MinionFocus do — free to open/close, no turn spent either way.
        gs.mode = Mode::Look;
        gs.target_x = gs.player.x;
        gs.target_y = gs.player.y;
        continue;
      }
      // '?' is Shift+/ on a US layout, so check both the dedicated keycode and the
      // unshifted one with the modifier set — same pattern the stairs keys use below.
      if (event.key.key == SDLK_QUESTION ||
          (event.key.key == SDLK_SLASH && (event.key.mod & SDL_KMOD_SHIFT))) {
        gs.mode = Mode::Help;
        continue;
      }
      // SDL reports keycodes for the *unshifted* key on a US layout, so Shift+Period
      // arrives as SDLK_PERIOD with the shift modifier set, not SDLK_GREATER — check
      // both forms so '>' / '<' work regardless of how the layout reports it.
      bool pressed_stairs_down =
          event.key.key == SDLK_GREATER || (event.key.key == SDLK_PERIOD && (event.key.mod & SDL_KMOD_SHIFT));
      bool pressed_stairs_up =
          event.key.key == SDLK_LESS || (event.key.key == SDLK_COMMA && (event.key.mod & SDL_KMOD_SHIFT));

      // Taking stairs costs a turn like any other action. end_turn() runs *before* the
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
        if (gs.player.x == level.stairs_down_x && gs.player.y == level.stairs_down_y) {
          end_turn();
          if (gs.mode != Mode::Dead) descend(gs);
        } else {
          add_message(gs, "There are no stairs down here.");
        }
        continue;
      }
      if (pressed_stairs_up) {
        if (level.has_stairs_up && gs.player.x == level.entry_x && gs.player.y == level.entry_y) {
          end_turn();
          if (gs.mode != Mode::Dead) ascend(gs);
        } else {
          add_message(gs, "There are no stairs up here.");
        }
        continue;
      }
      // Plain '.' (no shift, which is claimed above for '>') passes the turn without
      // moving or attacking — handy for watching what monsters do on their own.
      if (event.key.key == SDLK_PERIOD && !(event.key.mod & SDL_KMOD_SHIFT)) {
        add_message(gs, "You wait.");
        end_turn();
        continue;
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
      if (dx == 0 && dy == 0) continue;

      int new_x = gs.player.x + dx;
      int new_y = gs.player.y + dy;

      int target_index = -1;
      for (size_t i = 0; i < level.monsters.size(); ++i) {
        if (level.monsters[i].x == new_x && level.monsters[i].y == new_y) {
          target_index = static_cast<int>(i);
          break;
        }
      }

      if (target_index >= 0 && level.monsters[static_cast<size_t>(target_index)].allegiance == Allegiance::Player) {
        // Bump into your own minion: swap places instead of attacking it — you're
        // squeezing past an ally, not fighting one.
        Actor& minion = level.monsters[static_cast<size_t>(target_index)];
        std::swap(gs.player.x, minion.x);
        std::swap(gs.player.y, minion.y);
        level.map.update_fov(gs.player.x, gs.player.y, FOV_RADIUS);
        end_turn();
      } else if (target_index >= 0) {
        // Bump attack: walking into a monster attacks it instead of moving. Exactly the
        // same call a monster makes when it swings at you — the dodge roll, the armor
        // reduction, the XP and the loot drop all live in resolve_attack(), not here.
        resolve_attack(gs, gs.player, level.monsters[static_cast<size_t>(target_index)], gs.player.weapon);
        end_turn();  // any monster(s) still adjacent (including the one just hit) get to act
      } else if (level.map.is_walkable(new_x, new_y)) {
        gs.player.x = new_x;
        gs.player.y = new_y;
        level.map.update_fov(gs.player.x, gs.player.y, FOV_RADIUS);
        end_turn();
      }
    }
  }

  return 0;
}

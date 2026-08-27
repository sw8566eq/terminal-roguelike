#pragma once

// Private contract between render.cpp and its per-mode-group satellite files
// (render_item_menus.cpp, render_spell_minion_menus.cpp, render_screens.cpp) — NOT part
// of the public rendering API (that's render.hpp; nothing outside this small file group
// includes this header). Each mode still gets exactly one render function, reached
// through exactly one switch in render_frame() (render.cpp) — splitting the ~20 of them
// across files by mode-group is purely a file-size cut (see CLAUDE.md's "Known
// limitations" note this addresses), not a change to that architecture. A function
// declared here has external linkage instead of living in render.cpp's own anonymous
// namespace purely so render_frame() can reach it from another translation unit; nothing
// else outside these four files should call one directly.

#include <libtcod.hpp>

#include "game.hpp"

// --- render_item_menus.cpp: WeaponMenu, ArmorMenu, PotionMenu, Drop, Pickup ---
void render_weapon_menu(GameState& gs, tcod::Console& console);
void render_armor_menu(GameState& gs, tcod::Console& console);
void render_potion_menu(GameState& gs, tcod::Console& console);
void render_drop_screen(GameState& gs, tcod::Console& console);
void render_pickup_screen(GameState& gs, tcod::Console& console);

// --- render_spell_minion_menus.cpp: SpellMenu, SchoolChoice, MinionRoster,
//     MinionAbilityMenu ---
void render_spell_menu(GameState& gs, tcod::Console& console);
void render_school_choice(tcod::Console& console);
void render_minion_roster(GameState& gs, tcod::Console& console);
void render_minion_ability_menu(GameState& gs, tcod::Console& console);

// --- render_screens.cpp: StartMenu, SetSeed, RunHistory, Dead, Win, MessageLog, Help ---
void render_start_menu(GameState& gs, tcod::Console& console);
void render_set_seed_screen(GameState& gs, tcod::Console& console);
void render_run_history_screen(GameState& gs, tcod::Console& console);
void render_death_screen(GameState& gs, tcod::Console& console);
void render_win_screen(GameState& gs, tcod::Console& console);
void render_message_log(GameState& gs, tcod::Console& console);
void render_help(tcod::Console& console);

#pragma once

// The turn boundary: everything the world does between one player action and the next.

#include "game.hpp"

// Debug flag --pause-ai: skips both AI loops in end_turn() (hostile monsters, then
// minions) entirely, so nothing but the player moves — for freezing a scene to compose
// a screenshot without a monster wandering off or a fight breaking out mid-shot.
// Everything else in a turn still runs normally (upkeep, regen, buff timers, projectiles
// already in flight), so it's a narrower pause than it might sound: monsters just never
// decide to act. A plain global rather than a GameState field, same shape as
// g_debug_fast_monsters (level.hpp) — a debug knob set once at startup, not session
// state anything in-game reads or toggles.
extern bool g_debug_pause_ai;

// The single hook every turn-consuming player action funnels through — moving,
// attacking, waiting, equipping, dropping, drinking, casting, toggling a spell, giving a
// minion an order. There are roughly twenty call sites and they all just call this.
//
// A world turn runs, in order:
//
//   1. the free-action guard   — if the player has extra actions left (Haste, or
//                                Actor::extra_actions), spend one and return without
//                                advancing the world at all. Only hit-scan projectiles
//                                resolve on a free action; no upkeep, no aura, no AI.
//   2. upkeep                  — for the player, then every Actor on the floor: passive
//                                HP regen, passive mana regen, temporary buffs counting
//                                down. Buff timers therefore count *world* turns, so
//                                Haste's 8 turns is 16 actions.
//   3. projectiles             — everything in flight advances by its speed.
//   4. minion durations        — a timed minion expires.
//   5. the active toggle spell — Sandstorm's per-turn drain and damage.
//   6. hostile AI              — every living hostile acts, once per action it has.
//   7. projectiles again       — hit-scan only, so a monster's spell resolves in the turn
//                                it was cast rather than flying at where the player used
//                                to be. Load-bearing; see the comment at that call.
//   8. minion AI               — every living minion acts.
//   9. the deferred sweep      — everything that died this turn is finally erased.
//
// Steps 2-9 are skipped entirely on a free action. New per-turn systems (status effects,
// more monster behaviors) belong somewhere in that list.
void end_turn(GameState& gs);

#pragma once

// Keyboard handling: one function per Mode, chosen by handle_event().
//
// Input is the only layer that writes GameState in response to the player. A handler
// that changes the world calls end_turn() (turn.hpp) to let the world respond; one that
// only opens a menu or moves a cursor does not, which is exactly what makes those
// actions free.

#include <SDL3/SDL.h>

#include "game.hpp"

// Applies one key-down event to the game, dispatching on the current mode. The caller
// filters out quit and non-key events before calling this.
void handle_event(GameState& gs, const SDL_Event& event);

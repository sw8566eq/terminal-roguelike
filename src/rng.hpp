#pragma once

// Shared random-number utility used by map generation, combat, and item/monster
// placement, so everything draws from one engine instead of each file seeding
// its own.
//
// Returns an integer in [lo, hi], inclusive.
int random_int(int lo, int hi);

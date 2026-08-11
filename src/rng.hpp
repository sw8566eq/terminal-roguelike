#pragma once

// Shared random-number utility used by map generation, combat, and item/monster
// placement, so everything draws from one engine instead of each file seeding
// its own.
//
// Returns an integer in [lo, hi], inclusive.
int random_int(int lo, int hi);

// Reseeds the shared engine, making every subsequent draw reproducible. Normally the
// engine seeds itself from std::random_device, so a run is different every time; the
// --seed=N debug flag calls this before any level is generated to pin a run down.
// Reproducible floors are what make --dump-loot diffable and a bug report replayable.
void seed_rng(unsigned int seed);

// Rolls `count` dice of `sides` each and sums them (e.g. roll_dice(2, 6) is 2d6).
int roll_dice(int count, int sides);

#include "rng.hpp"

#include <random>

namespace {

std::mt19937& rng_engine() {
  static std::mt19937 engine{std::random_device{}()};
  return engine;
}

}  // namespace

void seed_rng(unsigned int seed) { rng_engine().seed(seed); }

int random_int(int lo, int hi) {
  std::uniform_int_distribution<int> dist(lo, hi);
  return dist(rng_engine());
}

int roll_dice(int count, int sides) {
  int total = 0;
  for (int i = 0; i < count; ++i) total += random_int(1, sides);
  return total;
}

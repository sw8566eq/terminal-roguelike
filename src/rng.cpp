#include "rng.hpp"

#include <random>

namespace {

std::mt19937& rng_engine() {
  static std::mt19937 engine{std::random_device{}()};
  return engine;
}

}  // namespace

int random_int(int lo, int hi) {
  std::uniform_int_distribution<int> dist(lo, hi);
  return dist(rng_engine());
}

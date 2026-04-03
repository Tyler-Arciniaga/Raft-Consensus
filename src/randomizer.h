#pragma once

#include <random>
class Randomizer {
public:
  Randomizer(std::random_device &rd);
  int GetRandomElectionTimeout();

private:
  std::mt19937 rng; // random engine
  std::uniform_int_distribution<int>
      dist; // int range to generate random numbers within (inclusive)
};

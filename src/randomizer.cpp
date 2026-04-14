#include "randomizer.h"

Randomizer::Randomizer(std::random_device &rd)
    : rng(rd()), dist(std::uniform_int_distribution<int>(300, 500)) {}

int Randomizer::GetRandomElectionTimeout() { return dist(rng); }

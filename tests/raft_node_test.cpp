#include "../src/network.h"
#include "../src/raft_node.h"
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <vector>

TEST(Smoke, Builds) { EXPECT_TRUE(true); }

class ElectionTest : public ::testing::Test {
protected:
  void SetUp() override {
    for (size_t i = 0; i < 5; i++) {
      nodes.emplace_back(std::make_unique<RaftNode>(i, rd, network));
    }

    for (auto &node : nodes) {
      node->SetPeers({0, 1, 2, 3, 4});
    }
  }

  void TearDown() override {
    for (auto &node : nodes) {
      node->StopNode();
    }
  }

  std::random_device rd;
  SimulatedNetwork network;
  std::vector<std::unique_ptr<RaftNode>> nodes;
};

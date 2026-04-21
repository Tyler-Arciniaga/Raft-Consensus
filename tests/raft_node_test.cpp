#include "../src/network.h"
#include "../src/raft_node.h"
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <thread>
#include <vector>

TEST(Smoke, Builds) { EXPECT_TRUE(true); }

class ElectionTest : public ::testing::Test {
protected:
  void SetUp() override {
    for (size_t i = 0; i < 5; i++) {
      nodes.emplace_back(std::make_unique<RaftNode>(i, rd, network));
      network.AddNode(nodes.back().get());
    }

    for (auto &node : nodes) {
      node->SetPeers({0, 1, 2, 3, 4});
    }

    for (auto &node : nodes) {
      node_threads.emplace_back(&RaftNode::StartNode, node.get());
    }
  }

  void TearDown() override {
    for (auto &node : nodes) {
      node->StopNode();
    }

    for (auto &t : node_threads) {
      t.join();
    }
  }

  std::vector<std::unique_ptr<RaftNode>> nodes;
  std::vector<std::thread> node_threads;

  std::random_device rd;
  SimulatedNetwork network;
};

TEST_F(ElectionTest, ElectsExactlyOneLeader) {
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  int leaderCounter = 0;

  for (auto &node : nodes) {
    if (node.get()->GetState() == NodeState::Leader) {
      leaderCounter++;
    }
  }

  EXPECT_EQ(leaderCounter, 1);
}

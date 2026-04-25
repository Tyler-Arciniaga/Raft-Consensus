#include "../src/network.h"
#include "../src/raft_node.h"
#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <thread>
#include <vector>

constexpr size_t N_NODES = 5;

TEST(Smoke, Builds) { EXPECT_TRUE(true); }

class ElectionTest : public ::testing::Test {
protected:
  inline const std::vector<size_t> GeneratePeers() const {
    std::vector<size_t> v;
    for (size_t i = 0; i < N_NODES; i++) {
      v.push_back(i);
    }

    return v;
  }

  void SetUp() override {
    for (size_t i = 0; i < N_NODES; i++) {
      nodes.emplace_back(std::make_unique<RaftNode>(i, rd, network));
      network.AddNode(nodes.back().get());
    }

    auto peers = GeneratePeers();
    for (auto &node : nodes) {
      node->SetPeers(peers);
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

  bool ExactlyOneLeader() {
    int leaderCounter = 0;
    for (auto &node : nodes) {
      if (node->GetState() == NodeState::Leader) {
        leaderCounter++;
      }
    }

    return leaderCounter == 1;
  }

  template <typename Condition>
  bool WaitForCondition(Condition condition, int timeoutMS) {
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMS);
    while (std::chrono::steady_clock::now() < deadline) {
      if (condition()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return false;
  }

  std::vector<std::unique_ptr<RaftNode>> nodes;
  std::vector<std::thread> node_threads;

  std::random_device rd;
  SimulatedNetwork network;
};

TEST_F(ElectionTest, ElectsExactlyOneLeader) {
  auto cond = [this] { return ExactlyOneLeader(); };

  auto res = WaitForCondition(cond, 1000);
  ASSERT_TRUE(res) << "no single leader elected after 1 sec";
}

TEST_F(ElectionTest, HandlesSingleLeaderLoss) {
  auto cond = [this] { return ExactlyOneLeader(); };

  auto res = WaitForCondition(cond, 1000);
  ASSERT_TRUE(res) << "no single leader elected after 1 sec";

  auto leader_index = SIZE_MAX;
  for (int i = 0; i < nodes.size(); i++) {
    if (nodes[i].get()->GetState() == NodeState::Leader) {
      leader_index = i;
      break;
    }
  }

  ASSERT_NE(leader_index, SIZE_MAX);

  nodes[leader_index].get()->StopNode();

  auto res2 = WaitForCondition(cond, 1000);
  ASSERT_TRUE(res2) << "no single leader elected after previous leader dies";
}

TEST_F(ElectionTest, HandlesFalseCandidateDemotion) {
  auto cond = [this] { return ExactlyOneLeader(); };

  auto res = WaitForCondition(cond, 1000);
  ASSERT_TRUE(res) << "no single leader elected after 1 sec";

  for (auto &node : nodes) {
    if (node->GetState() == NodeState::Follower) {
      node->SetState(NodeState::Candidate);
      break;
    }
  }

  auto res2 = WaitForCondition(cond, 1000);
  ASSERT_TRUE(res) << "no single leader after single false candidate";
}

TEST_F(ElectionTest, HandlesMultiFalseCandidates) {
  auto cond = [this] { return ExactlyOneLeader(); };

  auto res = WaitForCondition(cond, 1000);
  ASSERT_TRUE(res) << "no single leader elected after 1 sec";

  for (auto &node : nodes) {
    if (node->GetState() == NodeState::Follower) {
      node->SetState(NodeState::Candidate);
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  auto res2 = WaitForCondition(cond, 1000);
  ASSERT_TRUE(res2) << "no single leader after multiple false candidates";
}

TEST_F(ElectionTest, HandlesMultiCandidateElection) {
  for (auto &node : nodes) {
    if (node->GetState() == NodeState::Follower) {
      node->SetState(NodeState::Candidate);
    }
  }

  auto cond = [this] { return ExactlyOneLeader(); };

  auto res = WaitForCondition(cond, 1000);
  ASSERT_TRUE(res) << "multi candidate election does not resolve to single "
                      "leader after 1 sec";
}

TEST_F(ElectionTest, NodesResolveToSameTerm) {
  auto single_leader_cond = [this] { return ExactlyOneLeader(); };

  auto single_term_cond = [this] {
    uint64_t leader_term = UINT64_MAX;
    for (auto &node : nodes) {
      if (node->GetState() == NodeState::Leader) {
        leader_term = node->GetTerm();
        break;
      }
    }

    for (auto &node : nodes) {
      if (node->GetTerm() != leader_term) {
        return false;
      }
    }

    return true;
  };

  auto res = WaitForCondition(single_leader_cond, 1000);
  ASSERT_TRUE(res) << "no single leader after election resolves";

  auto res2 = WaitForCondition(single_term_cond, 2000);
  ASSERT_TRUE(res2) << "all nodes don't eventually resolve to leader's term";
}

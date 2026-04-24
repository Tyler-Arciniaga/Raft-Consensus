#include "../src/network.h"
#include "../src/raft_node.h"
#include <gtest/gtest.h>
#include <thread>

constexpr size_t N_NODES = 5;

class LogReplicationTest : public ::testing::Test {
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

TEST_F(LogReplicationTest, ReplicatesSingleRequest) {
  auto res = WaitForCondition([this] { return ExactlyOneLeader(); }, 1000);
  ASSERT_TRUE(res) << "single leader is not elected after 1 sec";

  RaftNode *leader_node;
  for (auto &node : nodes) {
    if (node->GetState() == NodeState::Leader) {
      leader_node = node.get();
      break;
    }
  }

  auto req = ServerRequest{ServerAction::Add, "x", 5};
  leader_node->SendRequest(req);

  auto expected_entry =
      LogEntry{ServerAction::Add, "x", 5, leader_node->GetTerm()};

  auto cond = [this, &expected_entry] {
    for (auto &node : nodes) {
      auto log = node->GetLog();
      if (log.size() == 0) {
        return false;
      }
      auto last_entry = log.back();
      if (last_entry != expected_entry) {
        return false;
      }
    }
    return true;
  };

  auto res2 = WaitForCondition(cond, 2000);
  ASSERT_TRUE(res2)
      << "some nodes fail to eventually reach consistent log after 2 sec";
}

// TODO test instances where node shuts down and then resumes after entry was
// committed and all other similar instances

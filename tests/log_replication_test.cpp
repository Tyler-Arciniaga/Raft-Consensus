#include "../src/network.h"
#include "../src/raft_node.h"
#include "rpc.h"
#include <chrono>
#include <gtest/gtest.h>
#include <limits>
#include <random>
#include <thread>

constexpr size_t N_NODES = 5;

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

  auto req =
      std::vector<ServerRequest>{ServerRequest{ServerAction::Add, "x", 5}};
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

TEST_F(LogReplicationTest, CommitsSingleRequestOnLeader) {
  auto res = WaitForCondition([this] { return ExactlyOneLeader(); }, 1000);
  ASSERT_TRUE(res) << "single leader is not elected after 1 sec";

  RaftNode *leader_node;
  for (auto &node : nodes) {
    if (node->GetState() == NodeState::Leader) {
      leader_node = node.get();
      break;
    }
  }

  auto req =
      std::vector<ServerRequest>{ServerRequest{ServerAction::Add, "x", 5}};
  leader_node->SendRequest(req);

  auto cond = [&] { return leader_node->GetCommitIndex() == 1; };

  auto res2 = WaitForCondition(cond, 2000);
  ASSERT_TRUE(res2) << "leader's commit index was not updated";
}

TEST_F(LogReplicationTest, CommitsSingleRequestOnFollowers) {
  auto res = WaitForCondition([this] { return ExactlyOneLeader(); }, 1000);
  ASSERT_TRUE(res) << "single leader is not elected after 1 sec";

  RaftNode *leader_node;
  for (auto &node : nodes) {
    if (node->GetState() == NodeState::Leader) {
      leader_node = node.get();
      break;
    }
  }

  auto req =
      std::vector<ServerRequest>{ServerRequest{ServerAction::Add, "x", 5}};
  leader_node->SendRequest(req);

  auto cond = [&](size_t expected_commit_index) {
    for (auto &node : nodes) {
      if (node->GetState() == NodeState::Leader) {
        continue;
      }

      if (node->GetCommitIndex() != expected_commit_index) {
        return false;
      }
    }

    return true;
  };

  auto res2 = WaitForCondition([&] { return cond(1); }, 2000);
  ASSERT_TRUE(res2)
      << "not all followers had an updated commit index after 2 sec";

  auto req2 =
      std::vector<ServerRequest>{ServerRequest{ServerAction::Remove, "y"}};
  leader_node->SendRequest(req2);

  res2 = WaitForCondition([&] { return cond(2); }, 2000);
  ASSERT_TRUE(res2)
      << "not all followers updated commit index (the second time) after 2 sec";
}

TEST_F(LogReplicationTest, NodesApplyLogToStateMachine) {
  auto res = WaitForCondition([this] { return ExactlyOneLeader(); }, 1000);
  ASSERT_TRUE(res) << "single leader is not elected after 1 sec";

  RaftNode *leader_node;
  for (auto &node : nodes) {
    if (node->GetState() == NodeState::Leader) {
      leader_node = node.get();
      break;
    }
  }

  auto req =
      std::vector<ServerRequest>{ServerRequest{ServerAction::Add, "t", 1},
                                 ServerRequest{ServerAction::Add, "y", 2},
                                 ServerRequest{ServerAction::Add, "A", 3}};

  leader_node->SendRequest(req);

  auto cond = [this] {
    for (auto &node : nodes) {
      auto val1 = node->FetchFromStateMachine("t");
      auto val2 = node->FetchFromStateMachine("y");
      auto val3 = node->FetchFromStateMachine("A");
      auto val4 = node->FetchFromStateMachine("X");
      if (val1 != 1 || val2 != 2 || val3 != 3 ||
          val4 != std::numeric_limits<int>::max()) {
        return false;
      }
    }

    return true;
  };

  res = WaitForCondition(cond, 2000);
  ASSERT_TRUE(res) << "all nodes do not properly apply log to local state "
                      "machine after 2 sec";
}

TEST(LogReplicationLogic, HandlesBehindFollower) {
  std::random_device rd;
  SimulatedNetwork network;
  RaftNode leader(0, rd, network);
  leader.SetState(NodeState::Leader);

  RaftNode follower(1, rd, network);

  network.AddNode(&leader);
  network.AddNode(&follower);

  std::vector<ServerRequest> req;
  for (auto i = 0; i < 5; i++) {
    req.push_back(ServerRequest{ServerAction::Add, "x", i});
  }

  leader.SendRequest(req);

  auto peers = std::vector<size_t>{0, 1};
  leader.SetPeers(peers);
  follower.SetPeers(peers);

  std::vector<std::thread> threads;

  threads.emplace_back(&RaftNode::StartNode, &leader);
  threads.emplace_back(&RaftNode::StartNode, &follower);

  std::this_thread::sleep_for(std::chrono::milliseconds(2000));

  ASSERT_TRUE(leader.GetState() == NodeState::Leader &&
              follower.GetState() == NodeState::Follower)
      << "node states still hold";

  auto req2 =
      std::vector<ServerRequest>{ServerRequest{ServerAction::Add, "y", 2}};
  leader.SendRequest(req2);

  auto cond = [&] {
    return (6 == follower.GetCommitIndex() && 6 == leader.GetCommitIndex());
  };

  auto res = WaitForCondition(cond, 5000);
  ASSERT_TRUE(res)
      << "follower and leaders do not converge to same commit index";

  follower.StopNode();
  leader.StopNode();

  for (auto &t : threads) {
    t.join();
  }
}

// TODO test instances where node shuts down and then resumes after entry was
// committed and all other similar instances

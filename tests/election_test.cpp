#include "../src/network.h"
#include "../src/raft_node.h"
#include "raft_node_testing.h"
#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>

class ElectionTest : public RaftNodeTest {};

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

  auto leader_term = nodes[leader_index]->GetCommitIndex();

  nodes[leader_index].get()->StopNode();

  auto res2 = WaitForCondition(cond, 1000);
  ASSERT_TRUE(res2) << "no single leader elected after previous leader dies";

  for (auto &node : nodes) {
    if (node->GetState() == NodeState::Leader) {
      ASSERT_GT(node->GetTerm(), leader_term);
      return;
    }
  }
}

TEST_F(ElectionTest, HandlesFalseCandidateDemotion) {
  auto cond = [this] { return ExactlyOneLeader(); };

  auto res = WaitForCondition(cond, 2000);
  ASSERT_TRUE(res) << "no single leader elected after 2 sec";

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

  auto res = WaitForCondition(cond, 2000);
  ASSERT_TRUE(res) << "multi candidate election does not resolve to single "
                      "leader after 2 sec";
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

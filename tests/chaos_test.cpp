#include "raft_node.h"
#include "raft_node_testing.h"
#include "rpc.h"
#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>
class ChaosTest : public RaftNodeTest {

  void SetModerateNetworkIssue() {
    network.SetDelayMS(50);
    network.SetDropRate(0.3);
  }
};

TEST_F(ChaosTest, RecoversFromSinglePartititon) {
  auto cond = [this] { return ExactlyOneLeader(); };
  auto res = WaitForCondition(cond, 2000);
  ASSERT_TRUE(res) << "Single leader invaraint does not hold after 2 sec";

  size_t partioned_follower;
  RaftNode *leader;
  for (size_t i = 0; i < nodes.size(); i++) {
    if (nodes[i]->GetState() == NodeState::Follower) {
      partioned_follower = i;
    } else if (nodes[i]->GetState() == NodeState::Leader) {
      leader = nodes[i].get();
    }
  }

  network.AddToPartioned(partioned_follower);

  std::vector<ServerRequest> reqs;
  for (auto i = 0; i < 5; i++) {
    reqs.emplace_back(ServerRequest{ServerAction::Add, std::to_string(i), i});
  }

  leader->SendRequest(reqs);

  std::this_thread::sleep_for(std::chrono::milliseconds(1200));

  auto leader_log = leader->GetLog();
  for (auto &node : nodes) {
    if (node->GetState() == NodeState::Leader) {
      continue;
    }

    auto log = node->GetLog();
    for (auto i = 0; i < log.size(); i++) {
      if (log[i] != leader_log.at(i)) {
        ASSERT_TRUE(false)
            << "at least one of the nodes still does not have a replicated log "
               "with the current leader";
      }
    }
  }

  // have to do size == 1 since each log has a initial sentry entry
  ASSERT_TRUE(nodes[partioned_follower]->GetLog().size() == 1)
      << "partioned follower still received AppendEntries calls from leader "
         "while it was partitioned";

  network.RemoveFromPartioned(partioned_follower);

  std::this_thread::sleep_for(std::chrono::milliseconds(2000));

  res = WaitForCondition(cond, 2000);
  ASSERT_TRUE(res) << "Single leader invariant doesn't hold after partitioned "
                      "node returns (~2 sec test)";

  auto partioned_follower_log = nodes[partioned_follower]->GetLog();

  for (auto i = 0; i < leader_log.size(); i++) {
    if (i >= partioned_follower_log.size() ||
        partioned_follower_log[i] != leader_log[i]) {
      ASSERT_TRUE(false) << "previously partioned follower does not have a "
                            "replicated log with leader after ~1.2 sec";
    }
  }

  auto cond2 = [&, this] {
    return leader->GetCommitIndex() ==
           nodes[partioned_follower]->GetCommitIndex();
  };

  res = WaitForCondition(cond2, 1500);
  ASSERT_TRUE(res) << "previously partioned follower does not have matching "
                      "commitIndex with leader after ~1.5 sec";
}

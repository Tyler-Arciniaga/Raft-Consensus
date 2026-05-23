#include "network.h"
#include "logger.h"
#include "raft_node.h"
#include "rpc.h"
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

SimulatedNetwork::SimulatedNetwork() {}

SimulatedNetwork::SimulatedNetwork(float dropRate, size_t delayMS)
    : dropRate(dropRate), delayMS(delayMS) {
  if (dropRate < 0.0 || dropRate > 1.0) {
    throw std::invalid_argument{
        "Network drop rate must fall within [0.0, 1.0]"};
  }
}

RequestVoteReply
SimulatedNetwork::sendRequestVote(size_t senderID, size_t targetID,
                                  const RequestVoteArgs &args) {
  auto shouldDrop = SimulateNetworkIssues(senderID, targetID);
  if (shouldDrop) {
    // Logger::getLogger().log(
    //     "(NETWORK) RequestVoteRPC being dropped by network...\n");
    return RequestVoteReply{.hadNetworkFailure = true};
  }

  auto targetNode = nodes[targetID];
  auto reply = targetNode->RequestVote(args);
  return reply;
}

AppendEntriesReply
SimulatedNetwork::sendAppendEntries(size_t senderID, size_t targetID,
                                    const AppendEntriesArgs &args) {
  auto shouldDrop = SimulateNetworkIssues(senderID, targetID);
  if (shouldDrop) {
    // Logger::getLogger().log(
    //     "(NETWORK) AppendEntriesRPC being dropped by network...\n");
    return AppendEntriesReply{.hadNetworkFailure = true};
  }

  auto targetNode = nodes[targetID];
  auto reply = targetNode->AppendEntries(args);
  return reply;
}

bool SimulatedNetwork::forwardClientRequest(
    size_t senderID, size_t targetID, const std::vector<ServerRequest> &reqs) {
  auto shouldDrop = SimulateNetworkIssues(senderID, targetID);
  if (shouldDrop) {
    return false;
  }

  return nodes[targetID]->SendRequest(reqs);
}

void SimulatedNetwork::AddNode(RaftNode *node) { nodes.push_back(node); }

void SimulatedNetwork::SetDropRate(float rate) { dropRate = rate; }

void SimulatedNetwork::SetDelayMS(size_t delay) { delayMS = delay; }

const bool SimulatedNetwork::InSameNetworkPartition(size_t senderID,
                                                    size_t targetID) {
  std::lock_guard<std::mutex> lock(mtx);

  auto end_itr = partitioned.end();
  size_t count = 0;
  if (partitioned.find(senderID) == end_itr) {
    count++;
  }
  if (partitioned.find(targetID) == end_itr) {
    count++;
  }

  // is only one of the nodes in the partitioned set?
  return count != 1;
}

// simulates all ways network can fail (delay, drop messages, etc), returns true
// if simNet shouldn't deliver msg, o.w. false
const bool SimulatedNetwork::SimulateNetworkIssues(size_t senderID,
                                                   size_t targetID) {
  if (!InSameNetworkPartition(senderID, targetID)) {
    return true;
  }

  float random;
  {
    std::lock_guard<std::mutex> lock(mtx);
    random = dist(rng);
  }

  if (dropRate <= random) {
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMS));
    return false;
  }

  return true;
}

void SimulatedNetwork::AddToPartioned(size_t nodeID) {
  {
    std::lock_guard<std::mutex> lock(mtx);
    partitioned.insert(nodeID);
  }

  Logger::getLogger().log("(NETWORK) node " + std::to_string(nodeID) +
                          " is now partitioned from the main set\n");
}

void SimulatedNetwork::RemoveFromPartioned(size_t nodeID) {
  {
    std::lock_guard<std::mutex> lock(mtx);
    partitioned.erase(nodeID);
  }

  Logger::getLogger().log("(NETWORK) node " + std::to_string(nodeID) +
                          " is no longer partitioned from the main set\n");
}

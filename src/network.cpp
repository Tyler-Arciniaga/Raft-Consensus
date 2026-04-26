#include "network.h"
#include "raft_node.h"
#include "rpc.h"
#include <chrono>
#include <mutex>
#include <stdexcept>
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
SimulatedNetwork::sendRequestVote(size_t targetID,
                                  const RequestVoteArgs &args) {
  auto shouldDrop = SimulateNetworkIssues();
  if (shouldDrop) {
    return RequestVoteReply{};
  }

  auto targetNode = nodes[targetID];
  auto reply = targetNode->RequestVote(args);
  return reply;
}

AppendEntriesReply
SimulatedNetwork::sendAppendEntries(size_t targetID,
                                    const AppendEntriesArgs &args) {
  auto shouldDrop = SimulateNetworkIssues();
  if (shouldDrop) {
    return AppendEntriesReply{};
  }

  auto targetNode = nodes[targetID];
  auto reply = targetNode->AppendEntries(args);
  return reply;
}

void SimulatedNetwork::AddNode(RaftNode *node) { nodes.push_back(node); }

void SimulatedNetwork::SetDropRate(float rate) { dropRate = rate; }

void SimulatedNetwork::SetDelayMS(size_t delay) { delayMS = delay; }

const bool SimulatedNetwork::SimulateNetworkIssues() {
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

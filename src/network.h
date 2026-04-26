#pragma once
#include "rpc.h"
#include <random>
#include <set>
#include <vector>

class RaftNode; // forward declaration to break dependency loop between RaftNode
                // and Network base class

class Network {
public:
  virtual RequestVoteReply sendRequestVote(size_t targetID,
                                           const RequestVoteArgs &args) = 0;
  virtual AppendEntriesReply
  sendAppendEntries(size_t targetID, const AppendEntriesArgs &args) = 0;
};

class SimulatedNetwork : public Network {
private:
  // dropRate should be between 0.0 and 1.0
  float dropRate = 0.0;
  std::mt19937 rng;
  std::uniform_real_distribution<float> dist{0.0, 1.0};

  size_t delayMS = 0;
  std::set<size_t> partitioned; // set of IDs of nodes that cannot communicate
                                // due to a network partition
  std::vector<RaftNode *> nodes;

  std::mutex mtx;

public:
  SimulatedNetwork();
  SimulatedNetwork(float dropRate, size_t delayMS);

  RequestVoteReply sendRequestVote(size_t targetID,
                                   const RequestVoteArgs &args) override;
  AppendEntriesReply sendAppendEntries(size_t targetID,
                                       const AppendEntriesArgs &args) override;

  void AddNode(RaftNode *node);
  void SetDropRate(float rate);
  void SetDelayMS(size_t delay);
  const bool SimulateNetworkIssues();
};

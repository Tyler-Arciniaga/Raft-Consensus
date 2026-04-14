#pragma once
#include "rpc.h"
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
  static constexpr float dropRate = 0.0;
  static constexpr size_t delayMS = 0;
  std::set<size_t> partitioned; // set of IDs of nodes that cannot communicate
                                // due to a network partition
  std::vector<RaftNode *> nodes;

public:
  RequestVoteReply sendRequestVote(size_t targetID,
                                   const RequestVoteArgs &args);
  AppendEntriesReply sendAppendEntries(size_t targetID,
                                       const AppendEntriesArgs &args);

  void AddNode(RaftNode *node);
};

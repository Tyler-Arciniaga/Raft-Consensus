#pragma once
#include "rpc.h"
#include <set>

class Network {
public:
  virtual RequestVoteReply sendRequestVote(size_t targetID,
                                           RequestVoteArgs args);
  virtual AppendEntriesReply sendAppendEntries(size_t targetID,
                                               AppendEntriesArgs args);
};

class SimulatedNetwork : public Network {
  static constexpr float dropRate = 0.0;
  static constexpr size_t delayMS = 0;
  std::set<size_t> partitioned; // set of IDs of nodes that cannot communicate
                                // due to a network partition

  RequestVoteReply sendRequestVote(size_t targetID, RequestVoteArgs args);
  AppendEntriesReply sendAppendEntries(size_t targetID, AppendEntriesArgs args);
};

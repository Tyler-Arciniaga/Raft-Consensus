#include "network.h"
#include "rpc.h"

RequestVoteReply SimulatedNetwork::sendRequestVote(size_t targetID,
                                                   RequestVoteArgs args) {
  return RequestVoteReply{};
}

AppendEntriesReply SimulatedNetwork::sendAppendEntries(size_t targetID,
                                                       AppendEntriesArgs args) {
  return AppendEntriesReply{};
}

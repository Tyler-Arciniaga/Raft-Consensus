#include "network.h"
#include "raft_node.h"
#include "rpc.h"

RequestVoteReply SimulatedNetwork::sendRequestVote(size_t targetID,
                                                   RequestVoteArgs args) {
  auto targetNode = nodes[targetID];
  auto reply = targetNode->RequestVote(args);
  return reply;
}

AppendEntriesReply SimulatedNetwork::sendAppendEntries(size_t targetID,
                                                       AppendEntriesArgs args) {
  return AppendEntriesReply{};
}

void SimulatedNetwork::AddNode(RaftNode *node) { nodes.push_back(node); }

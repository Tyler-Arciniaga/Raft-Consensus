#include "raft_node.h"
#include <cstdint>
#include <iostream>
#include <string>

std::string print_state(NodeState state) {
  std::string state_string;
  switch (state) {
  case NodeState::Follower:
    state_string = "Follower";
    break;
  case NodeState::Candidate:
    state_string = "Candidate";
    break;
  case NodeState::Leader:
    state_string = "Leader";
    break;
  }

  return state_string;
}

void print_switch_state_statement(uint64_t nodeID, NodeState oldState,
                                  NodeState newState) {
  std::cout << "Switching node " << nodeID << " from " << print_state(oldState)
            << " to " << print_state(newState) << "\n";
}

// RaftNode logic

// RaftNode constructor
RaftNode::RaftNode(size_t nodeID, std::random_device rd)
    : nodeID(nodeID), state(NodeState::Follower), currentTerm(0),
      commitIndex(0), lastApplied(0) {

  Log.resize(
      25); // give the node's log an initial size so that we can index directly
           // into the log without having to push new elements in

  rng = std::mt19937(rd());
  dist = std::uniform_int_distribution<int>(150, 300);

  WaitForAppendEntries();
};

// base state for RaftNode, called after init process and is default state of
// all Follower nodes
void RaftNode::WaitForAppendEntries() {
  int random_timeout = dist(rng);
  std::cout << nodeID << ": " << random_timeout << "\n";
}

void RaftNode::SwitchStateToFollower() {
  print_switch_state_statement(nodeID, state, NodeState::Follower);
  state = NodeState::Follower;
}

void RaftNode::SwitchStateToCandidate() {
  print_switch_state_statement(nodeID, state, NodeState::Candidate);
  state = NodeState::Candidate;
}

void RaftNode::SwitchStateToLeader() {
  print_switch_state_statement(nodeID, state, NodeState::Leader);
  state = NodeState::Leader;
}

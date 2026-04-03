#include "raft_node.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
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
RaftNode::RaftNode(size_t nodeID, std::random_device &rd)
    : nodeID(nodeID), state(NodeState::Follower), currentTerm(0),
      commitIndex(0), lastApplied(0), randomizer(Randomizer(rd)),
      cv(std::condition_variable{}), follower_mtx(std::mutex{}) {

  void HandleFollowerState();
};

// RaftNode RPC functions
RequestVoteReply RaftNode::RequestVote(RequestVoteArgs args) {
  if (currentTerm > args.candidate_term) {
    return RequestVoteReply{currentTerm, false};
  } // immediately reject RequestVote if local node's term is higher than
    // candidate's term

  if (votedFor == UINT32_MAX || votedFor == args.candidateID) {
    size_t lastLogIndex = Log.size() - 1;
    if (Log[lastLogIndex].termReceived != args.lastLogTerm) {
      return RequestVoteReply{currentTerm, Log[lastLogIndex].termReceived <
                                               args.lastLogTerm};
    } // first compare term of both log's last entry

    // finally compare length of both log's if the last entries had the same
    // term
    return RequestVoteReply{currentTerm, lastLogIndex < args.lastLogIndex};
  }

  // local node has already voted in this election
  return RequestVoteReply{currentTerm, false};
}

// main follower state function, has infinite loop only broken if current
// election timer countdown reached before AppendEntry RPC can notify condition
// variable
void RaftNode::HandleFollowerState() {
  std::unique_lock<std::mutex> lock(follower_mtx);

  while (true) {
    int new_countdown_duration = randomizer.GetRandomElectionTimeout();
    if (cv.wait_for(lock, std::chrono::milliseconds(new_countdown_duration)) ==
        std::cv_status::timeout) {
      break;
    }
  }

  currentTerm++;
  SwitchStateToCandidate();
  HandleCandidateState();
}

// TODO
void RaftNode::HandleCandidateState() {}

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

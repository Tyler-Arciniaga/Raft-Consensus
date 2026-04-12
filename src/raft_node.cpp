#include "raft_node.h"
#include "rpc.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

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
  std::ostringstream os;
  os << "Switching node " << nodeID << " from " << print_state(oldState)
     << " to " << print_state(newState) << "\n";

  Logger::getLogger().log(os.str());
}

// RaftNode logic

// RaftNode constructor
RaftNode::RaftNode(size_t nodeID, std::random_device &rd, Network &network)
    : nodeID(nodeID), state(NodeState::Follower), Log(std::vector<LogEntry>{}),
      currentTerm(0), commitIndex(0), lastApplied(0),
      randomizer(Randomizer(rd)), network(network) {};

// RaftNode RPC functions
void RaftNode::StartNode() {
  while (true) {
    switch (state) {
    case NodeState::Follower:
      HandleFollowerState();
      break;
    case NodeState::Candidate:
      HandleCandidateState();
      break;
    case NodeState::Leader:
      HandleLeaderState();
      break;
    default:
      throw std::runtime_error("undefined state encountered!");
    }
  }
}

// TODO MAJOR!!! probably need a lock on this for currentTerm
RequestVoteReply RaftNode::RequestVote(RequestVoteArgs args) {
  std::lock_guard<std::mutex> lock(
      mtx); // hold lock for the duration of this RPC

  if (currentTerm > args.candidate_term) {
    return RequestVoteReply{currentTerm, false};
  } // immediately reject RequestVote if local node's term is higher than
    // candidate's term

  if (currentTerm < args.candidate_term) {
    currentTerm = args.candidate_term;

    if (state != NodeState::Follower) {
      SwitchStateToFollower();
    }

    votedFor = UINT32_MAX;
  }

  if (votedFor == UINT32_MAX || votedFor == args.candidateID) {
    size_t lastLogIndex = (Log.size() == 0) ? 0 : Log.size() - 1;
    uint64_t lastTermReceived =
        (Log.size() == 0) ? 0 : Log[lastLogIndex].termReceived;
    if (lastTermReceived != args.lastLogTerm) {
      return RequestVoteReply{currentTerm, lastTermReceived < args.lastLogTerm};
    } // first compare term of both log last entries

    // if terms of last entry are same, compare length of both logs
    return RequestVoteReply{currentTerm, lastLogIndex <= args.lastLogIndex};
  } else {
    // local node has already voted in this election
    return RequestVoteReply{currentTerm, false};
  }
}

// main follower state function, has infinite loop only broken if current
// election timer countdown reached before AppendEntry RPC can notify condition
// variable
void RaftNode::HandleFollowerState() {
  std::unique_lock<std::mutex> lock(mtx);
  std::condition_variable cv;
  // TODO spin off a new thread to handle receiving AppendEntries RPC

  while (true) {
    int new_countdown_duration = randomizer.GetRandomElectionTimeout();
    if (cv.wait_for(lock, std::chrono::milliseconds(new_countdown_duration)) ==
        std::cv_status::timeout) {
      break;
    }
  }

  SwitchStateToCandidate();
}

void RaftNode::SendRequestVoteRPC(size_t targetID, uint32_t &voteCounter,
                                  std::mutex &counterMtx,
                                  std::condition_variable &cv) {
  uint64_t lastLogTerm =
      (Log.size() == 0) ? 0 : Log[Log.size() - 1].termReceived;
  size_t lastLogIndex = (Log.size() == 0) ? 0 : Log.size() - 1;
  RequestVoteArgs arg{currentTerm, nodeID, lastLogIndex, lastLogTerm};

  // TODO implement retry logic if network cannot reach target node
  auto reply = network.sendRequestVote(targetID, arg);

  if (reply.voteGranted) {
    Logger::getLogger().log(std::to_string(nodeID) + " from " +
                            std::to_string(targetID) + "\n");
    {
      std::lock_guard<std::mutex> lock(counterMtx);
      voteCounter++;
    }
    cv.notify_one();
  }
}

// TODO handle receiving an AppendEntries RPC from another node
void RaftNode::HandleCandidateState() {
  {
    std::lock_guard<std::mutex> lock(mtx);
    currentTerm++;
  }

  while (true) {
    uint32_t voteCounter = 1;
    std::mutex counterMtx;
    std::condition_variable cv;

    for (auto targetID : peers) {
      if (targetID != nodeID) {
        std::thread t(&RaftNode::SendRequestVoteRPC, this, targetID,
                      std::ref(voteCounter), std::ref(counterMtx),
                      std::ref(cv));
        t.detach();
      }
    }

    // sleep candidate's main thread until either deadline is reached
    // (election timeout) or candidate receives majority votes
    std::unique_lock<std::mutex> lock(counterMtx);
    bool electionResult = cv.wait_until(
        lock,
        std::chrono::steady_clock::now() +
            std::chrono::milliseconds(randomizer.GetRandomElectionTimeout()),
        [this, &voteCounter] {
          return voteCounter > uint32_t(peers.size() / 2);
        });

    // Logger::getLogger().log(std::to_string(electionResult) + "\n");

    // TODO consider setting an atomic stop bool to make all threads quit (in
    // case any sendRequestVote threads are still retrying)

    if (electionResult) {
      break;
    } else {
      continue;
    }
  }

  SwitchStateToLeader();
}

void RaftNode::SendHeartbeatRPCs(size_t targetID, std::atomic<bool> &stop) {
  size_t prevLogIndex = (Log.size() == 0) ? 0 : Log.size() - 1;
  uint64_t prevLogTerm =
      (Log.size() == 0) ? 0 : Log[Log.size() - 1].termReceived;
  auto arg = AppendEntriesArgs{
      currentTerm, nodeID, prevLogIndex, prevLogTerm, std::vector<LogEntry>{},
      commitIndex};

  // TODO think I may need to add atomic safety to updating currentTerm
  while (!stop.load()) {
    auto reply = network.sendAppendEntries(targetID, arg);
    if (reply.term > currentTerm) {
      currentTerm = reply.term;
      return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void RaftNode::HandleLeaderState() {
  std::atomic<bool> stop;
  std::vector<std::thread> heartbeatThreads;
  for (auto targetID : peers) {
    if (targetID != nodeID) {
      heartbeatThreads.emplace_back(&RaftNode::SendHeartbeatRPCs, this,
                                    targetID, std::ref(stop));
    }
  }

  for (auto &t : heartbeatThreads) {
    t.join();
  }

  SwitchStateToFollower();
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

#include "raft_node.h"
#include "rpc.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
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
  std::cout << "Switching node " << nodeID << " from " << print_state(oldState)
            << " to " << print_state(newState) << "\n";
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

RequestVoteReply RaftNode::RequestVote(RequestVoteArgs args) {
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
    size_t lastLogIndex = Log.size() - 1;
    uint64_t lastTermReceived =
        (Log.size() == 0) ? 0 : Log[lastLogIndex].termReceived;
    if (lastTermReceived != args.lastLogTerm) {
      return RequestVoteReply{currentTerm, lastTermReceived < args.lastLogTerm};
    } // first compare term of both log's last entry

    // finally compare length of both log's if the last entries had the same
    // term
    return RequestVoteReply{currentTerm, lastLogIndex < args.lastLogIndex};
  } else {
    // local node has already voted in this election
    return RequestVoteReply{currentTerm, false};
  }
}

// main follower state function, has infinite loop only broken if current
// election timer countdown reached before AppendEntry RPC can notify condition
// variable
void RaftNode::HandleFollowerState() {
  std::mutex follower_mtx;
  std::unique_lock<std::mutex> lock(follower_mtx);
  std::condition_variable cv;
  // TODO spin off a new thread to handle receiving AppendEntries RPC

  while (true) {
    int new_countdown_duration = randomizer.GetRandomElectionTimeout();
    if (cv.wait_for(lock, std::chrono::milliseconds(new_countdown_duration)) ==
        std::cv_status::timeout) {
      break;
    }
  }

  currentTerm++;
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
    {
      std::lock_guard<std::mutex> lock(counterMtx);
      voteCounter++;
    }
    cv.notify_one();
  }
}

// TODO handle receiving an AppendEntries RPC from another node
void RaftNode::HandleCandidateState() {
  while (true) {
    uint32_t voteCounter = 1;
    std::mutex counterMtx;
    std::condition_variable cv;

    std::vector<std::thread> voteReqThreads;
    for (auto targetID : peers) {
      if (targetID != nodeID) {
        voteReqThreads.emplace_back(std::thread(
            &RaftNode::SendRequestVoteRPC, this, targetID,
            std::ref(voteCounter), std::ref(counterMtx), std::ref(cv)));
      }
    }

    // sleep candidate's main thread until either a certain deadline is reached
    // (election timeout) or candidate receives majority votes
    std::unique_lock<std::mutex> lock(counterMtx);
    bool electionResult = cv.wait_until(
        lock,
        std::chrono::steady_clock::now() +
            std::chrono::milliseconds(randomizer.GetRandomElectionTimeout()),
        [this, &voteCounter] {
          return voteCounter > uint32_t(peers.size() / 2);
        });

    // TODO not sure about this, but just for now i'll have main thread clean up
    // request threads (maybe here is where I have atomic stop bool be called to
    // force req threads to exit)
    for (auto &t : voteReqThreads) {
      t.join();
    }

    if (electionResult) {
      break;
    } else {
      currentTerm++;
      continue;
    }
  }

  SwitchStateToLeader();
}

void RaftNode::SendHeartbeatRPCs(size_t targetID, std::atomic<bool> &stop) {
  auto arg = AppendEntriesArgs{currentTerm,
                               nodeID,
                               Log.size() - 1,
                               Log[Log.size() - 1].termReceived,
                               std::vector<LogEntry>{},
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

// TODO implement leader state logic
void RaftNode::HandleLeaderState() {
  std::atomic<bool> stop;
  std::vector<std::thread *> heartbeatThreads;
  for (auto targetID : peers) {
    if (targetID != nodeID) {
      std::thread t(&RaftNode::SendHeartbeatRPCs, this, targetID,
                    std::ref(stop));
      heartbeatThreads.push_back(&t);
    }
  }

  for (auto &t : heartbeatThreads) {
    t->join();
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

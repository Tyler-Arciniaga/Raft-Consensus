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
  os << "--> Switching node " << nodeID << " from " << print_state(oldState)
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

RequestVoteReply RaftNode::RequestVote(const RequestVoteArgs &args) {
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

  // Compare logs, refresh election timer if vote is granted
  if (votedFor == UINT32_MAX || votedFor == args.candidateID) {
    RequestVoteReply reply;
    size_t lastLogIndex = (Log.size() == 0) ? 0 : Log.size() - 1;
    uint64_t lastTermReceived =
        (Log.size() == 0) ? 0 : Log[lastLogIndex].termReceived;

    if (lastTermReceived != args.lastLogTerm) {
      // first compare term of both log last entries
      reply =
          RequestVoteReply{currentTerm, lastTermReceived < args.lastLogTerm};
    } else {
      // if terms of last entry are equal, compare length of both logs
      reply = RequestVoteReply{currentTerm, lastLogIndex <= args.lastLogIndex};
    }

    if (reply.voteGranted) {
      votedFor = args.candidateID;
      heartbeat_cv.notify_one();
      Logger::getLogger().log("node " + std::to_string(nodeID) +
                              " sends yes vote to node " +
                              std::to_string(args.candidateID) + "\n");
    }
    return reply;
  } else {
    // local node has already voted in this election
    return RequestVoteReply{currentTerm, false};
  }
}

AppendEntriesReply RaftNode::AppendEntries(const AppendEntriesArgs &args) {
  // TODO implement AppendEntriesRPC logic
  // if entries sent is empty, this is a heartbeat
  if (args.entries.size() == 0) {
    Logger::getLogger().log("node " + std::to_string(nodeID) +
                            " receives heartbeat from node " +
                            std::to_string(args.leaderID) + "\n");
    heartbeat_cv.notify_one();
  }

  std::lock_guard<std::mutex> lock(mtx);
  // if currently a candidate and recieve AppendEntries from pre existing leader
  // with high enough term immediately demote to follower
  if (args.leader_term >= currentTerm && state == NodeState::Candidate) {
    SwitchStateToFollower();
  }

  return AppendEntriesReply{};
}

// main follower state function, has infinite loop only broken if current
// election timer countdown reached before AppendEntry RPC can notify condition
// variable
void RaftNode::HandleFollowerState() {
  std::unique_lock<std::mutex> lock(mtx);

  while (true) {
    int new_countdown_duration = randomizer.GetRandomElectionTimeout();
    if (heartbeat_cv.wait_for(
            lock, std::chrono::milliseconds(new_countdown_duration)) ==
        std::cv_status::timeout) {
      break;
    }
  }

  SwitchStateToCandidate();
}

void RaftNode::SendRequestVoteRPC(size_t targetID, VoteState &voteState,
                                  const std::atomic<bool> &stop) {
  uint64_t lastLogTerm =
      (Log.size() == 0) ? 0 : Log[Log.size() - 1].termReceived;
  size_t lastLogIndex = Log.size();
  RequestVoteArgs arg{currentTerm, nodeID, lastLogIndex, lastLogTerm};

  // TODO implement retry logic if network cannot reach target node
  while (!stop.load()) {
    auto reply = network.sendRequestVote(targetID, arg);

    if (reply.voteGranted) {
      Logger::getLogger().log("node " + std::to_string(nodeID) +
                              " received yes vote from node " +
                              std::to_string(targetID) + "\n");
      voteState.votesReceived++;
      voteState.cv.notify_one();
    }
  }
}

// TODO handle receiving an AppendEntries RPC from pre-existing Leader node
void RaftNode::HandleCandidateState() {
  while (true) {
    {
      std::lock_guard<std::mutex> lock(mtx);
      currentTerm++;
      votedFor = nodeID;
    }

    VoteState voteState;
    std::atomic<bool> stop{false};

    std::vector<std::thread> reqVoteThreads;
    for (auto targetID : peers) {
      if (targetID != nodeID) {
        reqVoteThreads.emplace_back(&RaftNode::SendRequestVoteRPC, this,
                                    targetID, std::ref(voteState),
                                    std::ref(stop));
      }
    }

    // sleep candidate's main thread until either deadline is reached
    // (election timeout) or candidate receives majority votes
    std::unique_lock<std::mutex> lock(voteState.mtx);
    bool electionResult = voteState.cv.wait_until(
        lock,
        std::chrono::steady_clock::now() +
            std::chrono::milliseconds(randomizer.GetRandomElectionTimeout()),
        [this, &voteState, &stop] {
          if (state == NodeState::Follower ||
              voteState.votesReceived > uint32_t(peers.size() / 2)) {
            stop = true;
            return true;
          }
          return false;
        });

    for (auto &t : reqVoteThreads) {
      t.join();
    }

    if (state == NodeState::Follower) {
      return;
    }

    if (electionResult) {
      SwitchStateToLeader();
      return;
    } else {
      continue;
    }
  }
}

void RaftNode::SendHeartbeatRPCs(size_t targetID, std::atomic<bool> &stop) {
  // TODO think I may need to add atomic safety to updating currentTerm
  size_t prevLogIndex;
  uint64_t prevLogTerm;
  AppendEntriesArgs arg;
  while (!stop.load()) {
    {
      std::lock_guard<std::mutex> lock(mtx);

      prevLogIndex = (Log.size() == 0) ? 0 : Log.size() - 1;
      prevLogTerm = (Log.size() == 0) ? 0 : Log[Log.size() - 1].termReceived;
      arg = AppendEntriesArgs{currentTerm,
                              nodeID,
                              prevLogIndex,
                              prevLogTerm,
                              std::vector<LogEntry>{},
                              commitIndex};
    }

    Logger::getLogger().log("node " + std::to_string(nodeID) +
                            " sending heartbeat to node " +
                            std::to_string(targetID) + "\n");
    auto reply = network.sendAppendEntries(targetID, arg);
    if (reply.term > currentTerm) {
      {
        std::lock_guard<std::mutex> lock(mtx);
        currentTerm = reply.term;
      }

      stop = true;
      return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void RaftNode::HandleLeaderState() {
  std::atomic<bool> stop{false};
  std::vector<std::thread> heartbeatThreads;
  for (auto targetID : peers) {
    if (targetID != nodeID) {
      heartbeatThreads.emplace_back(&RaftNode::SendHeartbeatRPCs, this,
                                    targetID, std::ref(stop));
    }
  }

  for (size_t i = 1; i < heartbeatThreads.size(); i++) {
    auto &t = heartbeatThreads[i];
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

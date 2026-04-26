#include "raft_node.h"
#include "logger.h"
#include "rpc.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
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
// NOTE: Log is initialized with a dummy sentinel entry to make the math work
// out better, it is never committed or applied to node's state machine
RaftNode::RaftNode(size_t nodeID, std::random_device &rd, Network &network)
    : nodeID(nodeID), Log(std::vector<LogEntry>{LogEntry{.termReceived = 0}}),
      randomizer(Randomizer(rd)), network(network) {};

// RaftNode RPC functions
void RaftNode::StartNode() {
  std::thread state_machine_application_thread(&RaftNode::ApplyToStateMachine,
                                               this);

  while (true) {
    if (node_shutdown.load()) {
      state_machine_cv.notify_one(); // wake up state machine apply loop thread
                                     // so that we can gracefully exit
      state_machine_application_thread.join();
      return;
    }

    switch (state.load()) {
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

void RaftNode::ApplyToStateMachine() {
  std::unique_lock<std::mutex> lock(mtx);
  while (!node_shutdown.load()) {
    state_machine_cv.wait(lock, [this] {
      return node_shutdown.load() || (lastApplied < commitIndex);
    });

    if (node_shutdown.load()) {
      return;
    }

    // take a snapshot of the current commitIndex and slice of log so that state
    // machine background thread can unlock early
    auto currentCommitIndex = commitIndex;
    auto new_log_snapshot =
        std::vector<LogEntry>(Log.begin() + lastApplied + 1, Log.end());

    lock.unlock();

    // unique lock holds lock at this point...
    for (auto i = 0; lastApplied < currentCommitIndex; i++) {
      ApplySingleLogEntry(new_log_snapshot.at(i));
      lastApplied += 1;
    }

    // reaquire mtx lock since cv.wait() requires lock to be held when called
    lock.lock();
  }
}

void RaftNode::ApplySingleLogEntry(const LogEntry &entry) {
  if (entry.action == ServerAction::Add) {
    state_machine.insert({entry.key, entry.value});
  } else {
    auto itr = state_machine.find(entry.key);
    if (itr != state_machine.end()) {
      state_machine.erase(itr);
    }
  }
}

int RaftNode::FetchFromStateMachine(std::string key) {
  std::lock_guard<std::mutex> lock(mtx);
  auto itr = state_machine.find(key);
  if (itr == state_machine.end()) {
    return std::numeric_limits<int>::max();
  }

  return itr->second;
}

void RaftNode::SendAppendEntriesRPC(
    const AppendEntriesArgs &arg, size_t targetID,
    std::condition_variable &advance_commit_index_cv) {
  auto followers_args = arg;

  while (true && state == NodeState::Leader) {
    Logger::getLogger().log("(LOG) node " + std::to_string(nodeID) +
                            " sending AppendEntriesRPC to node " +
                            std::to_string(targetID) + "\n");

    auto reply = network.sendAppendEntries(targetID, followers_args);

    std::lock_guard<std::mutex> lock(mtx);
    if (reply.sucesss) {
      auto lastSentIndex = arg.prevLogIndex + arg.entries.size();
      matchIndex[targetID] = lastSentIndex;
      nextIndex[targetID] = lastSentIndex + 1;

      advance_commit_index_cv.notify_one();
      return;
    } else {
      nextIndex[targetID]--;

      followers_args.prevLogIndex--;
      followers_args.prevLogTerm =
          Log[followers_args.prevLogIndex].termReceived;
      followers_args.entries = std::vector<LogEntry>(
          Log.begin() + followers_args.prevLogIndex, Log.end());
      continue;
    }
  }
}

// for now assume this is only ever called on Leader node
bool RaftNode::SendRequest(const std::vector<ServerRequest> &reqs) {
  AppendEntriesArgs arg;
  {
    std::lock_guard<std::mutex> lock(mtx);
    size_t prevLogIndex = Log.size() - 1;
    uint64_t prevLogTerm = Log[prevLogIndex].termReceived;

    auto entries = AppendToLog(reqs);

    arg = AppendEntriesArgs{currentTerm, nodeID,  prevLogIndex,
                            prevLogTerm, entries, commitIndex};
  }

  std::vector<std::thread> append_entries_thread;
  std::condition_variable advance_commit_index_cv;
  for (auto targetID : peers) {
    if (targetID != nodeID) {
      append_entries_thread.emplace_back(&RaftNode::SendAppendEntriesRPC, this,
                                         std::ref(arg), targetID,
                                         std::ref(advance_commit_index_cv));
    }
  }

  std::unique_lock<std::mutex> lock(mtx);
  advance_commit_index_cv.wait(lock,
                               [this] { return TryAdvancingCommitIndex(); });
  lock.unlock();

  for (auto &t : append_entries_thread) {
    t.join();
  }

  return true; // placeholder
}

// converts reqs into Log entries and appends to back of log, then returns newly
// added entries
// NOTE: this function is not safe, requires caller to be holding lock.
std::vector<LogEntry>
RaftNode::AppendToLog(const std::vector<ServerRequest> &reqs) {
  std::vector<LogEntry> entries;
  entries.reserve(reqs.size());

  for (auto &req : reqs) {
    entries.push_back(LogEntry{req.action, req.key, req.value, currentTerm});
  }

  Log.insert(Log.end(), entries.begin(), entries.end());

  return entries;
}

// NOTE: this function is not safe, requires caller to be holding lock.
bool RaftNode::TryAdvancingCommitIndex() {
  // check all entries above the prev commit index
  for (int i = commitIndex + 1; i < Log.size(); i++) {
    // leader only commits entries from their term (will implicitely commit
    // previous uncomitted entires via invariant)
    if (Log[i].termReceived != currentTerm) {
      continue;
    }

    size_t numReplicated = 1;
    for (int n = 0; n < peers.size(); n++) {
      if (n == nodeID) {
        continue;
      }

      if (matchIndex[n] >= i) {
        numReplicated++;
      }
    }

    // majority check
    if (!(numReplicated > peers.size() / 2)) {
      return false;
    }

    commitIndex = i;
    state_machine_cv.notify_one();
  }

  return true;
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

    // if node is leader or candidate and discovers higher term, immediately
    // demote to follower
    if (state.load() != NodeState::Follower) {
      SwitchStateToFollower();
      voting_cv.notify_one();
    }

    votedFor = UINT32_MAX;
  }

  // Compare logs, refresh election timer if vote is granted
  if (votedFor == UINT32_MAX || votedFor == args.candidateID) {
    RequestVoteReply reply;
    size_t lastLogIndex = Log.size() - 1;
    uint64_t lastTermReceived = Log[lastLogIndex].termReceived;

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
      Logger::getLogger().log("(VOTE) node " + std::to_string(nodeID) +
                              " sends yes vote to node " +
                              std::to_string(args.candidateID) + "\n");
    }
    return reply;
  } else {
    // local node has already voted in this term's election
    return RequestVoteReply{currentTerm, false};
  }
}

AppendEntriesReply RaftNode::AppendEntries(const AppendEntriesArgs &args) {
  std::lock_guard<std::mutex> lock(mtx);

  if (args.leader_term < currentTerm) {
    return AppendEntriesReply{currentTerm, false};
  }

  // always reset election timer (has no effect if node is leader or candidate)
  heartbeat_cv.notify_one();

  // update currentTerm if higher and demote when necessary
  if (args.leader_term >= currentTerm) {
    currentTerm = args.leader_term;
    if (state.load() != NodeState::Follower) {
      SwitchStateToFollower();
    }
  }

  // if entries is not empty than this is a regular AppendEntriesRPC...
  if (args.entries.size() != 0) {
    Logger::getLogger().log("(LOG) node " + std::to_string(nodeID) +
                            " receives AppendEntriesRPC from node " +
                            std::to_string(args.leaderID) + "\n");

    if (args.prevLogIndex >= Log.size() ||
        Log[args.prevLogIndex].termReceived != args.prevLogTerm) {
      return AppendEntriesReply{currentTerm, false};
    }

    // truncate all trailing entries and append leader's sent entries
    Log.resize(args.prevLogIndex + 1);
    Log.insert(Log.end(), args.entries.begin(), args.entries.end());
  } else {
    // ... must be a heartbeat then...
    Logger::getLogger().log("(HEARTBEAT) node " + std::to_string(nodeID) +
                            " receives heartbeat from node " +
                            std::to_string(args.leaderID) + "\n");
  }

  // always check and update commitIndex when necessary
  if (args.leaderCommitIndex > commitIndex) {
    commitIndex = std::min(args.leaderCommitIndex, Log.size() - 1);
    state_machine_cv.notify_one();
  }

  return AppendEntriesReply{currentTerm, true};
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

    if (node_shutdown.load()) {
      return;
    }
  }

  SwitchStateToCandidate();
}

void RaftNode::SendRequestVoteRPC(size_t targetID, VoteState &voteState,
                                  const std::atomic<bool> &stop) {
  RequestVoteArgs arg;
  {
    std::lock_guard<std::mutex> lock(mtx);
    size_t lastLogIndex = Log.size() - 1;
    uint64_t lastLogTerm = Log[lastLogIndex].termReceived;
    arg = RequestVoteArgs{currentTerm, nodeID, lastLogIndex, lastLogTerm};
  }

  while (!stop.load()) {
    auto reply = network.sendRequestVote(targetID, arg);

    if (reply.voteGranted) {
      Logger::getLogger().log("(VOTE) node " + std::to_string(nodeID) +
                              " received yes vote from node " +
                              std::to_string(targetID) + "\n");
      voteState.votesReceived++;
      voteState.cv.notify_one();
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mtx);
      if (reply.term > currentTerm) {
        currentTerm = reply.term;
        SwitchStateToFollower();
        return;
      }
    }
  }
}

void RaftNode::HandleCandidateState() {
  while (true) {
    {
      std::lock_guard<std::mutex> lock(mtx);
      currentTerm++;
      votedFor = nodeID;
      Logger::getLogger().log("--> node " + std::to_string(nodeID) +
                              " starting an election (term = " +
                              std::to_string(currentTerm) + ")...\n");
    }

    VoteState voteState(voting_cv);
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
          if (node_shutdown.load() || state.load() == NodeState::Follower ||
              voteState.votesReceived > uint32_t(peers.size() / 2)) {
            stop = true;
            return true;
          }
          return false;
        });

    for (auto &t : reqVoteThreads) {
      t.join();
    }

    // clean exit for node shutdown
    if (node_shutdown.load()) {
      return;
    }

    if (electionResult) {
      // atomic check and act operation on node state,
      // check if node is still candidate, if so promote to leader
      // otherwise may have been demoted to follower
      std::lock_guard<std::mutex> lock(mtx);
      if (state.load() == NodeState::Candidate) {
        SwitchStateToLeader();
        return;
      }

      return;
    } else {
      continue;
    }
  }
}

void RaftNode::SendHeartbeatRPCs(size_t targetID, std::atomic<bool> &stop) {
  size_t prevLogIndex;
  uint64_t prevLogTerm;
  AppendEntriesArgs arg;
  while (!stop.load()) {
    if (node_shutdown.load() || state.load() != NodeState::Leader) {
      stop = true;
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mtx);

      prevLogIndex = Log.size() - 1;
      prevLogTerm = Log[prevLogIndex].termReceived;
      arg = AppendEntriesArgs{currentTerm,
                              nodeID,
                              prevLogIndex,
                              prevLogTerm,
                              std::vector<LogEntry>{},
                              commitIndex};
    }

    Logger::getLogger().log("(HEARTBEAT) node " + std::to_string(nodeID) +
                            " sending heartbeat to node " +
                            std::to_string(targetID) + "\n");
    auto reply = network.sendAppendEntries(targetID, arg);
    {
      std::lock_guard<std::mutex> lock(mtx);
      if (reply.term > currentTerm) {
        {
          currentTerm = reply.term;
        }

        stop = true;
        return;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void RaftNode::HandleLeaderState() {
  RefreshVolatileLeaderState();

  std::atomic<bool> stop{false};
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

  if (node_shutdown.load()) {
    return;
  }

  // formally switch state to follower
  if (!node_shutdown.load() && state.load() != NodeState::Follower) {
    SwitchStateToFollower();
  }
}

// executed once node becomes a leader
void RaftNode::RefreshVolatileLeaderState() {
  std::lock_guard<std::mutex> lock(mtx);

  // set the nextIndex for each of the followers
  nextIndex = std::vector<uint32_t>(peers.size(), Log.size());

  // initialize the matchIndex to 0 for all followers
  matchIndex = std::vector<uint32_t>(peers.size(), 0);
}

void RaftNode::StopNode() {
  Logger::getLogger().log("(SHUTDOWN) shutting down node " +
                          std::to_string(nodeID) + "...\n");
  node_shutdown = true;
  heartbeat_cv.notify_one();
}

void RaftNode::SetPeers(const std::vector<size_t> p) {
  std::lock_guard<std::mutex> lock(mtx);
  peers = p;
}

NodeState RaftNode::GetState() const { return state.load(); }

void RaftNode::SetState(NodeState new_state) {
  switch (new_state) {
  case NodeState::Follower:
    SwitchStateToFollower();
    return;
  case NodeState::Candidate:
    SwitchStateToCandidate();
    return;
  case NodeState::Leader:
    SwitchStateToLeader();
    return;
  }
}

uint64_t RaftNode::GetTerm() {
  std::lock_guard<std::mutex> lock(mtx);
  return currentTerm;
}

void RaftNode::SetTerm(uint64_t new_term) {
  std::lock_guard<std::mutex> lock(mtx);
  currentTerm = new_term;
}

// NOTE: consider making commitIndex an atomic rather than acquiring entire lock
size_t RaftNode::GetCommitIndex() {
  std::lock_guard<std::mutex> lock(mtx);
  return commitIndex;
}

std::vector<LogEntry> RaftNode::GetLog() {
  std::lock_guard<std::mutex> lock(mtx);
  return Log;
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

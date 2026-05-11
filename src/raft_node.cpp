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
  std::thread peer_replication_cleanup_thread(
      &RaftNode::CleanUpReplicationThreads, this);

  while (true) {
    if (node_shutdown.load()) {
      state_machine_application_thread.join();
      peer_replication_cleanup_thread.join();
      Logger::getLogger().log("(SHUTDOWN) StartNode shutting down for node " +
                              std::to_string(nodeID) + "\n");
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
      Logger::getLogger().log(
          "(SHUTDOWN) ApplyToStateMachine exiting for node " +
          std::to_string(nodeID) + "\n");
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
  Logger::getLogger().log("(SHUTDOWN) ApplyToStateMachien exiting for node " +
                          std::to_string(nodeID) + "\n");
}

void RaftNode::ApplySingleLogEntry(const LogEntry &entry) {
  std::lock_guard<std::mutex> lock(state_machine_mtx);
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
  std::lock_guard<std::mutex> lock(state_machine_mtx);
  auto itr = state_machine.find(key);
  if (itr == state_machine.end()) {
    return std::numeric_limits<int>::max();
  }

  return itr->second;
}

void RaftNode::CleanUpReplicationThreads() {
  std::unique_lock<std::mutex> lock(peer_replication_mtx);
  while (!node_shutdown.load()) {
    peer_replication_cv.wait(lock, [&, this] {
      return node_shutdown.load() || (state.load() == NodeState::Leader &&
                                      peer_replication_threads.size() > 0 &&
                                      joinable_replication_threads.load() ==
                                          peer_replication_threads.size());
    });

    for (auto &t : peer_replication_threads) {
      t.join();
    }

    if (node_shutdown.load()) {
      Logger::getLogger().log(
          "(SHUTDOWN) CleanUpReplicationThreads exiting for node " +
          std::to_string(nodeID) + "\n");
      return;
    }

    peer_replication_threads.clear();
    joinable_replication_threads = 0;
  }
}

void RaftNode::SendAppendEntriesRPC(
    std::shared_ptr<AppendEntriesArgs> arg, size_t targetID,
    std::shared_ptr<std::condition_variable> advance_commit_index_cv) {

  // follower_args is thread local thus can be read and written without locking
  auto followers_args = *arg.get();

  while (!node_shutdown.load() && state.load() == NodeState::Leader) {
    Logger::getLogger().log("(LOG) node " + std::to_string(nodeID) +
                            " sending AppendEntriesRPC to node " +
                            std::to_string(targetID) + "\n");

    auto reply = network.sendAppendEntries(nodeID, targetID, followers_args);

    if (reply.hadNetworkFailure) {
      // back off for 100 MS, wake up once timeout or node_shutdown occurs
      std::unique_lock<std::mutex> lock(shutdown_mtx);
      shutdown_cv.wait_for(lock, std::chrono::milliseconds(100),
                           [this] { return node_shutdown.load(); });
      lock.unlock();

      if (node_shutdown.load()) {
        // notify advance_commit_index_cv so that Leader's SendRequest thread
        // can exit
        advance_commit_index_cv->notify_one();
        return;
      }

      continue;
    }

    std::lock_guard<std::mutex> lock(mtx);
    if (reply.sucess) {
      auto lastSentIndex =
          followers_args.prevLogIndex + followers_args.entries.size();
      matchIndex[targetID] = lastSentIndex;
      nextIndex[targetID] = lastSentIndex + 1;

      advance_commit_index_cv->notify_one();

      joinable_replication_threads += 1;
      peer_replication_cv.notify_one();
      return;
    } else {
      nextIndex[targetID]--;

      followers_args.prevLogIndex--;
      followers_args.prevLogTerm =
          Log[followers_args.prevLogIndex].termReceived;

      followers_args.entries = std::vector<LogEntry>(
          Log.begin() + followers_args.prevLogIndex + 1, Log.end());
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

  // std::vector<std::thread> append_entries_thread;
  auto shared_arg = std::make_shared<AppendEntriesArgs>(arg);
  // std::condition_variable advance_commit_index_cv;
  auto shared_cv = std::make_shared<std::condition_variable>();

  {
    std::lock_guard<std::mutex> lock2(peer_replication_mtx);
    for (auto targetID : peers) {
      if (targetID != nodeID) {
        peer_replication_threads.emplace_back(&RaftNode::SendAppendEntriesRPC,
                                              this, shared_arg, targetID,
                                              shared_cv);
      }
    }
  }

  std::unique_lock<std::mutex> lock(mtx);

  // TryAdvancingCommitIndex returns true when a majority of nodes have
  // replicated all of the new entries and thus the Leader can advance its
  // commit index to its new maximum
  shared_cv->wait(lock, [this] {
    return node_shutdown.load() || TryAdvancingCommitIndex();
  });
  lock.unlock();

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
      voting_cv.notify_one(); // handles case of node being candidate
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
      heartbeat_received = true;
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
  heartbeat_received = true;
  heartbeat_cv.notify_one();

  // update currentTerm if higher and demote when necessary
  if (args.leader_term == currentTerm && state == NodeState::Candidate) {
    // candidate must recognize incoming leader and demote...
    SwitchStateToFollower();
  } else if (args.leader_term > currentTerm) {
    currentTerm = args.leader_term;
    if (state != NodeState::Follower) {
      SwitchStateToFollower();
    }
  }

  if (args.entries.size() != 0) {
    // if args.entries isn't empty this is regular AppendEntriesRPC...
    Logger::getLogger().log("(LOG) node " + std::to_string(nodeID) +
                            " receives AppendEntriesRPC from node " +
                            std::to_string(args.leaderID) + "\n");
  } else {
    // must be a heartbeat then...
    Logger::getLogger().log("(HEARTBEAT) node " + std::to_string(nodeID) +
                            " receives heartbeat from node " +
                            std::to_string(args.leaderID) + "\n");
  }

  if (args.prevLogIndex >= Log.size() ||
      Log[args.prevLogIndex].termReceived != args.prevLogTerm) {
    return AppendEntriesReply{currentTerm, false};
  }

  // truncate all trailing entries and append leader's sent entries
  Log.resize(args.prevLogIndex + 1);
  Log.insert(Log.end(), args.entries.begin(), args.entries.end());

  // always check and update commitIndex when necessary
  if (args.leaderCommitIndex > commitIndex) {
    commitIndex = std::min(args.leaderCommitIndex, Log.size() - 1);
    state_machine_cv.notify_one();
  }

  return AppendEntriesReply{currentTerm, true};
}

// Main Follower state function, some key components:
// 1) Loops indefinitely as long as !node_shutdown and election timer does not
// timeout
// 2) Does not wake up when NodeState changes from Follower
void RaftNode::HandleFollowerState() {
  std::unique_lock<std::mutex> lock(mtx);

  while (!node_shutdown.load()) {
    int timeout = randomizer.GetRandomElectionTimeout();
    auto notified =
        heartbeat_cv.wait_for(lock, std::chrono::milliseconds(timeout), [this] {
          return node_shutdown.load() || heartbeat_received;
        });

    if (node_shutdown.load()) {
      Logger::getLogger().log(
          "(SHUTDOWN) HandleFollowerState exiting for node " +
          std::to_string(nodeID) + "\n");
      return;
    }

    // did not receive heartbeat within timeout period...
    if (!notified) {
      break;
    }

    heartbeat_received = false;
  }

  if (!node_shutdown.load() && state == NodeState::Follower) {
    SwitchStateToCandidate();
  }

  Logger::getLogger().log("(SHUTDOWN) HandleFollowerState exiting for node " +
                          std::to_string(nodeID) + "\n");
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

  while (!stop.load() && !node_shutdown.load()) {
    auto reply = network.sendRequestVote(nodeID, targetID, arg);

    std::lock_guard<std::mutex> lock(mtx);

    // if network.sendRequestVote timed out continue in loop and retry
    if (!(reply.voteGranted || reply.term > currentTerm)) {
      continue;
    }

    // always adopt new term and demote if candidate encounters higher term
    if (reply.term > currentTerm) {
      currentTerm = reply.term;
      SwitchStateToFollower();
    }

    // if vote was granted increment voteState number of "yes" votes
    else if (reply.voteGranted) {
      Logger::getLogger().log("(VOTE) node " + std::to_string(nodeID) +
                              " received yes vote from node " +
                              std::to_string(targetID) + "\n");
      voteState.votesReceived++;
    }

    // network.sendRequestVote must've timed out so continue in loop and retry
    else {
      continue;
    }

    // for the first two cases above voteState cv must be notified
    voteState.cv.notify_one();

    return;
  }
  // maybe voteState.cv.notify_one()
}

void RaftNode::HandleCandidateState() {
  while (!node_shutdown.load()) {
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

    // sleep main candidate thread until one of the following is encountered:
    // 1) NodeShutdown in progress
    // 2) Candidate is demoted back to follower (encounters higher term or
    // AppendEntries from existing leader)
    // 3) Candidate's election receives "yes" votes from a majority of nodes
    std::unique_lock<std::mutex> lock(voteState.mtx);
    bool electionResult = voting_cv.wait_until(
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

    // TODO: follow same pattern as sendAppendEntries where you vote
    // reqVoteThreads ownership outside of HandleCandidateState and into node
    // class itself, this would allow HandleCandidateState to continue once a
    // majority of
    //  votes have been reached (bypassing having to wait for the rest of the
    //  Follower nodes to reply to RequestVote)
    for (auto &t : reqVoteThreads) {
      t.join();
    }

    // clean exit for node shutdown
    if (node_shutdown.load()) {
      Logger::getLogger().log(
          "(SHUTDOWN) HandleCandidateState exiting for node " +
          std::to_string(nodeID) + "\n");
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
    auto reply = network.sendAppendEntries(nodeID, targetID, arg);

    Logger::getLogger().log("(BOMBO) to node " + std::to_string(targetID) +
                            " " + std::to_string(reply.term) + " " +
                            std::to_string(reply.sucess) + " " +
                            std::to_string(reply.hadNetworkFailure) + "\n");
    if (!reply.hadNetworkFailure) {
      // TODO:
      if (!reply.sucess) {
        Logger::getLogger().log("here\n");
        std::lock_guard<std::mutex> lock(peer_replication_mtx);
        peer_replication_threads.emplace_back(&RaftNode::CatchUpLaggingFollower,
                                              this, targetID);
      }

      std::lock_guard<std::mutex> lock(mtx);
      if (reply.term > currentTerm) {
        currentTerm = reply.term;
        stop = true;
        return;
      }
    }

    std::unique_lock<std::mutex> lock(shutdown_mtx);
    shutdown_cv.wait_for(lock, std::chrono::milliseconds(100),
                         [this] { return node_shutdown.load(); });
    lock.unlock();
  }
}

void RaftNode::CatchUpLaggingFollower(size_t targetID) {
  AppendEntriesArgs args;
  {
    std::lock_guard<std::mutex> node_lock(mtx);
    auto prevLogIndex = Log.size() - 1;
    args = AppendEntriesArgs{currentTerm,
                             nodeID,
                             prevLogIndex,
                             Log[prevLogIndex].termReceived,
                             std::vector<LogEntry>{},
                             commitIndex};
  }

  while (!node_shutdown.load() && state == NodeState::Leader) {
    Logger::getLogger().log("(DEBUG) catching up lagging follower\n");
    auto reply = network.sendAppendEntries(nodeID, targetID, args);
    std::lock_guard<std::mutex> lock(mtx);
    if (reply.sucess) {
      auto lastSentIndex = args.prevLogIndex + args.entries.size();
      matchIndex[targetID] = lastSentIndex;
      nextIndex[targetID] = lastSentIndex + 1;

      joinable_replication_threads++;
      peer_replication_cv.notify_one();
      return;
    } else {
      nextIndex[targetID]--;

      args.prevLogIndex--;
      args.prevLogTerm = Log[args.prevLogIndex].termReceived;
      args.entries =
          std::vector<LogEntry>(Log.begin() + args.prevLogIndex + 1, Log.end());
      continue;
    }
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
    Logger::getLogger().log("(SHUTDOWN) HandleLeaderState exiting for node " +
                            std::to_string(nodeID) + "\n");
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

  shutdown_cv.notify_all();         // wakes SendHeartbeatRPCs
  heartbeat_cv.notify_all();        // wakes HandleFollowerState
  voting_cv.notify_all();           // wakes HandleCandidateState
  state_machine_cv.notify_all();    // wakes ApplyToStateMachine
  peer_replication_cv.notify_all(); // wakes CleanUpReplicationThreads
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

// NOTE: consider making commitIndex an atomic rather than acquiring entire
// lock
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
  heartbeat_cv.notify_one();
  print_switch_state_statement(nodeID, state, NodeState::Candidate);
  state = NodeState::Candidate;
}

void RaftNode::SwitchStateToLeader() {
  print_switch_state_statement(nodeID, state, NodeState::Leader);
  state = NodeState::Leader;
}

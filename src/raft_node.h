#pragma once

#include "network.h"
#include "randomizer.h"
#include "rpc.h"
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>

enum class NodeState { Follower = 0, Candidate, Leader };

class RaftNode {
public:
  RaftNode(size_t nodeID, std::random_device &rd, Network &network);

  void StartNode();
  void StopNode();
  void SetPeers(const std::vector<size_t> p);

  NodeState GetState() const;
  void SetState(NodeState new_state);
  uint64_t GetTerm();
  void SetTerm(uint64_t new_term);
  size_t GetCommitIndex();
  std::vector<LogEntry> GetLog();

  // RPC functions
  AppendEntriesReply AppendEntries(const AppendEntriesArgs &args);
  RequestVoteReply RequestVote(const RequestVoteArgs &args);

  // Request function
  bool SendRequest(const std::vector<ServerRequest> &reqs);
  int FetchFromStateMachine(std::string key);

private:
  // Member Variables
  std::atomic<NodeState> state{NodeState::Follower};
  const size_t nodeID;
  std::vector<size_t> peers;
  std::atomic<size_t> currentLeader;

  std::vector<LogEntry> Log;

  std::unordered_map<std::string, int>
      state_machine; // map used to represent node's state machine
  std::mutex state_machine_mtx;

  uint64_t currentTerm = 0; // last term server has seen
  uint32_t votedFor;        // candidateID that received vote in current term
                            // (UINT32_MAX if none)

  size_t commitIndex = 0; // index of highest log entry known to be committed
  size_t lastApplied = 0; // index of highest log entry known to be applied to
                          // local state machine

  std::vector<uint32_t> nextIndex; // index of next log entry to send for each
                                   // of the servers (used by leader)
  std::vector<uint32_t>
      matchIndex; // index of highest log entry known to be replicated for each
                  // server (used by leader)

  std::mutex mtx;

  bool heartbeat_received = false;

  std::condition_variable heartbeat_cv;
  std::condition_variable voting_cv;
  std::condition_variable state_machine_cv;

  std::vector<std::thread> peer_replication_threads;
  std::mutex peer_replication_mtx;
  std::condition_variable peer_replication_cv;
  std::atomic<size_t> joinable_replication_threads;

  std::atomic<bool> node_shutdown{false};
  std::condition_variable shutdown_cv;
  std::mutex shutdown_mtx;

  Randomizer randomizer;

  Network &network;

  // Member functions
  // switch state functions
  void SwitchStateToFollower();
  void SwitchStateToCandidate();
  void SwitchStateToLeader();

  // node state machine functions
  void HandleFollowerState();
  void HandleCandidateState();
  void HandleLeaderState();

  inline void RefreshVolatileLeaderState();

  void SendRequestVoteRPC(size_t targetID, VoteState &voteState,
                          const std::atomic<bool> &stop);

  void CatchUpFollowerLog(AppendEntriesArgs followers_args, size_t targetID);
  void SendAppendEntriesRPC(
      std::shared_ptr<AppendEntriesArgs> arg, size_t targetID,
      std::shared_ptr<std::condition_variable> advance_commit_index_cv);
  void CleanUpReplicationThreads();
  void SendHeartbeatRPCs(size_t targetID, std::atomic<bool> &stop);

  void CatchUpLaggingFollower(size_t targetID);

  bool TryAdvancingCommitIndex();

  void ApplyToStateMachine();
  inline void ApplySingleLogEntry(const LogEntry &entry);

  inline std::vector<LogEntry>
  AppendToLog(const std::vector<ServerRequest> &reqs);
};

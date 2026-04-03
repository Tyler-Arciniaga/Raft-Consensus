#pragma once

#include "randomizer.h"
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
enum NodeState { Follower = 0, Candidate, Leader };

enum ServerAction { Add = 0, Remove };

struct LogEntry {
  ServerAction action;
  std::string key;
  int value;
  uint64_t termReceived;
};

struct AppendEntriesArgs {};
struct AppendEntriesReply {};

struct RequestVoteArgs {
  uint64_t candidate_term;
  size_t candidateID;
  size_t lastLogIndex;  // index of candidate's last log entry
  uint64_t lastLogTerm; // term of candidate's last log entry
};
struct RequestVoteReply {
  uint64_t
      term; // currentTerm (used for candidate to possibly update it's term)
  bool voteGranted;
};

class RaftNode {
public:
  RaftNode(size_t nodeID, std::random_device &rd);

  // RPC functions
  AppendEntriesReply AppendEntries(AppendEntriesArgs args);
  RequestVoteReply RequestVote(RequestVoteArgs args);

private:
  // Member Variables
  NodeState state;
  size_t nodeID;

  std::vector<LogEntry> Log;
  std::unordered_map<std::string, int>
      data; // map used to represent node's state machine

  uint64_t currentTerm; // last term server has seen
  uint32_t votedFor;    // candidateID that received vote in current term
                        // (UINT32_MAX if none)

  size_t commitIndex; // index of highest log entry known to be committed
  size_t lastApplied; // index of highest log entry known to be applied to local
                      // state machine

  std::vector<uint32_t> nextIndex; // index of next log entry to send for each
                                   // of the servers (used by leader)
  std::vector<uint32_t>
      matchIndex; // index of highest log entry known to be replicated for each
                  // server (used by leader)

  Randomizer randomizer;

  std::mutex follower_mtx;
  std::condition_variable cv;
  bool received_heartbeat;

  // Member functions
  // switch state logger functions
  void SwitchStateToFollower();
  void SwitchStateToCandidate();
  void SwitchStateToLeader();

  // node state machine functions
  void HandleFollowerState();
  void HandleCandidateState();
  void WaitForAppendEntries();
};

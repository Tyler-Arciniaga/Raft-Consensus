#pragma once

#include "network.h"
#include "randomizer.h"
#include "rpc.h"
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

class RaftNode {
public:
  RaftNode(size_t nodeID, std::random_device &rd, Network &network);

  // RPC functions
  AppendEntriesReply AppendEntries(AppendEntriesArgs args);
  RequestVoteReply RequestVote(RequestVoteArgs args);

private:
  // Member Variables
  NodeState state;
  size_t nodeID;
  std::vector<size_t> peers{
      0, 1, 2}; // TODO currently using hardcoded number of nodes/peers vector

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

  Network &network;

  // Member functions
  // switch state logger functions
  void SwitchStateToFollower();
  void SwitchStateToCandidate();
  void SwitchStateToLeader();

  // node state machine functions
  void HandleFollowerState();
  void HandleCandidateState();
  void HandleLeaderState();

  void SendRequestVoteRPC(size_t targetID, uint32_t &voteCounter,
                          std::mutex &counterMtx, std::condition_variable &cv);
};

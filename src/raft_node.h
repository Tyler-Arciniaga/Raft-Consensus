#pragma once

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

struct RequestVoteArgs {};
struct RequestVoteReply {};

class RaftNode {
public:
  RaftNode(size_t nodeID, std::random_device rd);

  AppendEntriesReply AppendEntries(AppendEntriesArgs args);
  RequestVoteReply RequestVote(RequestVoteReply args);

private:
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

  std::mt19937 rng; // mt19937 used to determine random numbers for the
                    // randomized election timeout
  std::uniform_int_distribution<int> dist;

  // switch state logger functions
  void SwitchStateToFollower();
  void SwitchStateToCandidate();
  void SwitchStateToLeader();

  // node state machine functions
  void WaitForAppendEntries();
};

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

enum ServerAction { Add = 0, Remove };

struct LogEntry {
  ServerAction action;
  std::string key;
  int value;
  uint64_t termReceived;
};

struct AppendEntriesArgs {
  uint64_t leader_term;
  size_t leaderID;
  size_t prevLogIndex; // index of log entry immediately preceding new ones
  uint64_t prevLogTerm;
  std::vector<LogEntry> entries; // log entries to store (empty for heartbeat;
                                 // may send more than one for efficiency)
  size_t leaderCommitIndex;
};

struct AppendEntriesReply {
  uint64_t term;
  bool sucesss; // true if follower contained entry matching prevLogIndex and
                // prevLogTerm
};

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

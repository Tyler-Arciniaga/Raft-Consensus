#pragma once

#include <cstddef>
#include <cstdint>

struct AppendEntriesArgs {};
struct AppendEntriesReply {};

struct RequestVoteArgs {
  uint64_t candidate_term;
  std::size_t candidateID;
  size_t lastLogIndex;  // index of candidate's last log entry
  uint64_t lastLogTerm; // term of candidate's last log entry
};
struct RequestVoteReply {
  uint64_t
      term; // currentTerm (used for candidate to possibly update it's term)
  bool voteGranted;
};

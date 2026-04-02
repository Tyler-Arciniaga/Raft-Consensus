#include <cstdint>
#include <iostream>
#include <vector>

enum NodeState { Follower = 0, Candidate, Leader };

enum ServerAction { Add = 0, Remove };

struct LogEntry {
  ServerAction action;
  std::string key;
  int value;
};

class RaftNode {
public:
private:
  std::vector<LogEntry> Log;

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
};

int main(int argc, char *argv[]) {
  std::cout << "Hello Raft!\n";
  return 0;
}

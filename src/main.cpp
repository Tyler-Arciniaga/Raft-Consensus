#include "raft_node.h"
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char *argv[]) {
  std::cout << "Hello Raft!\n";

  std::random_device rd;
  for (size_t i = 0; i < 5; i++) {
    RaftNode(i, rd);
  }

  std::this_thread::sleep_for(std::chrono::seconds(2));
  return 0;
}

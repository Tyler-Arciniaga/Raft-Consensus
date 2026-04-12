#include "network.h"
#include "raft_node.h"
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char *argv[]) {
  std::cout << "Hello Raft!\n";

  SimulatedNetwork simNetwork;
  std::random_device rd;
  std::vector<RaftNode> nodes;
  nodes.reserve(
      5); // reserve vector space upfront to ensure no vector reallocation
          // (which would dangle the pointers in simNetwork)

  for (size_t i = 0; i < 5; i++) {
    nodes.emplace_back(RaftNode(i, rd, simNetwork));
    simNetwork.AddNode(&nodes.back());
  }

  for (size_t i = 0; i < nodes.size(); i++) {
    std::thread t(&RaftNode::StartNode, nodes[i]);
    t.detach();
  }

  std::this_thread::sleep_for(std::chrono::seconds(30));
  return 0;
}

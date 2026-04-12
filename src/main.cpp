#include "network.h"
#include "raft_node.h"
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

int main(int argc, char *argv[]) {
  std::cout << "Hello Raft!\n";

  SimulatedNetwork simNetwork;
  std::random_device rd;
  std::vector<std::unique_ptr<RaftNode>> nodes;

  for (size_t i = 0; i < 5; i++) {
    nodes.emplace_back(std::make_unique<RaftNode>(i, rd, simNetwork));
    simNetwork.AddNode(nodes.back().get());
  }

  for (size_t i = 0; i < nodes.size(); i++) {
    std::thread t(&RaftNode::StartNode, nodes[i].get());
    t.detach();
  }

  std::this_thread::sleep_for(std::chrono::seconds(30));
  return 0;
}

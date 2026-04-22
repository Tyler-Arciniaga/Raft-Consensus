for i in $(seq 1 100); do
  ./build/raft_tests --gtest_filter=ElectionTest.* || break
  echo "pass $i"
done

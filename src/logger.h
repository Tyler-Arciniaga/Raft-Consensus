#pragma once

#include <iostream>
#include <mutex>
class Logger {
public:
  static Logger &getLogger() {
    static Logger instance;
    return instance;
  }

  void log(const std::string &message) {
    std::lock_guard<std::mutex> lock(mtx);

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    std::cout << "[" << ms << "] " << message;
  }

  // prevent copying
  Logger(const Logger &) = delete;
  Logger &operator=(const Logger &) = delete;

private:
  Logger() = default;
  std::mutex mtx;
};

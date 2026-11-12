#include "core/blocking.h"

#include <thread>

namespace thelife {

trantor::ConcurrentTaskQueue& blockingPool() {
  static trantor::ConcurrentTaskQueue pool(std::max(2u, std::thread::hardware_concurrency() / 2), "blocking");
  return pool;
}

}  // namespace thelife

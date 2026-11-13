#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "llm/classifier.h"

namespace thelife::llm {

// Blocking HTTP POST used by providers. Runs on the gateway's worker threads, never on an event loop.
class Transport {
 public:
  virtual ~Transport() = default;
  // onChunk receives body bytes as they arrive for a 2xx response (streaming); the full body is still returned
  // when onChunk is empty. timeoutSec bounds the whole exchange; idleTimeoutSec bounds silence between bytes.
  virtual HttpResult post(const std::string& url, const std::map<std::string, std::string>& headers,
                          const std::string& body, int timeoutSec, int idleTimeoutSec,
                          const std::function<void(std::string_view)>& onChunk) = 0;
};

std::unique_ptr<Transport> makeCurlTransport();

}  // namespace thelife::llm

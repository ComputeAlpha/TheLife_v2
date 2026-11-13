#pragma once

#include <drogon/utils/coroutine.h>

#include <optional>
#include <string>
#include <vector>

// Client of thelife-embedder (backend_lld.md §2.1) over its Unix socket. Optional by design: when it is not
// configured or not reachable, embed() returns nullopt and callers fall back to keyword search; after a failure
// it is not tried again for 30 s, so a missing embedder never slows a chat turn down.
namespace thelife::memory {

struct Embeddings {
  std::string model, modelVersion;
  std::vector<std::vector<float>> vectors;
};

void initEmbedder();      // THELIFE_EMBEDDER_SOCKET; unset → disabled
bool embedderConfigured();
// Up to 8 texts per call (the embedder's batch limit); longer lists are split.
drogon::Task<std::optional<Embeddings>> embed(std::vector<std::string> texts);

// pgvector text literal: [0.1,-0.2,...]
std::string vectorLiteral(const std::vector<float>& v);

}  // namespace thelife::memory

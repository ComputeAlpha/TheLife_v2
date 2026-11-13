#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace thelife::embedder {

struct ModelOptions {
  std::string path;              // fp16 attention-fused bge-m3 (spikes/embed/convert_fp16.py)
  bool cpu = false;              // development machines without a GPU; logged loudly
  std::size_t gpuMemLimit = 2560ull * 1024 * 1024;
  int device = 0;
};

// One ONNX Runtime session. Refuses to start on the CUDA provider's absence instead of silently falling back to
// the CPU (the trap found in the Stage 0 spike). Not thread-safe: the server serialises calls.
class Model {
 public:
  explicit Model(const ModelOptions& opts);
  ~Model();
  // L2-normalised sentence embeddings (CLS pooling is in the graph's sentence_embedding output).
  std::vector<std::vector<float>> embed(const std::vector<std::vector<int64_t>>& batch);
  const std::string& provider() const;
  int dim() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace thelife::embedder

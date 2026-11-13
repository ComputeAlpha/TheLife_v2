#include "model.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

#include "tokenizer.h"

namespace thelife::embedder {

struct Model::Impl {
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "thelife-embedder"};
  std::unique_ptr<Ort::Session> session;
  std::string provider;
  std::string output = "sentence_embedding";
  ONNXTensorElementDataType outType = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
  int dim = 0;
};

Model::Model(const ModelOptions& opts) : impl_(std::make_unique<Impl>()) {
  Ort::SessionOptions so;
  so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  so.SetIntraOpNumThreads(2);
  if (opts.cpu) {
    std::cerr << "thelife-embedder: WARNING running on the CPU (--cpu); 15-30x slower than the GPU\n";
    impl_->provider = "cpu";
  } else {
    const auto available = Ort::GetAvailableProviders();
    if (std::find(available.begin(), available.end(), "CUDAExecutionProvider") == available.end()) {
      throw std::runtime_error("this ONNX Runtime build has no CUDAExecutionProvider");
    }
    OrtCUDAProviderOptions cuda{};
    cuda.device_id = opts.device;
    cuda.gpu_mem_limit = opts.gpuMemLimit;
    cuda.arena_extend_strategy = 1;  // kSameAsRequested: grow only as needed (spike memory figures)
    cuda.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchHeuristic;
    // Throws when the CUDA provider library or its CUDA 12 dependencies cannot be loaded: no silent CPU fallback.
    so.AppendExecutionProvider_CUDA(cuda);
    impl_->provider = "cuda";
  }
  impl_->session = std::make_unique<Ort::Session>(impl_->env, opts.path.c_str(), so);

  Ort::AllocatorWithDefaultOptions alloc;
  bool found = false;
  for (std::size_t i = 0; i < impl_->session->GetOutputCount(); ++i) {
    if (impl_->session->GetOutputNameAllocated(i, alloc).get() != impl_->output) continue;
    const auto info = impl_->session->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo();
    impl_->outType = info.GetElementType();
    const auto shape = info.GetShape();
    impl_->dim = shape.size() == 2 && shape[1] > 0 ? static_cast<int>(shape[1]) : 1024;
    found = true;
  }
  if (!found) throw std::runtime_error("model has no sentence_embedding output");
  // Warm-up: the first CUDA run builds kernels; do it before serving.
  embed({{Tokenizer::kBos, 33600, Tokenizer::kEos}});
}

Model::~Model() = default;

const std::string& Model::provider() const { return impl_->provider; }
int Model::dim() const { return impl_->dim; }

std::vector<std::vector<float>> Model::embed(const std::vector<std::vector<int64_t>>& batch) {
  if (batch.empty()) return {};
  std::size_t len = 0;
  for (const auto& b : batch) len = std::max(len, b.size());
  const auto n = batch.size();
  std::vector<int64_t> ids(n * len, Tokenizer::kPad), mask(n * len, 0);
  for (std::size_t i = 0; i < n; ++i) {
    std::copy(batch[i].begin(), batch[i].end(), ids.begin() + static_cast<std::ptrdiff_t>(i * len));
    std::fill_n(mask.begin() + static_cast<std::ptrdiff_t>(i * len), batch[i].size(), 1);
  }
  const std::array<int64_t, 2> shape{static_cast<int64_t>(n), static_cast<int64_t>(len)};
  auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::array<Ort::Value, 2> inputs{
      Ort::Value::CreateTensor<int64_t>(mem, ids.data(), ids.size(), shape.data(), shape.size()),
      Ort::Value::CreateTensor<int64_t>(mem, mask.data(), mask.size(), shape.data(), shape.size())};
  const char* inNames[] = {"input_ids", "attention_mask"};
  const char* outNames[] = {impl_->output.c_str()};
  auto out = impl_->session->Run(Ort::RunOptions{nullptr}, inNames, inputs.data(), 2, outNames, 1);
  const auto info = out[0].GetTensorTypeAndShapeInfo();
  const auto dims = info.GetShape();
  const std::size_t d = dims.size() == 2 ? static_cast<std::size_t>(dims[1]) : static_cast<std::size_t>(impl_->dim);
  std::vector<std::vector<float>> vecs(n, std::vector<float>(d));
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j < d; ++j) {
      vecs[i][j] = impl_->outType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16
                       ? out[0].GetTensorData<Ort::Float16_t>()[i * d + j].ToFloat()
                       : out[0].GetTensorData<float>()[i * d + j];
    }
    double norm = 0;
    for (const float v : vecs[i]) norm += static_cast<double>(v) * v;
    norm = std::sqrt(norm);
    if (norm > 0) {
      for (auto& v : vecs[i]) v = static_cast<float>(v / norm);
    }
  }
  return vecs;
}

}  // namespace thelife::embedder

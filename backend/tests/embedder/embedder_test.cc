// thelife-embedder against the upstream reference (backend/tests/fixtures/embed/make_reference.py): tokenizer ids
// must match the Hugging Face tokenizer exactly; fp16 CUDA vectors must match fp32 CPU ones (cosine ≥ 0.999).
// Needs the local model files (git-ignored): THELIFE_EMBED_SPM / THELIFE_EMBED_MODEL, defaulting to the spike's
// download under spikes/embed/models. Skipped when they are absent (CI).
#include <gtest/gtest.h>
#include <json/json.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#ifndef THELIFE_NO_ORT
#include "model.h"
#endif
#include "tokenizer.h"

using namespace thelife::embedder;

namespace {

std::string pathFrom(const char* var, const std::string& fallback) {
  const char* v = std::getenv(var);
  const std::string p = v && *v ? v : std::string(THELIFE_REPO_ROOT) + "/" + fallback;
  return std::filesystem::exists(p) ? p : "";
}
std::string spm() { return pathFrom("THELIFE_EMBED_SPM", "spikes/embed/models/bge-m3/onnx/sentencepiece.bpe.model"); }
std::string model() { return pathFrom("THELIFE_EMBED_MODEL", "spikes/embed/models/bge-m3-fp16/model.onnx"); }

Json::Value reference() {
  std::ifstream in(std::string(THELIFE_REPO_ROOT) + "/backend/tests/fixtures/embed/reference.json");
  Json::Value v;
  in >> v;
  return v["items"];
}

double cosine(const std::vector<float>& a, const Json::Value& b) {
  double dot = 0, nb = 0;
  for (Json::ArrayIndex i = 0; i < b.size(); ++i) {
    dot += a[i] * b[i].asDouble();
    nb += b[i].asDouble() * b[i].asDouble();
  }
  return dot / std::sqrt(nb);  // a is unit length
}

}  // namespace

TEST(Tokenizer, NormalizesWhitespace) {
  EXPECT_EQ(Tokenizer::normalize("  a\t\tb \n c  "), "a b c");
  EXPECT_EQ(Tokenizer::normalize(""), "");
}

TEST(Tokenizer, MatchesTheReferenceTokenizer) {
  if (spm().empty()) GTEST_SKIP() << "sentencepiece model not present";
  Tokenizer tok(spm());
  for (const auto& item : reference()) {
    const auto ids = tok.encode(item["text"].asString(), 8192);
    std::vector<int64_t> want;
    for (const auto& x : item["ids"]) want.push_back(x.asInt64());
    EXPECT_EQ(ids, want) << item["text"].asString();
  }
  const auto cut = tok.encode(std::string(5000, 'a') + " word", 16);
  ASSERT_EQ(cut.size(), 16u);
  EXPECT_EQ(cut.front(), Tokenizer::kBos);
  EXPECT_EQ(cut.back(), Tokenizer::kEos);
}

#ifndef THELIFE_NO_ORT
TEST(Model, Fp16CudaMatchesFp32ReferenceAndBatchingDoesNotChangeVectors) {
  if (spm().empty() || model().empty()) GTEST_SKIP() << "model files not present";
  Tokenizer tok(spm());
  ModelOptions o;
  o.path = model();
  std::unique_ptr<Model> m;
  try {
    m = std::make_unique<Model>(o);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "no usable CUDA device: " << e.what();
  }
  EXPECT_EQ(m->provider(), "cuda");
  EXPECT_EQ(m->dim(), 1024);
  const auto ref = reference();
  std::vector<std::vector<int64_t>> batch;
  for (const auto& item : ref) batch.push_back(tok.encode(item["text"].asString(), 512));
  const auto together = m->embed(batch);
  for (Json::ArrayIndex i = 0; i < ref.size(); ++i) {
    EXPECT_GE(cosine(together[i], ref[i]["vector"]), 0.999) << ref[i]["text"].asString();
    const auto alone = m->embed({batch[i]});  // padding in a batch must not leak into a vector
    double dot = 0;
    for (std::size_t j = 0; j < alone[0].size(); ++j) dot += alone[0][j] * together[i][j];
    EXPECT_GE(dot, 0.999) << ref[i]["text"].asString();
  }
}
#endif

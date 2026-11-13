#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sentencepiece {
class SentencePieceProcessor;
}

namespace thelife::embedder {

// bge-m3 / XLM-RoBERTa tokenization (spikes/embed/REPORT.md): sentencepiece ids shifted by the fairseq offset
// (id + 1, sentencepiece's <unk> 0 → 3), wrapped in <s>=0 … </s>=2, padding 1. Whitespace is trimmed and collapsed
// first, which makes it match the reference HF tokenizer on the spike corpus (15/15).
class Tokenizer {
 public:
  static constexpr int64_t kBos = 0, kPad = 1, kEos = 2, kUnk = 3;

  explicit Tokenizer(const std::string& modelPath);
  ~Tokenizer();

  // At most maxTokens ids including <s> and </s>.
  std::vector<int64_t> encode(std::string_view text, std::size_t maxTokens) const;
  static std::string normalize(std::string_view text);

 private:
  std::unique_ptr<sentencepiece::SentencePieceProcessor> sp_;
};

}  // namespace thelife::embedder

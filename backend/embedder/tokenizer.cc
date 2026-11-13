#include "tokenizer.h"

#include <sentencepiece_processor.h>

#include <stdexcept>

namespace thelife::embedder {

Tokenizer::Tokenizer(const std::string& modelPath) : sp_(std::make_unique<sentencepiece::SentencePieceProcessor>()) {
  const auto status = sp_->Load(modelPath);
  if (!status.ok()) throw std::runtime_error("sentencepiece model: " + status.ToString());
}

Tokenizer::~Tokenizer() = default;

std::string Tokenizer::normalize(std::string_view text) {
  std::string out;
  bool space = false;
  for (const char c : text) {
    const bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    if (ws) {
      space = !out.empty();
      continue;
    }
    if (space) out.push_back(' ');
    space = false;
    out.push_back(c);
  }
  return out;
}

std::vector<int64_t> Tokenizer::encode(std::string_view text, std::size_t maxTokens) const {
  if (maxTokens < 2) throw std::invalid_argument("maxTokens must be at least 2");
  std::vector<int> pieces;
  const auto status = sp_->Encode(normalize(text), &pieces);
  if (!status.ok()) throw std::runtime_error("sentencepiece encode: " + status.ToString());
  std::vector<int64_t> ids;
  ids.reserve(std::min(pieces.size() + 2, maxTokens));
  ids.push_back(kBos);
  for (const int p : pieces) {
    if (ids.size() + 1 >= maxTokens) break;
    ids.push_back(p == 0 ? kUnk : static_cast<int64_t>(p) + 1);
  }
  ids.push_back(kEos);
  return ids;
}

}  // namespace thelife::embedder

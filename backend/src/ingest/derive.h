#pragma once

#include <json/value.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ingest/rules.h"

// Focus spans and process detections (backend_lld.md §7.4). Pure functions over decoded observations; the
// ingest.derive job (ingest/jobs.cc) loads them, calls these and writes detections.
namespace thelife::ingest {

struct Obs {
  int64_t at = 0;       // captured_at, epoch seconds
  int64_t eventAt = 0;  // idle: when it really began (payload "at", backdated to the last input); 0 = `at`
  std::string kind;
  Json::Value payload;
};

struct Span {
  int64_t from = 0, to = 0;
  Window window;
};

struct SpanParams {
  int64_t silenceSec = 300;   // no observation for this long: the collector was off, the span ends
  int64_t openMaxSec = 1800;  // an open span this long is closed at the last observation and continues
};

struct SpanResult {
  std::vector<Span> spans;  // closed spans, in time order
  int64_t until = 0;        // everything up to here is final; the next run starts here
};

// `current`: the window focused at `from` (the last window observation at or before it), `idle`: whether the
// user was idle then. `obs`: observations after `from`, in time order.
SpanResult focusSpans(std::optional<Window> current, bool idle, int64_t from, const std::vector<Obs>& obs,
                      const SpanParams& p = {});

Window windowOf(const Json::Value& payload);

struct Block {
  int64_t from = 0, to = 0;
  Classification c;
  std::map<std::string, int64_t> subjects;  // subject → seconds
  std::string wmClass, exe;
  int spans = 0;
};

struct BlockParams {
  int64_t minSpanSec = 20;   // shorter glances are ignored
  int64_t mergeGapSec = 60;  // same app/category/area across a gap this short is one block
  int64_t minBlockSec = 120; // shorter blocks make no detection
};

std::vector<Block> blocks(const std::vector<Span>& spans, const std::function<Classification(const Window&)>& classify,
                          const BlockParams& p = {});

// Detection fields for a block: summary ("VS Code: TheLife_v2") and evidence (app, top subjects with minutes,
// window class, exe).
std::string summaryOf(const Block& b);
Json::Value evidenceOf(const Block& b);

}  // namespace thelife::ingest

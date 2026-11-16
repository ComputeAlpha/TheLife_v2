#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <string>

#include "core/secure_buffer.h"

// Vision pipeline (backend_lld.md §7.4): an uploaded capture waits in a bounded in-memory queue (never on disk),
// one worker sends the images and context to the vision model, writes one detection (source=vision) and drops the
// images, zeroed, on every path. A failed call is not retried: the next capture replaces it.
namespace thelife::vision {

struct Capture {
  std::string tenantId, userId, deviceId, captureId;
  Json::Value context;    // CaptureContext (ingest.yaml)
  Attachments images;     // JPEG, one per monitor
  int windowSec = 300;    // the period the detection covers: up to the capture time
};

// Queues a capture; false when all slots (THELIFE_VISION_QUEUE, default 8, queued plus in flight) are taken.
bool submit(Capture c);
// Slots in use, and how many there are.
int inUse();
int capacity();

// Runs one capture now (the worker's body; also used by tests). Returns the detection id, or "" when the model
// could not be used.
drogon::Task<std::string> analyze(Capture c);

}  // namespace thelife::vision

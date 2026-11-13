#pragma once

namespace thelife::proactive {

// Registers proactive.tick (queued per user by ops.enqueue_proactive_ticks during housekeeping).
void registerTick();

}  // namespace thelife::proactive

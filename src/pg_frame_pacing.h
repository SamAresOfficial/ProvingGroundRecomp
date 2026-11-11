#pragma once

#include <cstdint>

// Frame rate of the game and the optional host-side limit, measured and applied at the game's
// swap (see pg_frame_pacing.cpp).
namespace pg {

// Frames the game presented per second, averaged over the last half second; 0 before the first.
double PresentedFps();

// Applies the frame_rate_limit and present_sync settings: 30 restores the game's own vsync
// pacing, any other value runs the game uncapped and, unless it is 0, holds it at that many
// frames per second; present_sync tells the presenter when to present with vsync.
void ApplyFrameRateLimit();

// The engine's game time in milliseconds: the sum of its frame times over the main loop's frames.
// It stops while the main loop does (loading), so input scripts timed by it replay the same at
// any frame rate.
uint32_t EngineTimeMillis();

// Starts or pauses the pacing_trace_csv trace (the measurement window of an input replay).
void SetPacingTraceActive(bool active);

}  // namespace pg

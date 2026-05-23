// stream.hpp — Double-buffered ADPCM streaming engine for live render output.
//
// Two ADPCM buffers live in SPU RAM. The SPU plays buffer A, then jumps
// (via the LOOP_END+LOOP_ON flag in A's last block) to buffer B, plays it,
// and jumps back to A. While A is playing, the CPU refills B with the next
// chunk of freshly-rendered samples, and vice versa.
//
// Status of the active buffer is polled via the SPU IRQ flag (set when the
// SPU reads from SPU_RAM_IRQ_ADDR). No interrupt handler is wired yet —
// AcidRom::frame() calls tick() once per video frame to refill on time.
//
// Phase 2 (M17): silent stream only. Phase 3+ will route the ACB voice
// mix into render().

#pragma once
#include <cstdint>

namespace acid::audio::stream {

// 28 PCM samples per ADPCM block; ADPCM block = 16 bytes.
constexpr int SAMPLES_PER_BLOCK = 28;
constexpr int BYTES_PER_BLOCK   = 16;

// One buffer = N blocks. 64 blocks * 28 samples = 1792 samples per buffer.
// At BASE_SAMPLE_RATE 44100 Hz that's ~40 ms; one NTSC video frame is ~17ms,
// so each video frame the CPU has 2 buffers worth of time (~80ms) to make
// the next refill before underrun.
constexpr int BLOCKS_PER_BUFFER = 64;
constexpr int BYTES_PER_BUFFER  = BLOCKS_PER_BUFFER * BYTES_PER_BLOCK;  // 1024
constexpr int SAMPLES_PER_BUFFER = BLOCKS_PER_BUFFER * SAMPLES_PER_BLOCK;

// SPU RAM placement: streaming buffers live well above the static voice
// samples (which are uploaded starting at 0x1100 in main.cpp::prepare).
// 0x10000 leaves ~60 KB of headroom for sample voices before we collide.
constexpr uint32_t SPU_BUFFER_A_ADDR = 0x10000;
constexpr uint32_t SPU_BUFFER_B_ADDR = SPU_BUFFER_A_ADDR + BYTES_PER_BUFFER;

// We hijack the highest channel for the live mix. The sequencer's 13
// sample voices are 0..12, so 23 is free.
constexpr uint8_t  STREAM_CHANNEL = 23;

// Initialize SPU streaming. Uploads two silent ADPCM buffers, configures
// the LOOP flags so the SPU chains A→B→A→B forever, and kicks playback
// on STREAM_CHANNEL. Safe to call from prepare() after SPU::initialize().
void initialize();

// Called once per video frame. If the SPU has finished one buffer and
// crossed the IRQ marker, render the next buffer and DMA it in.
//
// Phase 2 fills the inactive buffer with silence (a zeroed ADPCM block).
// Phase 3+ will replace this with the live mix from acid::engine.
void tick();

}  // namespace acid::audio::stream

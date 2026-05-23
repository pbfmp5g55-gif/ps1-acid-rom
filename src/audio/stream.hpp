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

// Revert to 64 blocks (40 ms). 26-block (1-frame) sizing caused total
// silence — likely the SPU couldn't latch a LOOP_END+LOOP_ON before the
// CPU's next refill overran the same block.
constexpr int BLOCKS_PER_BUFFER = 64;
constexpr int BYTES_PER_BUFFER  = BLOCKS_PER_BUFFER * BYTES_PER_BLOCK;  // 1024
constexpr int SAMPLES_PER_BUFFER = BLOCKS_PER_BUFFER * SAMPLES_PER_BLOCK;

// SPU RAM placement: streaming buffers MUST sit above the static voice
// samples. The 13 voice ADPCM samples (BD/SD/.../SQ2) total ~160 KB
// uploaded starting at 0x1100, ending around 0x29000. We park the stream
// at 0x30000 (192 KB) with ~250 KB of headroom before the reverb area
// (0x70000). Earlier placement at 0x10000 was inside the voice sample
// range and silently overwrote our stream buffers as voices DMA'd in
// (= total silence on channel 23, even with sine fill).
constexpr uint32_t SPU_BUFFER_A_ADDR = 0x30000;
constexpr uint32_t SPU_BUFFER_B_ADDR = SPU_BUFFER_A_ADDR + BYTES_PER_BUFFER;

// We hijack a channel above the sequencer's 13 sample voices (0..12).
// Started on 23 (top channel) but pcsx-redux seems unhappy playing
// looped ADPCM there — moved to 16 to see if a mid-range channel works.
constexpr uint8_t  STREAM_CHANNEL = 16;

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

// Pick what the next refill writes into the buffer:
//   SILENT — zeros (default after Phase 3+)
//   SINE   — 1 kHz test tone for streaming-chain verification
//   ACB    — live mix of the integrated ACB voices
enum class FillMode : uint8_t { Silent, Sine, Acb };
void set_fill_mode(FillMode m);

// ---- Phase 3+ wiring ------------------------------------------------------
// The streaming engine owns one ACB voice instance per acid sample slot.
// Voice indices follow the SequencerScene voice table:
//   7  = TB-303 STG1 SAW
//   8  = TB-303 STG1 SQR
//   11 = TB-303 STG2 SAW (sample-only until Phase 4b)
//   12 = TB-303 STG2 SQR (sample-only until Phase 4b)
//
// noteOffset: semitones above/below the base note (A2 ≈ 110 Hz).
void trigger_acb_voice(int voiceIdx, int noteOffset, bool slide, bool accent);

// Push the SequencerScene knob bank (0..255 uint8_t) down to the voice.
// Called whenever knobs change so the next note picks up the new params.
void set_acb_voice_knobs(int voiceIdx,
                         int cutoff8, int reso8, int envMod8,
                         int decay8, int accent8);

// Drum-flavoured knob setter for the 808/909 voices. `pit` is signed
// (−12..+12 from the PIT knob) — drum voices interpret it as a tuning
// nudge rather than a per-step note offset.
void set_acb_drum_knobs(int voiceIdx, int pit, int tone8, int decay8, int level8);

// Returns true if `voiceIdx` is currently routed through the live ACB
// engine (rather than the SPU sample path). The sequencer uses this to
// decide whether to bypass triggerAcidVoice().
bool is_voice_live(int voiceIdx);

}  // namespace acid::audio::stream

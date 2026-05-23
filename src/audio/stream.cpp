// stream.cpp — see stream.hpp.

#include "audio/stream.hpp"
#include "audio/adpcm_encode.hpp"

#include "psyqo/spu.hh"

#include "common/hardware/spu.h"

namespace acid::audio::stream {

namespace {

// SPU IRQ address register (not in psyqo's wrapper). Setting bit 6 of
// SPU_CTRL enables IRQ; when the SPU reads the address stored at
// SPU_RAM_IRQ_ADDR it raises CPU IRQ 9 and latches bit 6 of SPU_STATUS.
constexpr uint32_t SPU_RAM_IRQ_ADDR_OFS = 0x1f801da4;
inline volatile uint16_t &SPU_RAM_IRQ_ADDR() {
    return *reinterpret_cast<volatile uint16_t *>(SPU_RAM_IRQ_ADDR_OFS);
}

// Active buffer index: 0 = SPU is playing A, refill B next; 1 = vice versa.
int  g_activeBuf = 0;
bool g_initialized = false;

// Local PCM scratch — one buffer's worth of 16-bit samples. We encode in
// place to ADPCM in `g_encodedBuf` and DMA that into SPU RAM.
int16_t g_pcmBuf[SAMPLES_PER_BUFFER];
alignas(4) uint8_t g_encodedBuf[BYTES_PER_BUFFER];

// Encode `g_pcmBuf` to `g_encodedBuf`, setting the LOOP flags so the last
// block jumps to the *other* buffer.
void encode_current_buffer(uint32_t selfAddr, uint32_t nextAddr) {
    (void)selfAddr;
    (void)nextAddr;
    for (int b = 0; b < BLOCKS_PER_BUFFER; ++b) {
        uint8_t flags = 0;
        if (b == BLOCKS_PER_BUFFER - 1) {
            // Last block: tell SPU "end of block, jump to repeat addr".
            // The repeat addr is set on the SPU voice register, not here.
            flags = adpcm::FLAG_LOOP_END | adpcm::FLAG_LOOP_ON;
        }
        adpcm::encode_block(&g_pcmBuf[b * SAMPLES_PER_BLOCK],
                            flags,
                            &g_encodedBuf[b * BYTES_PER_BLOCK]);
    }
}

void fill_silence() {
    for (int i = 0; i < SAMPLES_PER_BUFFER; ++i) g_pcmBuf[i] = 0;
}

void dma_to_spu(uint32_t spuAddr) {
    psyqo::SPU::dmaWrite(spuAddr, g_encodedBuf,
                         static_cast<uint16_t>(BYTES_PER_BUFFER), 16);
}

}  // namespace

void initialize() {
    if (g_initialized) return;

    // Pre-load both buffers with silence so the SPU has something safe to
    // play immediately and the chain A→B→A keeps running.
    fill_silence();
    encode_current_buffer(SPU_BUFFER_A_ADDR, SPU_BUFFER_B_ADDR);
    dma_to_spu(SPU_BUFFER_A_ADDR);
    encode_current_buffer(SPU_BUFFER_B_ADDR, SPU_BUFFER_A_ADDR);
    dma_to_spu(SPU_BUFFER_B_ADDR);

    // Configure SPU IRQ to fire at the start of buffer B. When the SPU
    // crosses that address, bit 6 of SPU_STATUS latches and we know A is
    // done playing → refill A next. (We flip the IRQ addr on each tick.)
    SPU_CTRL = SPU_CTRL | (1u << 6);          // IRQ enable
    SPU_RAM_IRQ_ADDR() = static_cast<uint16_t>(SPU_BUFFER_B_ADDR / 8);

    // Kick playback on the stream channel. Sample rate is locked to
    // BASE_SAMPLE_RATE (44100 Hz native) — anything else would resample
    // and that's the host renderer's job, not the SPU's.
    psyqo::SPU::ChannelPlaybackConfig cfg{};
    cfg.sampleRate.value = 0x1000;            // 1.0 = 44100 Hz native
    cfg.volumeLeft  = 0x3fff;
    cfg.volumeRight = 0x3fff;
    cfg.adsr        = 0x1fffc0ff;             // sustain-forever envelope
    psyqo::SPU::playADPCM(STREAM_CHANNEL, static_cast<uint16_t>(SPU_BUFFER_A_ADDR),
                          cfg, true);
    // Override repeat addr to B so A's LOOP_END jumps to B (not back to A).
    SPU_VOICES[STREAM_CHANNEL].sampleRepeatAddr =
        static_cast<uint16_t>(SPU_BUFFER_B_ADDR / 8);

    g_activeBuf = 0;
    g_initialized = true;
}

void tick() {
    if (!g_initialized) return;

    // Check if SPU crossed the IRQ marker since last tick.
    if ((SPU_STATUS & (1u << 6)) == 0) return;

    // Acknowledge: clear IRQ enable then re-enable. This is the
    // documented way to drop the latched status bit without writing it
    // directly (which is read-only on PSX).
    SPU_CTRL = SPU_CTRL & ~(1u << 6);
    SPU_CTRL = SPU_CTRL | (1u << 6);

    // Refill the buffer the SPU just finished. After IRQ at B's start,
    // A is the one freshly-consumed → refill A and set IRQ to A's start
    // so the next firing tells us B is consumed.
    if (g_activeBuf == 0) {
        // We were playing A, IRQ fired at B start → refill A.
        fill_silence();  // Phase 2: silence only.
        encode_current_buffer(SPU_BUFFER_A_ADDR, SPU_BUFFER_B_ADDR);
        // Update repeat addr so the next loop_end in B jumps back to A.
        SPU_VOICES[STREAM_CHANNEL].sampleRepeatAddr =
            static_cast<uint16_t>(SPU_BUFFER_A_ADDR / 8);
        dma_to_spu(SPU_BUFFER_A_ADDR);
        SPU_RAM_IRQ_ADDR() = static_cast<uint16_t>(SPU_BUFFER_A_ADDR / 8);
        g_activeBuf = 1;
    } else {
        fill_silence();
        encode_current_buffer(SPU_BUFFER_B_ADDR, SPU_BUFFER_A_ADDR);
        SPU_VOICES[STREAM_CHANNEL].sampleRepeatAddr =
            static_cast<uint16_t>(SPU_BUFFER_B_ADDR / 8);
        dma_to_spu(SPU_BUFFER_B_ADDR);
        SPU_RAM_IRQ_ADDR() = static_cast<uint16_t>(SPU_BUFFER_B_ADDR / 8);
        g_activeBuf = 0;
    }
}

}  // namespace acid::audio::stream

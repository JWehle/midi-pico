#include "i2s_audio.h"

#include <math.h>
#include <stdint.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"

namespace {
constexpr uint kDataPin = 22;
constexpr uint kBitClockPin = 26;
constexpr uint kWordSelectPin = 27;
constexpr uint kAudioSampleRate = 44100;
constexpr uint kFramesPerBuffer = 256;
constexpr uint kVoiceCount = 8;
constexpr uint16_t kMaxGain = 32767;
constexpr uint16_t kAttackStep = 1024;
constexpr uint16_t kReleaseStep = 128;
constexpr uint kI2sProgramLength = 8;
constexpr uint64_t kPhaseScale = uint64_t{1} << 32;

PIO const kPio = pio0;
uint kPioOffset;
uint kDmaChannel;
uint kActiveStateMachine;
volatile uint32_t dmaBufferRefills;
bool audioInitialized;
int16_t sineTable[256];
uint32_t notePhaseIncrements[128];
alignas(4) uint32_t audioBuffer[kFramesPerBuffer];

struct Voice {
    uint32_t phase;
    uint32_t phaseIncrement;
    uint16_t gain;
    uint16_t targetGain;
    uint8_t channel;
    uint8_t note;
    bool assigned;
    bool keyDown;
};

Voice voices[kVoiceCount] = {};

uint16_t i2sInstructions[kI2sProgramLength] = {
    static_cast<uint16_t>(pio_encode_out(pio_pins, 1) | pio_encode_sideset(2, 0b10)),
    static_cast<uint16_t>(pio_encode_jmp_x_dec(0) | pio_encode_sideset(2, 0b11)),
    static_cast<uint16_t>(pio_encode_out(pio_pins, 1) | pio_encode_sideset(2, 0b00)),
    static_cast<uint16_t>(pio_encode_set(pio_x, 14) | pio_encode_sideset(2, 0b01)),
    static_cast<uint16_t>(pio_encode_out(pio_pins, 1) | pio_encode_sideset(2, 0b00)),
    static_cast<uint16_t>(pio_encode_jmp_x_dec(4) | pio_encode_sideset(2, 0b01)),
    static_cast<uint16_t>(pio_encode_out(pio_pins, 1) | pio_encode_sideset(2, 0b10)),
    static_cast<uint16_t>(pio_encode_set(pio_x, 14) | pio_encode_sideset(2, 0b11)),
};

const pio_program_t i2sProgram = {
    i2sInstructions,
    kI2sProgramLength,
    -1,
};

void fillAudioBuffer() {
    for (uint frame = 0; frame < kFramesPerBuffer; ++frame) {
        int32_t mixedSample = 0;
        uint activeVoiceCount = 0;

        for (Voice& voice : voices) {
            if (!voice.assigned) {
                continue;
            }

            if (voice.keyDown && voice.gain < voice.targetGain) {
                const uint16_t remainingGain = voice.targetGain - voice.gain;
                voice.gain += remainingGain < kAttackStep ? remainingGain : kAttackStep;
            } else if (!voice.keyDown && voice.gain > 0) {
                voice.gain = voice.gain < kReleaseStep ? 0 : voice.gain - kReleaseStep;
            }

            if (!voice.keyDown && voice.gain == 0) {
                voice.assigned = false;
                continue;
            }

            const int32_t sample = sineTable[voice.phase >> 24];
            voice.phase += voice.phaseIncrement;
            mixedSample += (sample * voice.gain) / kMaxGain;
            ++activeVoiceCount;
        }

        if (activeVoiceCount > 1) {
            mixedSample /= static_cast<int32_t>(activeVoiceCount);
        }
        if (mixedSample > INT16_MAX) {
            mixedSample = INT16_MAX;
        } else if (mixedSample < INT16_MIN) {
            mixedSample = INT16_MIN;
        }

        const uint16_t pcmSample = static_cast<uint16_t>(static_cast<int16_t>(mixedSample));
        audioBuffer[frame] = (static_cast<uint32_t>(pcmSample) << 16) | pcmSample;
    }
}

void __isr audioDmaIrqHandler() {
    const uint32_t channelMask = 1u << kDmaChannel;
    if ((dma_hw->ints0 & channelMask) == 0) {
        return;
    }

    dma_hw->ints0 = channelMask;
    ++dmaBufferRefills;
    fillAudioBuffer();
    dma_channel_set_read_addr(kDmaChannel, audioBuffer, false);
    dma_channel_set_trans_count(kDmaChannel, kFramesPerBuffer, true);
}
} // namespace

bool i2sAudioBegin() {
    constexpr float kTwoPi = 6.28318530718f;
    for (uint index = 0; index < 256; ++index) {
        const float phase = kTwoPi * static_cast<float>(index) / 256.0f;
        sineTable[index] = static_cast<int16_t>(sinf(phase) * 12000.0f);
    }
    for (uint note = 0; note < 128; ++note) {
        const float semitonesFromA4 = static_cast<float>(static_cast<int>(note) - 69) / 12.0f;
        const float frequency = 440.0f * powf(2.0f, semitonesFromA4);
        notePhaseIncrements[note] = static_cast<uint32_t>(
            (static_cast<double>(frequency) * static_cast<double>(kPhaseScale)) / kAudioSampleRate);
    }

    const int stateMachine = pio_claim_unused_sm(kPio, false);
    if (stateMachine < 0) {
        return false;
    }
    kActiveStateMachine = static_cast<uint>(stateMachine);
    if (!pio_can_add_program(kPio, &i2sProgram)) {
        pio_sm_unclaim(kPio, static_cast<uint>(stateMachine));
        return false;
    }
    const int dmaChannel = dma_claim_unused_channel(false);
    if (dmaChannel < 0) {
        pio_sm_unclaim(kPio, static_cast<uint>(stateMachine));
        return false;
    }

    kPioOffset = pio_add_program(kPio, &i2sProgram);
    i2sInstructions[1] = static_cast<uint16_t>(
        pio_encode_jmp_x_dec(kPioOffset) | pio_encode_sideset(2, 0b11));
    i2sInstructions[5] = static_cast<uint16_t>(
        pio_encode_jmp_x_dec(kPioOffset + 4) | pio_encode_sideset(2, 0b01));
    kPio->instr_mem[kPioOffset + 1] = i2sInstructions[1];
    kPio->instr_mem[kPioOffset + 5] = i2sInstructions[5];
    kDmaChannel = static_cast<uint>(dmaChannel);

    pio_gpio_init(kPio, kDataPin);
    pio_gpio_init(kPio, kBitClockPin);
    pio_gpio_init(kPio, kWordSelectPin);

    pio_sm_config config = pio_get_default_sm_config();
    sm_config_set_out_pins(&config, kDataPin, 1);
    sm_config_set_sideset(&config, 2, false, false);
    sm_config_set_sideset_pins(&config, kBitClockPin);
    sm_config_set_out_shift(&config, false, true, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_wrap(&config, kPioOffset, kPioOffset + kI2sProgramLength - 1);

    const uint32_t divider256 =
        (clock_get_hz(clk_sys) * 4u + kAudioSampleRate / 2u) / kAudioSampleRate;
    sm_config_set_clkdiv_int_frac(&config, divider256 >> 8, divider256 & 0xFF);

    pio_sm_init(kPio, static_cast<uint>(stateMachine), kPioOffset, &config);
    pio_sm_set_consecutive_pindirs(kPio, static_cast<uint>(stateMachine), kDataPin, 1, true);
    pio_sm_set_consecutive_pindirs(kPio, static_cast<uint>(stateMachine), kBitClockPin, 2, true);
    pio_sm_exec(kPio, static_cast<uint>(stateMachine), pio_encode_jmp(kPioOffset + 7));

    fillAudioBuffer();

    dma_channel_config dmaConfig = dma_channel_get_default_config(kDmaChannel);
    channel_config_set_transfer_data_size(&dmaConfig, DMA_SIZE_32);
    channel_config_set_read_increment(&dmaConfig, true);
    channel_config_set_write_increment(&dmaConfig, false);
    channel_config_set_dreq(&dmaConfig, pio_get_dreq(kPio, static_cast<uint>(stateMachine), true));
    dma_channel_configure(kDmaChannel, &dmaConfig, &kPio->txf[stateMachine], audioBuffer,
                          kFramesPerBuffer, false);

    irq_add_shared_handler(DMA_IRQ_0, audioDmaIrqHandler,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    dma_channel_set_irq0_enabled(kDmaChannel, true);
    irq_set_enabled(DMA_IRQ_0, true);

    dma_channel_set_read_addr(kDmaChannel, audioBuffer, false);
    dma_channel_set_trans_count(kDmaChannel, kFramesPerBuffer, true);
    pio_sm_set_enabled(kPio, static_cast<uint>(stateMachine), true);
    audioInitialized = true;
    return true;
}

I2sAudioDiagnostics i2sAudioGetDiagnostics() {
    I2sAudioDiagnostics diagnostics{};
    diagnostics.initialized = audioInitialized;
    diagnostics.dmaBufferRefills = dmaBufferRefills;

    if (audioInitialized) {
        diagnostics.pioEnabled =
            (kPio->ctrl & (1u << (PIO_CTRL_SM_ENABLE_LSB + kActiveStateMachine))) != 0;
        diagnostics.dmaBusy = dma_channel_is_busy(kDmaChannel);
        diagnostics.txFifoEmpty = pio_sm_is_tx_fifo_empty(kPio, kActiveStateMachine);
    }

    return diagnostics;
}

void i2sAudioNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (channel > 15 || note > 127 || velocity == 0 || !audioInitialized) {
        return;
    }

    const uint32_t interruptState = save_and_disable_interrupts();
    int selectedVoice = -1;
    int quietestReleasedVoice = -1;
    int quietestVoice = 0;

    for (uint index = 0; index < kVoiceCount; ++index) {
        const Voice& voice = voices[index];
        if (voice.assigned && voice.keyDown && voice.channel == channel && voice.note == note) {
            selectedVoice = static_cast<int>(index);
            break;
        }
        if (!voice.assigned) {
            if (selectedVoice < 0) {
                selectedVoice = static_cast<int>(index);
            }
            continue;
        }
        if (!voice.keyDown && (quietestReleasedVoice < 0 ||
                               voice.gain < voices[quietestReleasedVoice].gain)) {
            quietestReleasedVoice = static_cast<int>(index);
        }
        if (voice.gain < voices[quietestVoice].gain) {
            quietestVoice = static_cast<int>(index);
        }
    }

    if (selectedVoice < 0) {
        selectedVoice = quietestReleasedVoice >= 0 ? quietestReleasedVoice : quietestVoice;
    }

    Voice& voice = voices[selectedVoice];
    const bool retriggering = voice.assigned && voice.keyDown &&
                              voice.channel == channel && voice.note == note;
    if (!retriggering) {
        voice.phase = 0;
        voice.gain = 0;
    }
    voice.phaseIncrement = notePhaseIncrements[note];
    voice.targetGain = static_cast<uint16_t>((static_cast<uint32_t>(velocity) * kMaxGain) / 127);
    voice.channel = channel;
    voice.note = note;
    voice.assigned = true;
    voice.keyDown = true;
    restore_interrupts(interruptState);
}

void i2sAudioNoteOff(uint8_t channel, uint8_t note) {
    if (channel > 15 || note > 127 || !audioInitialized) {
        return;
    }

    const uint32_t interruptState = save_and_disable_interrupts();
    for (Voice& voice : voices) {
        if (voice.assigned && voice.keyDown && voice.channel == channel && voice.note == note) {
            voice.keyDown = false;
            break;
        }
    }
    restore_interrupts(interruptState);
}
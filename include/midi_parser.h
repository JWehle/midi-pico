#pragma once

#include <stdint.h>

// A complete MIDI 1.0 message. SysEx messages are reported at EOX and carry
// only their payload byte count so arbitrarily long messages need no buffer.
struct MidiMessage {
    enum class Type : uint8_t {
        ChannelVoice,
        SystemCommon,
        SystemRealTime,
        SystemExclusive
    };

    Type type;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t dataLength;
    uint32_t sysExDataLength;
};

class MidiParser {
public:
    // Feed one received byte. Returns true only when a complete message is ready.
    bool processByte(uint8_t byte, MidiMessage& message);

private:
    static uint8_t dataLengthForStatus(uint8_t status);

    uint8_t runningStatus_ = 0;
    uint8_t pendingStatus_ = 0;
    uint8_t pendingData_[2] = {0, 0};
    uint8_t pendingDataCount_ = 0;
    uint8_t expectedDataLength_ = 0;
    bool inSysEx_ = false;
    uint32_t sysExDataLength_ = 0;
};

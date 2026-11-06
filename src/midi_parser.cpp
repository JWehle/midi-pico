#include "midi_parser.h"

uint8_t MidiParser::dataLengthForStatus(uint8_t status) {
    if (status < 0xF0) {
        const uint8_t command = status & 0xF0;
        return (command == 0xC0 || command == 0xD0) ? 1 : 2;
    }

    switch (status) {
        case 0xF1: // MIDI Time Code Quarter Frame
        case 0xF3: // Song Select
            return 1;
        case 0xF2: // Song Position Pointer
            return 2;
        case 0xF6: // Tune Request
            return 0;
        default:
            return 0xFF; // Unsupported or variable-length status.
    }
}

bool MidiParser::processByte(uint8_t byte, MidiMessage& message) {
    // Real-time bytes may appear between any two bytes; they do not disturb
    // running status or an in-progress SysEx message.
    if (byte >= 0xF8) {
        message = {MidiMessage::Type::SystemRealTime, byte, 0, 0, 0, 0};
        return true;
    }

    if (byte & 0x80) {
        if (byte == 0xF0) {
            runningStatus_ = 0;
            pendingStatus_ = 0;
            pendingDataCount_ = 0;
            inSysEx_ = true;
            sysExDataLength_ = 0;
            return false;
        }

        if (byte == 0xF7) {
            if (!inSysEx_) {
                return false;
            }
            message = {MidiMessage::Type::SystemExclusive, 0xF0, 0, 0, 0,
                       sysExDataLength_};
            inSysEx_ = false;
            return true;
        }

        if (inSysEx_) {
            // A non-real-time status terminates malformed/incomplete SysEx.
            inSysEx_ = false;
        }

        pendingDataCount_ = 0;
        if (byte >= 0x80 && byte <= 0xEF) {
            runningStatus_ = byte;
            pendingStatus_ = byte;
            expectedDataLength_ = dataLengthForStatus(byte);
            return false;
        }

        // System Common statuses cancel channel running status.
        runningStatus_ = 0;
        pendingStatus_ = byte;
        expectedDataLength_ = dataLengthForStatus(byte);
        if (expectedDataLength_ == 0) {
            message = {MidiMessage::Type::SystemCommon, byte, 0, 0, 0, 0};
            pendingStatus_ = 0;
            return true;
        }
        if (expectedDataLength_ == 0xFF) {
            pendingStatus_ = 0;
        }
        return false;
    }

    if (inSysEx_) {
        ++sysExDataLength_;
        return false;
    }

    if (pendingStatus_ == 0) {
        if (runningStatus_ == 0) {
            return false;
        }
        pendingStatus_ = runningStatus_;
        expectedDataLength_ = dataLengthForStatus(pendingStatus_);
        pendingDataCount_ = 0;
    }

    if (pendingDataCount_ < sizeof(pendingData_)) {
        pendingData_[pendingDataCount_] = byte;
    }
    ++pendingDataCount_;

    if (pendingDataCount_ < expectedDataLength_) {
        return false;
    }

    const bool isChannelVoice = pendingStatus_ < 0xF0;
    message = {isChannelVoice ? MidiMessage::Type::ChannelVoice
                              : MidiMessage::Type::SystemCommon,
               pendingStatus_, pendingData_[0], pendingData_[1],
               expectedDataLength_, 0};

    pendingDataCount_ = 0;
    pendingStatus_ = 0;
    return true;
}

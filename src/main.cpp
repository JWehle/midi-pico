#include <Arduino.h>

#include "hardware/gpio.h"
#include "i2s_audio.h"
#include "midi_parser.h"

namespace {
constexpr uint32_t kMidiTxPin = 0;
constexpr uint32_t kMidiRxPin = 1;
constexpr uint32_t kMidiBaud = 31250;
constexpr uint32_t kLedPulseMs = 40;
constexpr uint32_t kHeartbeatIntervalMs = 1000;

MidiParser midiParser;
bool ledIsOn = false;
uint32_t ledTurnOffAt = 0;
uint32_t lastHeartbeatAt = 0;
uint32_t lastDmaBufferRefills = 0;

const char* channelCommandName(uint8_t status) {
    switch (status & 0xF0) {
        case 0x80: return "Note Off";
        case 0x90: return "Note On";
        case 0xA0: return "Polyphonic Pressure";
        case 0xB0: return "Control Change";
        case 0xC0: return "Program Change";
        case 0xD0: return "Channel Pressure";
        case 0xE0: return "Pitch Bend";
        default:   return "Unknown Channel Message";
    }
}

const char* realTimeName(uint8_t status) {
    switch (status) {
        case 0xF8: return "Clock";
        case 0xFA: return "Start";
        case 0xFB: return "Continue";
        case 0xFC: return "Stop";
        case 0xFE: return "Active Sensing";
        case 0xFF: return "System Reset";
        default:   return "Real-Time";
    }
}

void printHexByte(uint8_t value) {
    if (value < 0x10) {
        Serial.print('0');
    }
    Serial.print(value, HEX);
}

void printMessage(const MidiMessage& message) {
    Serial.print("MIDI ");

    switch (message.type) {
        case MidiMessage::Type::ChannelVoice:
            Serial.print(channelCommandName(message.status));
            Serial.print(" | channel ");
            Serial.print((message.status & 0x0F) + 1);
            Serial.print(" | data ");
            Serial.print(message.data1);
            if (message.dataLength == 2) {
                Serial.print(", ");
                Serial.print(message.data2);
            }
            break;
        case MidiMessage::Type::SystemCommon:
            Serial.print("System Common");
            if (message.dataLength > 0) {
                Serial.print(" | data ");
                Serial.print(message.data1);
                if (message.dataLength == 2) {
                    Serial.print(", ");
                    Serial.print(message.data2);
                }
            }
            break;
        case MidiMessage::Type::SystemRealTime:
            Serial.print(realTimeName(message.status));
            break;
        case MidiMessage::Type::SystemExclusive:
            Serial.print("System Exclusive complete | ");
            Serial.print(message.sysExDataLength);
            Serial.print(" data bytes");
            break;
    }

    Serial.print(" | bytes: ");
    if (message.type == MidiMessage::Type::SystemExclusive) {
        Serial.print("F0 ... F7");
    } else {
        printHexByte(message.status);
        if (message.dataLength > 0) {
            Serial.print(' ');
            printHexByte(message.data1);
        }
        if (message.dataLength > 1) {
            Serial.print(' ');
            printHexByte(message.data2);
        }
    }
    Serial.println();
}

void handleMidiNote(const MidiMessage& message) {
    if (message.type != MidiMessage::Type::ChannelVoice) {
        return;
    }

    const uint8_t command = message.status & 0xF0;
    const uint8_t channel = message.status & 0x0F;
    if (command == 0x90 && message.data2 != 0) {
        i2sAudioNoteOn(channel, message.data1, message.data2);
    } else if (command == 0x80 || command == 0x90) {
        i2sAudioNoteOff(channel, message.data1);
    }
}

void pulseLed() {
    digitalWrite(LED_BUILTIN, HIGH);
    ledIsOn = true;
    ledTurnOffAt = millis() + kLedPulseMs;
}

void updateLed() {
    if (ledIsOn && static_cast<int32_t>(millis() - ledTurnOffAt) >= 0) {
        digitalWrite(LED_BUILTIN, LOW);
        ledIsOn = false;
    }
}
} // namespace

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    // USB CDC debug output. Do not wait for a host connection so MIDI starts
    // immediately after power-up.
    Serial.begin(115200);

    if (i2sAudioBegin()) {
        Serial.println("I2S MIDI synth ready: 44100 Hz, 16-bit stereo, 8 voices");
    } else {
        Serial.println("I2S audio initialization failed");
    }

    // On the RP2040, GPIO1 can be selected as UART0 RX. MIDI uses 8-N-1.
    // The Arduino Mbed core maps Serial1 RX to GPIO1 on the Pico variant.
    Serial1.begin(kMidiBaud);
    gpio_disable_pulls(kMidiTxPin);
    gpio_disable_pulls(kMidiRxPin);

    Serial.println("RP2040 MIDI monitor ready");
    Serial.println("UART0 TX/RX: GPIO0/GPIO1, no internal pulls; MIDI RX: GPIO1, 31250 baud");
    Serial.println("USB CDC debug: 115200");
}

void loop() {
    const uint32_t now = millis();
    if (now - lastHeartbeatAt >= kHeartbeatIntervalMs) {
        lastHeartbeatAt = now;
        const I2sAudioDiagnostics audio = i2sAudioGetDiagnostics();
        const uint32_t refillsPerSecond = audio.dmaBufferRefills - lastDmaBufferRefills;
        lastDmaBufferRefills = audio.dmaBufferRefills;
        Serial.print(":3 | I2S init=");
        Serial.print(audio.initialized ? "ok" : "FAIL");
        Serial.print(" PIO=");
        Serial.print(audio.pioEnabled ? "on" : "off");
        Serial.print(" DMA=");
        Serial.print(audio.dmaBusy ? "busy" : "idle");
        Serial.print(" TX-FIFO=");
        Serial.print(audio.txFifoEmpty ? "empty" : "data");
        Serial.print(" refills/s=");
        Serial.println(refillsPerSecond);
    }

    while (Serial1.available() > 0) {
        MidiMessage message{};
        if (midiParser.processByte(static_cast<uint8_t>(Serial1.read()), message)) {
            pulseLed();
            handleMidiNote(message);
            printMessage(message);
        }
    }

    updateLed();
}

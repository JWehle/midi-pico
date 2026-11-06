#pragma once

#include <stdint.h>

struct I2sAudioDiagnostics {
	bool initialized;
	bool pioEnabled;
	bool dmaBusy;
	bool txFifoEmpty;
	uint32_t dmaBufferRefills;
};

bool i2sAudioBegin();
I2sAudioDiagnostics i2sAudioGetDiagnostics();
void i2sAudioNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
void i2sAudioNoteOff(uint8_t channel, uint8_t note);
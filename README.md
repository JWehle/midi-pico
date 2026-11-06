# MIDI Pico

RP2040-based MIDI keyboard to PCM I2S DAC. PlatformIO C++ firmware for a Raspberry Pi Pico. It receives MIDI 1.0 serial data on GPIO1 (UART0 RX), parses complete MIDI messages, plays notes through the PCM5102 I2S DAC, pulses the onboard LED, and reports messages over USB serial.

## Project layout

- `src/main.cpp` — hardware setup, LED pulse, USB serial diagnostics
- `src/i2s_audio.cpp` and `include/i2s_audio.h` — 8-voice MIDI sine synth over I2S using PIO and DMA
- `src/midi_parser.cpp` and `include/midi_parser.h` — MIDI byte-stream parser (channel voice, running status, System Common, real-time, and completed SysEx messages)
- `platformio.ini` — PlatformIO Raspberry Pi Pico / Arduino Mbed build configuration

## Hardware

- MIDI input signal to Pico GPIO1 (UART0 RX)
- MIDI input ground to Pico GND
- MIDI baud: 31,250, 8-N-1
- USB cable from Pico to host for power, programming, and debug output
- PCM5102 `DIN` to GPIO22 (physical pin 29)
- PCM5102 `BCK` to GPIO26 (physical pin 31)
- PCM5102 `LRCK` to GPIO27 (physical pin 32)
- PCM5102 `SCK` to GND for 3-wire I2S mode (unless the breakout already ties it low)
- PCM5102 ground to Pico GND
- I2S output: 44,100 frames/s, 16-bit stereo
- MIDI Note On starts the corresponding equal-tempered note; Note Off releases it, and velocity controls volume

The PCM5102 provides line-level audio output; use a headphone amplifier rather than driving low-impedance headphones directly from the DAC output. The once-per-second USB serial heartbeat reports PIO state, DMA activity, TX FIFO state, and the total DMA buffer refill count to help confirm digital audio streaming.

**Electrical note:** RP2040 GPIO is 3.3 V only and is not 5 V tolerant. A traditional 5-pin DIN MIDI input should use a proper opto-isolated MIDI IN circuit, with its output compatible with 3.3 V logic. Do not connect a 5 V MIDI signal directly to GPIO1.

## Build and flash

Install PlatformIO Core (the `pio` command) or the PlatformIO IDE extension, then from this project directory run:

```sh
pio run
pio run --target upload
```

The project uses PlatformIO's `picotool` uploader to program the RP2040 over USB. The configured `COM3` port lets PlatformIO use the Pico's 1200-bps USB-serial touch to reboot into its bootloader before programming flash. If Windows assigns a different COM port, update `upload_port` in `platformio.ini`. `picotool` writes the firmware directly to flash; it does not copy a `.bin` file to the `RPI-RP2` drive. Upload with `pio run --target upload`.

## Read debug output

After upload, connect/reset the board and run:

```sh
pio device monitor --baud 115200
```

The USB CDC monitor reports decoded message names, MIDI channel (shown 1–16), data bytes, and raw status/data bytes. Realtime messages are also reported as received. Close the serial monitor before attempting another upload if the upload process cannot access the board.

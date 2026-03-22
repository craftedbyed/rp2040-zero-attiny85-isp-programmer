# RP2040-Zero ISP Programmer for ATtiny85

A dead-simple ATtiny85 programmer built on the Waveshare RP2040 Zero. No USBasp for this one, no FTDI adapter. It got it to speak the STK500v1 protocol, so avrdude talks to it out of the box. The onboard WS2812 RGB LED gives you live status at a glance: Blue when idle, Yellow while programming, Green on success, and Red if something goes wrong.

---

## Table of Contents

- [Hardware Requirements](#hardware-requirements)
- [Library Dependencies](#library-dependencies)
- [Configuration](#configuration)
- [Wiring Diagram](#wiring-diagram)
- [LED Status Reference](#led-status-reference)
- [How It Works](#how-it-works)

---

## Hardware Requirements

| Component | Notes |
|---|---|
| Waveshare RP2040 Zero | The WS2812 LED on GP16 is built in |
| ATtiny85 20U (DIP-8) | The target chip being programmed |
| Breadboard + jumper wires | Four signal wires between the two boards |
| USB-C cable | Powers the RP2040 Zero and carries the serial connection to your PC |

> The RP2040 Zero is breadboard-friendly and cheap. Any clone should work as long as the pinout matches and GP16 carries the onboard NeoPixel.

---

## Library Dependencies

| Library | Source | Notes |
|---|---|---|
| `Adafruit_NeoPixel` | Arduino Library Manager | Required for the WS2812 status LED on GP16 |
| `Arduino-Pico` core | [arduino-pico on GitHub](https://github.com/earlephilhower/arduino-pico) | Board support for RP2040 in Arduino IDE |

Install `Adafruit_NeoPixel` library if you haven't already. For the board core, add the Arduino-Pico board manager URL in **File → Preferences** and install `Raspberry Pi Pico/RP2040` from the Boards Manager.

---

## Configuration

1. **Add the RP2040 board manager URL** in Arduino IDE preferences:
   ```
   https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
   ```

2. **Install the board core** — Boards Manager → search `Raspberry Pi Pico/RP2040/RP2350` → Install.

3. **Select the board** — Tools → Board → `Waveshare RP2040 Zero`.

4. **Set baud rate** — I hardcoded my sketch to `19200`. No need to change it; just make sure avrdude matches.

5. **Flash the sketch** — upload `RP2040_ISP.ino` over USB. On power-up you'll see a quick blue → yellow → green LED test sequence confirming the firmware is running.

6. **Point avrdude at the right port.** Use programmer type `arduino` and baud `19200`. Example:
   ```bash
   avrdude -c arduino -p t85 -P /dev/ttyACM0 -b 19200 -U flash:w:your_sketch.hex
   ```
   On Windows, replace `/dev/ttyACM0` with the COM port shown in Device Manager.

> Arduino IDE users: set the programmer to **Arduino as ISP** and the port to the RP2040's COM/serial port, then use **Sketch → Upload Using Programmer**.

---

## Wiring Diagram

Four wires connect the RP2040 Zero to the ATtiny85. I can't stress this enough: Power the ATtiny85 from the RP2040's 3.3 V pin, NOT the 5V rail

```
RP2040 Zero          ATtiny85 (DIP-8)
───────────          ────────────────
GP3  (MOSI) ──────►  Pin 5  PB0 (MOSI)
GP4  (MISO) ◄──────  Pin 6  PB1 (MISO)
GP2  (SCK)  ──────►  Pin 7  PB2 (SCK)
GP5  (RESET)──────►  Pin 1  PB5 (RESET)
3V3         ──────►  Pin 8  VCC
GND         ──────►  Pin 4  GND
```

ATtiny85 DIP-8 pin numbering:

```
        ┌──────────┐
 PB5  1 │●         │ 8  VCC
 PB3  2 │          │ 7  PB2  ◄── SCK   (GP2)
 PB4  3 │          │ 6  PB1  ──► MISO  (GP4)
 GND  4 │          │ 5  PB0  ◄── MOSI  (GP3)
        └──────────┘
Pin 1 (PB5) ◄── RESET (GP5)
```

> Keep wires short. A couple of inches on a breadboard is fine. No pull-up resistors or decoupling caps are strictly needed for bench programming, but a 100 nF cap between VCC and GND on the ATtiny85 doesn't hurt. In fact, I recommend it.

---

## LED Status Reference

| Colour | State |
|---|---|
| Pulsing blue | Idle / ready, waiting for avrdude |
| Pulsing yellow | Programming in progress |
| Pulsing green (brief) | Page written successfully |
| Pulsing red | Protocol error — check wiring and baud rate |

---

## How It Works

The sketch implements the STK500v1 protocol over USB-Serial at 19200 baud. Rather than using the RP2040's hardware SPI peripheral, it uses a software bit-banged SPI class to drive GP2/3/4 directly. This keeps the implementation portable and avoids any hardware SPI conflicts with the USB stack.

The SPI clock is set to approximately `100000 / 6 ≈ 16 kHz` — well within the ATtiny85's ISP timing requirements. On each programming session, the sketch asserts RESET on GP5, sends the `0xAC 0x53` programming-enable command, then handles flash and EEPROM read/write commands as they arrive from avrdude. When the session ends cleanly, RESET is released and all SPI pins go back to input.

# Pico-ARC452A21
RP2040-based IoT implementation of a Daikin AC Remote Control using a BMP280 temperature sensor and a Pimoroni Pico Explorer breakout board.

This is a sister project to the [ESP-ARC452A21](https://github.com/sohampatwardhan/ESP-ARC452A21) project, and will be built using CMake.

## Hardware

- Raspberry Pi Pico W / RP2040
- 16 MB external flash
- IR LED on GPIO0 through an NPN low-side driver: 3.3V -> current-limiting resistors -> IR LED -> NPN collector, NPN emitter -> common ground, GPIO0 -> base resistor -> NPN base
- BMP280 or BME280 sensor over I2C: GP20 SDA, GP21 SCL/SCK

The default I2C pins are GP20 SDA and GP21 SCL/SCK. Override them at configure time if your wiring uses different pins.

## Flash Layout

The Pico SDK links the application into XIP flash from offset `0x00000000`.
This project declares the board flash as 16 MB and reserves the final 1 MB for app-owned persistent data:

- `0x00000000..0x00EFFFFF`: firmware image
- `0x00F00000..0x00F0FFFF`: app settings and last AC state
- `0x00F10000..0x00F1FFFF`: future redundant settings/state
- `0x00F20000..0x00FFFFFF`: future IR capture log

See `flash_layout.csv` and `include/pico_arc452a21/flash_layout.h`.

## Build

Install or clone the Raspberry Pi Pico SDK, then configure with CMake:

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -DPICO_BOARD=pico_w
cmake --build build
```

On this development machine, the SDK and Arm GNU toolchain were installed locally without sudo. Use the preset:

```sh
cmake --preset pico-w-local-toolchain
cmake --build --preset pico-w-local-toolchain
```

Optional pin overrides:

```sh
cmake -S . -B build \
  -DPICO_BOARD=pico_w \
  -DPICO_ARC_IR_GPIO=0 \
  -DPICO_ARC_I2C_INSTANCE=0 \
  -DPICO_ARC_I2C_SDA_PIN=20 \
  -DPICO_ARC_I2C_SCL_PIN=21
```

Flash `build/pico_arc452a21.uf2` to the Pico W.

## USB Serial Commands

The first firmware slice mirrors the ESP project command model over USB serial:

```text
status | send | save | help
on [temp] | off [temp] | 72 | 22c | temp 72 [f|c]
unit fahrenheit|celsius
mode auto|dry|cool|heat|fan
fan 1|2|3|4|5|auto|night
vswing on|off | hswing on|off | quiet on|off
sensor off|comfort|eye|both
polarity normal|invert | timing nominal|captured
repeat 1..10 [gap_ms]  # baseline: repeat 1 80
irtest [ms]
reboot
```

`BMP280` parts expose temperature and pressure. If the attached module is actually a `BME280`, the firmware also reports humidity.

## IoT Setup

The Pico W tries saved Wi-Fi credentials on boot. If no saved network exists, or all saved networks fail to connect, it starts a setup access point:

- SSID: `Pico-ARC452A21`
- Password: `arc452a21`
- Setup URL: `http://192.168.4.1/`

Open `http://192.168.4.1/settings`, add your Wi-Fi SSID/password, then reboot from the settings page or with the serial command `reboot`. On the next boot the Pico W should join the saved network and print its IP address on USB serial:

```text
Wi-Fi connected: your-ssid at 192.168.x.y
HTTP server ready on http://192.168.x.y/
```

The web UI exposes the same main control/settings shape as the ESP32 version:

- `/` - air conditioner controls
- `/settings` - unit, IR, Wi-Fi, integration, and reboot settings
- `/send?cmd=status` - JSON status
- `/send?cmd=on%2072%20f` - send a serial-style AC command over HTTP
- `/command` - POST a plain text serial-style command
- `/wifi` - GET saved Wi-Fi status, POST Wi-Fi forms
- `/health` - simple health check

The Pico build currently includes Wi-Fi storage, AP fallback, HTTP control, and ESP-compatible JSON stubs for HomeKit/MQTT settings pages. Native HomeKit and MQTT runtime modules are not compiled into this firmware yet.

## IR Hardware Notes

The Pico hardware uses an NPN low-side driver, so GPIO0 high turns the transistor and IR LED on. The default polarity is `normal`. With three 220 ohm resistors in parallel, the effective resistance is about 73 ohms; at 3.3V this is a reasonable pulsed LED-side current for a transistor driver, but the GPIO base path should still have its own resistor.

The known-good IR baseline is `polarity normal`, `timing nominal`, and `repeat 1 80`. Keep the 80 ms repeat gap even when using a single transmit repeat; the Daikin unit was verified with that setting.

If the AC does not respond, first run `irtest 3000` and look at the IR LED through a phone camera. If it does not visibly glow or flicker, check the GPIO0 base wiring, base resistor, LED orientation, and common ground.

# ESPHome BC7215A Universal AC IR Controller

An ESPHome external component that integrates the **BC7215A** universal
air-conditioner IR transceiver chip with Home Assistant, running on a
**LILYGO TTGO T-Display (ESP32)** board.

The BC7215A handles all brand-specific IR encoding and decoding internally.
The ESP32 communicates with it over UART, so this works with virtually any
AC brand without needing per-brand IR databases.

---

## Features

- **Home Assistant climate entity** -- control temperature, mode, and fan speed
- **IR state tracking** -- state updates automatically when the original remote is used
- **TTGO T-Display** -- live status on the 135x240 colour TFT screen
- **Physical buttons** -- adjust temperature, mode, and fan without a phone
- **OTA updates** -- no USB cable needed after first flash
- **Pairing via HA service** -- one-click pairing with any AC unit

---

## Repository structure

```
.
|- bc7215ac_ttgo.yaml          ESPHome configuration (flash this)
|- secrets.yaml.example        Template for secrets -- copy to secrets.yaml
|- components/
|  `- bc7215ac/
|     |- __init__.py           C++ namespace and class declaration
|     |- climate.py            ESPHome config schema and code generation
|     `- bc7215ac_climate.h    C++ component implementation
|- LICENSE
`- README.md
```

---

## Hardware

### BC7215A IR Transceiver Module

The BC7215A chip handles all brand-specific IR encoding/decoding.
It is available as a small breakout board with an IR LED and receiver.

- Product page: https://www.bitcode.com.cn/en/bc7215/
- Original Arduino library: https://github.com/bitcode-tech/bc7215ac

### LILYGO TTGO T-Display (ESP32)

Any revision works. The board has a built-in 135x240 ST7789 TFT and two
physical buttons, both used by this firmware.

### Wiring

```
TTGO T-Display    BC7215A Module
--------------    --------------
GPIO25       -->  RXD   (ESP32 sends commands to chip)
GPIO33       <--  TXD   (chip sends ACKs and reports to ESP32)
GPIO27       -->  MOD   (HIGH = pairing mode)
GPIO26       <--  BUSY  (HIGH = chip is processing)
3.3V         -->  VCC
GND          -->  GND
```

Both devices run at 3.3 V -- no level shifter required.

### TTGO T-Display built-in pins (pre-configured in YAML)

| Function      | GPIO |
|---------------|------|
| TFT CS        | 5    |
| TFT DC        | 16   |
| TFT RST       | 23   |
| TFT Backlight | 4    |
| TFT CLK (SPI) | 18   |
| TFT MOSI(SPI) | 19   |
| Right button  | 0    |
| Left button   | 35   |

---

## Setup

### 1. Install ESPHome

```bash
pip install esphome
```

### 2. Configure secrets

```bash
cp secrets.yaml.example secrets.yaml
```

Edit `secrets.yaml` and fill in your WiFi credentials, HA API key, and OTA
password. **Never commit this file** -- it is listed in `.gitignore`.

### 3. First flash (USB)

```bash
esphome run bc7215ac_ttgo.yaml
```

Select your USB port when prompted. After the first flash, all subsequent
updates can be done over WiFi via OTA.

### 4. Pair the BC7215A with your AC

The chip must be paired with your specific AC model once before it can send
or decode IR commands. The profile is stored in the chip's own flash.

**Via Home Assistant:**

Once the device appears in HA, call the service:

```
Developer Tools -> Services -> ESPHome: bc7215ac_ttgo_pair_ac
```

Within 10 seconds, point your original AC remote at the BC7215A module and
press any button (25 C Cool mode recommended).

---

## Button controls

| Button        | Short press (< 350 ms) | Long press (> 500 ms) |
|---------------|------------------------|-----------------------|
| Left (GPIO35) | Temperature -1 C       | Cycle mode            |
| Right (GPIO0) | Temperature +1 C       | Cycle fan speed       |

Mode cycle: Off -> Cool -> Heat -> Dry -> Fan Only -> Off

Fan cycle: Auto -> Low -> Medium -> High -> Auto

---

## Home Assistant climate entity

| Attribute        | Values                                       |
|------------------|----------------------------------------------|
| Modes            | Off, Cool, Heat, Dry, Fan Only, Auto         |
| Fan modes        | Auto, Low, Medium, High                      |
| Temperature range| 16 C to 30 C (1 C steps)                    |

The entity state is kept in sync when the AC is operated with its original
remote (the BC7215A decodes the incoming IR and reports the new state).

---

## Troubleshooting

| Symptom                        | Likely cause           | Fix                              |
|-------------------------------|------------------------|----------------------------------|
| AC does not respond            | Not paired             | Run the pair_ac service          |
| "BUSY timeout" in logs         | MOD/BUSY wiring issue  | Check GPIO26/27 connections      |
| No display                     | SPI wiring or wrong model | Verify TFT pin configuration  |
| WiFi not connecting            | Wrong credentials      | Check secrets.yaml               |
| Python syntax error on load    | Non-ASCII in .py file  | Ensure files are saved as UTF-8  |

---

## Credits

- BC7215A chip and its firmware: Bitcode Technology (https://www.bitcode.com.cn)
- Original Arduino library: https://github.com/bitcode-tech/bc7215ac
- ESPHome: https://esphome.io

---

## License

MIT -- see [LICENSE](LICENSE).

The BC7215A chip firmware is proprietary to Bitcode Technology and is not
covered by this licence.

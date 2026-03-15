# 📺 ESP Display

A lightweight WiFi-controlled display controller for the **ESP8266 NodeMCU** with an **ST7735 1.8" TFT** (160×128 landscape). Upload images, animations, or custom text directly from your browser — no app, no cables, no fuss.

---

## ✨ Features

- **In-browser JPG processing** — any image you pick gets automatically resized to 160×128, letterboxed, and compressed under 15 KB before uploading. Zero processing load on the ESP.
- **RAW RGB565 animation** (`.rgb`) — upload multi-frame animations that loop smoothly with configurable frame delay.
- **Custom text display** — set text, font size, text colour, and background colour from the web UI.
- **Captive portal** — connect to the ESP's WiFi AP and the web UI opens automatically on most devices.
- **Persists across reboots** — last displayed content (image/animation/text + settings) is saved to EEPROM and LittleFS.
- **Tear-free animation** — each frame renders atomically (single seek + sequential line reads, no mid-frame yields).

---

## 🛠 Hardware

| Component | Details |
|---|---|
| Microcontroller | ESP8266 NodeMCU (any variant) |
| Display | ST7735 1.8" TFT, 128×160, SPI |
| Rotation | Landscape — 160 wide × 128 tall (`setRotation(1)`) |

### Wiring (NodeMCU → ST7735)

| ST7735 Pin | NodeMCU Pin | Notes |
|---|---|---|
| VCC | 3.3V | |
| GND | GND | |
| SCL / SCK | D5 (GPIO14) | SPI Clock |
| SDA / MOSI | D7 (GPIO13) | SPI Data |
| RES / RST | D4 (GPIO2) | Reset |
| DC / A0 | D3 (GPIO0) | Data/Command |
| CS | D8 (GPIO15) | Chip Select |
| BL | 3.3V or D0 | Backlight (always on or GPIO-controlled) |

> Pin assignments depend on your `User_Setup.h` in TFT_eSPI. Adjust to match.

---

## 📦 Dependencies

Install all via Arduino Library Manager or PlatformIO:

| Library | Version tested |
|---|---|
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | ≥ 2.5.0 |
| [TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder) | ≥ 1.0.0 |
| [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) | ≥ 1.2.3 |
| [ESPAsyncTCP](https://github.com/me-no-dev/ESPAsyncTCP) | ≥ 1.2.2 |
| LittleFS | Built-in (ESP8266 Arduino core ≥ 3.0) |
| EEPROM | Built-in |
| Ticker | Built-in |

### TFT_eSPI setup

Edit `TFT_eSPI/User_Setup.h` to match your display and wiring:

```cpp
#define ST7735_DRIVER
#define TFT_WIDTH  128
#define TFT_HEIGHT 160
#define TFT_CS   PIN_D8
#define TFT_DC   PIN_D3
#define TFT_RST  PIN_D4
#define SPI_FREQUENCY  27000000   // 27 MHz — max reliable for ST7735
```

---

## 🚀 Getting Started

1. Clone this repo and open `ESP_Display.ino` in Arduino IDE.
2. Select board: **NodeMCU 1.0 (ESP-12E Module)**.
3. Set **Flash Size** to include LittleFS (e.g. `4MB (FS:2MB OTA:~1019KB)`).
4. Configure `User_Setup.h` in TFT_eSPI as above.
5. Flash the sketch.
6. On your phone or laptop, connect to WiFi:
   - **SSID:** `ESP_Display`
   - **Password:** `12345678`
7. Open a browser and go to `192.168.4.1` (or just wait for the captive portal to open).

---

## 🖥 Web UI

The web interface runs directly on the ESP at `192.168.4.1`.

### Image Upload
- Pick any JPG/JPEG from your device.
- The browser automatically:
  - Resizes it to **160×128** with black letterbox bars (aspect ratio preserved).
  - Iteratively compresses it to under **15 KB**.
  - Shows a live preview with final size and quality.
- Click **Upload Image** to send the processed file.

### RAW Animation Upload
- Upload a `.rgb` file (see format below).
- Set frame delay (ms) — default 150 ms (~6 fps).
- Click **Upload RAW**.

### Text Display
- Type up to 200 characters.
- Choose text colour, background colour, and text size (1–4).
- Click **Display Text**.

### Reset
- Clears the display back to the placeholder screen and wipes saved settings.

---

## 🎞 RAW RGB565 Animation Format

The `.rgb` format is a simple raw binary dump of concatenated frames:

| Property | Value |
|---|---|
| Resolution | 160 × 128 pixels (landscape) |
| Pixel format | RGB565 little-endian |
| Bytes per frame | `160 × 128 × 2 = 40,960 bytes` |
| Multi-frame | Frames concatenated end-to-end |
| File size | Must be an exact multiple of 40,960 |

### Convert a GIF or video to `.rgb` (Python)

```python
from PIL import Image
import struct, sys

INPUT  = "animation.gif"   # or any image sequence
OUTPUT = "u.rgb"
W, H   = 160, 128

frames = []
img = Image.open(INPUT)
try:
    while True:
        frame = img.copy().convert("RGB").resize((W, H), Image.LANCZOS)
        for y in range(H):
            for x in range(W):
                r, g, b = frame.getpixel((x, y))
                rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                frames.append(struct.pack('<H', rgb565))
        img.seek(img.tell() + 1)
except EOFError:
    pass

with open(OUTPUT, 'wb') as f:
    for px in frames:
        f.write(px)

print(f"Written {len(frames) // (W*H)} frames to {OUTPUT}")
```
### Web UI Preview

<img src="Browser UI.jpeg" alt="Project Logo" width="200"/>
---

## ⚙️ Configuration

Key constants at the top of the sketch:

```cpp
#define AP_SSID             "ESP_Display"   // WiFi AP name
#define AP_PASS             "12345678"      // WiFi AP password
#define MAX_JPG_SIZE        15000           // Max JPG size in bytes
#define MAX_RGB_SIZE        800000          // Max .rgb file size in bytes (~800 KB)
#define DEFAULT_FRAME_DELAY_MS  150         // Default animation frame delay
```

---

## 📁 File Structure

```
ESP_Display.ino       — Main sketch (single file)
README.md             — This file
```

LittleFS stores:
```
/u.jpg       — uploaded JPG image
/u.rgb       — uploaded RAW animation
/text.txt    — saved display text
```

---

## 🐛 Troubleshooting

| Problem | Fix |
|---|---|
| Blank/white screen | Check `User_Setup.h` pin config and SPI frequency |
| Display upside down or wrong orientation | Try `setRotation(0..3)` values |
| Upload fails / file too large | Check `MAX_RGB_SIZE` — increase if needed. Ensure `.rgb` file size is a multiple of 40960 |
| Animation freezes | Serial monitor will show the error. Usually a corrupt `.rgb` file |
| Web UI not loading | Connect to `ESP_Display` AP, navigate to `192.168.4.1` manually |
| Watchdog resets | Normal during heavy flash I/O — `feed()` is called after every frame |

---

## 📄 License

MIT — do whatever you want with it.

---

## 🙏 Credits

- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) by Bodmer
- [TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder) by Bodmer
- [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) by me-no-dev

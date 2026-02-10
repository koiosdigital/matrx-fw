# MATRX Firmware

ESP32-based firmware for MATRX LED matrix display controllers. Manages dynamic content display, scheduling, and remote management for networked LED matrix panels.

## Features

- **HUB75 LED Matrix Display** - DMA-accelerated driver supporting 64x32, 64x64, 128x32, and 128x64 panel configurations
- **WebP Decoding** - Supports static and animated WebP content - _Now accelerated with Xtensa PIE instructions!_
- **Content Scheduling** - Server-driven schedule with display durations, pinning, and smart pre-fetching
- **WebSocket Communication** - Protobuf-serialized messaging for real-time server sync
- **Button Controls** - Previous/pin/next navigation with hardware debouncing
- **Auto-Brightness** - Ambient light sensor integration (VEML6030)
- **OTA Updates** - Over-the-air firmware updates
- **BLE Provisioning** - WiFi configuration via Bluetooth

## Hardware Support

| Variant  | Description                                     |
| -------- | ----------------------------------------------- |
| MATRX v9 | Primary version with 3 buttons and light sensor |

## Requirements

- ESP-IDF v6.0-beta2+
- [buf](https://buf.build/) CLI for protobuf generation

## Building

```bash
# Configure (first time)
idf.py set-target esp32s3

# Build
idf.py build

# Build with version info
idf.py build -D BUILD_VERSION=1.0.0 -D BUILD_VARIANT=release

# Flash
idf.py flash monitor
```

## Configuration

Hardware and display settings are configured via `idf.py menuconfig`:

- `CONFIG_MATRIX_WIDTH` - Panel width (64/128)
- `CONFIG_MATRIX_HEIGHT` - Panel height (32/64)
- `CONFIG_HW_TYPE` - Hardware variant selection

## Project Structure

```
main/
├── scheduler/      # Content scheduling and timing
├── display/        # HUB75 LED matrix driver
├── sockets/        # WebSocket client
├── sprites/        # Image RAM storage
├── daughterboard/  # Light sensor and button input
├── config/         # NVS configuration
└── hw_defs/        # Hardware pinout definitions

components/
├── ESP32-HUB75-MatrixPanel-DMA/  # LED matrix library
├── libwebp/                       # WebP codec
├── kd_common/                     # Shared utilities
└── protobufs/                     # Protocol buffer definitions
```

## Button Controls

| Button   | Action                           |
| -------- | -------------------------------- |
| A        | Previous item (unpins if pinned) |
| B        | Pin/unpin current item           |
| C        | Next item (unpins if pinned)     |
| A+C (3s) | Factory reset                    |

## License

Proprietary - Koios Digital

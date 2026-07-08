# Local fork of esp-hub75

Vendored from https://github.com/acvigue/esp-hub75 (`components/hub75` subdir)
at commit `dc45393355b3ee9021a84b724e2cf5c109b8e6e1` (2026-02-12), replacing the
`esphome/esp-hub75` managed component.

## Local changes

- `CMakeLists.txt`: add `esp_driver_dma` to REQUIRES on ESP32-S3 for
  `esp_private/gdma.h` under ESP-IDF 6.
- `src/platforms/gdma/gdma_dma.cpp` — `draw_pixels()` hot-path rework:
  - Fused row-pair fast path for full-panel-height blits: upper and lower half
    pixels share one DMA word, so each bit-plane word is read-modify-written
    once instead of twice (~2x less DMA-buffer traffic for full-frame draws).
  - General identity path hoists row lookup, half-select mask and RGB bit
    positions out of the per-pixel loop (they only depend on the row).
  - Bit-plane loops use the compile-time `HUB75_BIT_DEPTH` constant instead of
    the runtime `bit_depth_` member so GCC fully unrolls them.
- `include/hub75_types.h` + both DMA backends (`gdma_dma.cpp`, `i2s_dma.cpp`):
  new `Hub75Config::gpio_drive_strength` (0-3, default 3 = upstream behavior).
  Drive 3 on all 14 panel pins desenses the SoC's own 2.4GHz radio — measured
  30% ICMP loss to the local gateway with the panel running vs ~0% with the
  DMA stopped (2026-07-07). matrx-fw sets drive 1.

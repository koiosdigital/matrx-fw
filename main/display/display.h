// Display - Pure hardware layer for HUB75 LED matrix
#ifndef DISPLAY_H
#define DISPLAY_H

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

    //------------------------------------------------------------------------------
    // Lifecycle
    //------------------------------------------------------------------------------

    /**
     * Initialize the HUB75 DMA display.
     * Configures pins, starts DMA, clears to black.
     */
    void display_init();

    //------------------------------------------------------------------------------
    // Rendering Functions
    //------------------------------------------------------------------------------

    /**
     * Render a decoded RGBA frame to the display.
     * Frame buffer must be RGBA8888 format (4 bytes per pixel).
     * Applies status bar overlay if enabled.
     *
     * @param rgba_frame Pointer to RGBA8888 frame buffer
     * @param width      Frame width in pixels
     * @param height     Frame height in pixels
     */
    void display_render_rgba_frame(const uint8_t* rgba_frame, int width, int height);

    /**
     * Render a raw RGB888 buffer to the display.
     * Buffer must be exactly (CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT * 3) bytes.
     *
     * @param rgb_buffer Pointer to RGB888 buffer
     * @param buffer_len Buffer length in bytes
     */
    void display_render_rgb_buffer(const uint8_t* rgb_buffer, size_t buffer_len);

    /**
     * Clear the display to black.
     */
    void display_clear();

    //------------------------------------------------------------------------------
    // Configuration
    //------------------------------------------------------------------------------

    /**
     * Set display brightness.
     * @param brightness 0-255
     */
    void display_set_brightness(uint8_t brightness);

    //------------------------------------------------------------------------------
    // Utility
    //------------------------------------------------------------------------------

    /**
     * Get display dimensions.
     */
    void display_get_dimensions(int* width, int* height);

    /**
     * Get required buffer size for raw RGB888 rendering.
     */
    size_t display_get_buffer_size();

#ifdef __cplusplus
}
#endif

#endif  // DISPLAY_H

#ifndef DISPLAY_H
#define DISPLAY_H

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

    void display_init();
    void display_register_console_cmds();
    void display_deinit();

    void display_render_rgba_frame(const uint8_t* rgba_frame, int width, int height);
    void display_render_rgba_span(const uint8_t* rgba_span, int x, int y, int width);
    void display_render_rgb_buffer(const uint8_t* rgb_buffer, size_t buffer_len);
    void display_clear();

    void display_set_brightness(uint8_t brightness);

    void display_get_dimensions(int* width, int* height);
    size_t display_get_buffer_size();

#ifdef __cplusplus
}
#endif

#endif  // DISPLAY_H

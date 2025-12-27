#ifndef RENDER_REQUESTS_H
#define RENDER_REQUESTS_H

#include <stdint.h>
#include <stdlib.h>

#define UUID_SIZE_BYTES 16

// Render state for each schedule item
enum class RenderState : uint8_t {
    NeedsRender,      // No valid sprite data yet, needs initial render
    RenderPending,    // Request sent, waiting for response
    RenderComplete,   // Valid sprite data available
    RenderFailed,     // Server returned error, will retry on next prepare window
    ValidationFailed, // Data validation failed, retry immediately
};

// Result of a render response
enum class RenderResult {
    Success,          // Valid sprite data received
    ServerError,      // Server indicated an error (response->error = true)
    InvalidData,      // Sprite data failed validation
    ItemNotFound,     // Schedule item not found for UUID
};

// Initialize the render requests module
void render_requests_init();

// Request a render for a specific UUID
// Returns true if request was sent, false if deduplicated or invalid
bool render_request(const uint8_t* uuid);

// Called when a render response is received from the server
// Returns the result of processing the response
RenderResult render_response_received(const uint8_t* uuid, const uint8_t* data, size_t len, bool server_error);

// Get the render state for a UUID
RenderState render_get_state(const uint8_t* uuid);

// Set render state for a UUID (called by scheduler when schedule changes)
void render_set_state(const uint8_t* uuid, RenderState state);

// Clear all render tracking (called when schedule is cleared)
void render_clear_all();

// Mark a UUID as needing re-render (called before display cycle)
void render_mark_needs_render(const uint8_t* uuid);

// Check if sprite data is valid WebP format
// Returns true if data appears to be valid, false otherwise
bool render_validate_sprite_data(const uint8_t* data, size_t len);

// Get the number of pending render requests
size_t render_get_pending_count();

#endif


# Scheduler / Sprite / Render System

## Overview
The scheduler manages which "app" (sprite) is displayed on the LED matrix and for how long. Apps are rendered server-side and sent as WebP images.

## Core Flow
1. Device connects to websocket → requests schedule
2. Server sends `MatrxSchedule` with list of `ScheduleItem` (uuid, display_time, user_pinned, user_skipped)
3. Device requests render for current item via `SpriteRenderRequest` (uuid + device dimensions)
4. Server responds with `MatrxSpriteData` (uuid, webp data, system_skipped, render_error)
5. Device displays sprite, waits display_time seconds, advances to next item

## Key Concepts

### Flags per schedule item:
- `user_skipped`: Skip entirely, don't render or display (server controls this flag)
- `user_pinned`: Loop this item forever, blank screen if no data, auto-display when data arrives
- `skipped_server`: Temporarily unavailable (empty data, error, or system_skipped=true from server)

### Render states (single source of truth in scheduler):
- `NeedsRender`: No data yet, needs request
- `RenderPending`: Request sent, waiting for response (also used after server-skip to prevent spam)
- `HasData`: Ready to display

### Prepare Window
- 3 seconds before current item's display time expires, request render for upcoming item(s)
- Request render for any server-skipped items in sequence, PLUS the next "normal" item
- This ensures if server-skipped items recover, they'll have fresh data; if not, next normal item is ready
- Don't spam: server-skipped items stay in RenderPending state, only retry after 5-second timeout

### Pinned Item Behavior
- When schedule is received, if any item is pinned, start on that item immediately
- Pinned items loop on themselves (timer resets instead of advancing)
- If pinned item has no data or becomes server-skipped, blank the screen and keep requesting
- When data arrives for pinned item, display it immediately

### Server-Skipped Recovery
- When advancing and encountering a server-skipped item that now has data, clear the flag and display it
- Server-skipped items are retried: in prepare window (with timeout protection) and when advancing past them

## File Structure
- `scheduler.h/cpp`: All scheduling logic, render tracking, display timing
- `sockets.h/cpp`: Websocket communication, calls `scheduler_handle_render_response()` and `scheduler_set_schedule()`
- `sprites.h/cpp`: RAM storage for WebP data with mutex protection
- `display.h/cpp`: HUB75 LED matrix driver, `display_sprite()` and `display_clear()`

## Important Implementation Notes
- Render tracking is merged into scheduler (no separate render_requests module)
- Button logic is currently removed, will be added back later
- When receiving a new schedule while running, preserve existing sprite data for matching UUIDs
- Be careful with `existing == &slot` case when reprocessing same items in same slots

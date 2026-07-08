# esp_http_client (vendored from ESP-IDF v6.0.0)

**This is a patched copy of ESP-IDF's `esp_http_client` component.** Because it
lives in the project's `components/` directory, the build system uses it
instead of the copy in `$IDF_PATH` — no changes to the IDF install.

## Why it exists

ESP-IDF v6.0.0/v6.0.1 regression in `esp_http_client_connect()`
(`esp_http_client.c`): `client->state = HTTP_STATE_CONNECTING;` was placed
*before* the `if (client->state < HTTP_STATE_CONNECTED)` guard, so:

- keep-alive reuse never happened — every request dialed a brand-new TLS
  connection (full handshake, big transient internal-RAM spikes), and
- `esp_transport_connect()` overwrote the still-open `esp_tls` handle without
  closing it, leaking one socket fd plus TLS buffers per request.

`kd_http.c` shipped a workaround (force-close after every request) that stopped
the leak but still paid a full TLS handshake per render fetch — a major source
of internal-heap churn/fragmentation on this device.

## The patch

The upstream v6.0.2 fix backported, marked with `KOIOS FIX`: the state
assignment is removed from before the guard and placed in the async-connect
branch only. Everything else is byte-identical to IDF v6.0.0 (`test_apps/`
removed).

`CMakeLists.txt` additionally exports `KD_HTTP_CLIENT_CONNECT_FIX_BACKPORTED`
(INTERFACE define) which `kd_http.c` uses to compile out its close-per-request
workaround — keep-alive now actually works.

## When bumping ESP-IDF

A guard at the top of `esp_http_client.c` makes the build fail on any IDF
version other than 6.0.0.

- Moving to IDF >= 6.0.2: just delete this directory — the fix is upstream,
  and `kd_http.c`'s version gate handles the rest automatically.
- Moving to 6.0.1: re-copy from that IDF and re-apply the `KOIOS FIX` hunks.

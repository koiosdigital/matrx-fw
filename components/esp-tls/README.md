# esp-tls (vendored from ESP-IDF v6.0.0)

**This is a patched copy of ESP-IDF's `esp-tls` component.** Because it lives in
the project's `components/` directory, the build system uses it instead of the
copy in `$IDF_PATH` — no changes to the IDF install.

## Why it exists

ESP-IDF v6.0.x leaks one PSA key per mTLS connection attempt when the client
key comes from the DS peripheral (`CONFIG_ESP_TLS_USE_DS_PERIPHERAL=y`, i.e.
our websocket connection to vn-sec.koios.sh):

- `esp_mbedtls_init_pk_ctx_for_ds()` imports a volatile PSA key
  (`psa_import_key`, ~1.6 KB copy of the DS blob) before every handshake —
  including ones that go on to fail.
- `esp_mbedtls_cleanup()` only calls `psa_destroy_key()` when
  `mbedtls_pk_get_type(&tls->clientkey) == MBEDTLS_PK_RSASSA_PSS`. But
  `mbedtls_pk_wrap_psa()` installs `mbedtls_rsa_opaque_info`, whose type is
  `MBEDTLS_PK_OPAQUE`, so the condition is never true and the key is never
  destroyed. (`mbedtls_pk_free()` explicitly does not destroy wrapped PSA
  keys.)

Every websocket connect/reconnect therefore leaked a key permanently.
**Still broken in v6.0.2** (same condition). Found July 2026 during the
internal-heap-exhaustion investigation.

## The patch

One change, in `esp_tls_mbedtls.c` / `esp_mbedtls_cleanup()`: an added
`MBEDTLS_PK_OPAQUE` branch that destroys `clientkey`'s PSA key. Marked with
`KOIOS FIX`. Everything else is byte-identical to IDF v6.0.0
(`test_apps/` removed).

## When bumping ESP-IDF

A guard at the top of `esp_tls_mbedtls.c` makes the build fail on any IDF
version other than 6.0.0. When bumping IDF:

1. Check whether upstream fixed the cleanup (look for `MBEDTLS_PK_OPAQUE`
   handling in `esp_mbedtls_cleanup()`; consider reporting/upstreaming if not).
2. If fixed: delete this directory.
3. If not: re-copy the component from the new IDF, re-apply the `KOIOS FIX`
   hunk, update the version guard, and update this README.

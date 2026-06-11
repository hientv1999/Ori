// Ori chunked transfer reassembly engine.
// Implements the 6-byte frame header protocol from ble-protocol.md §5.

#include "ble/chunked_transfer.h"

#include <Arduino.h>
#include "ori_log.h"
#include <esp_heap_caps.h>
#include <string.h>

namespace chunked_transfer {

// ─── Feed one raw BLE write payload into ctx ─────────────────────────────

bool feed(Context* ctx, const uint8_t* data, uint16_t len) {
    if (!ctx || !data || len < 6) {
        LOG("[chunk] ERROR: frame too short\n");
        return false;
    }

    uint16_t seq_num     = (uint16_t)(data[0] | (data[1] << 8));
    uint16_t total_frags = (uint16_t)(data[2] | (data[3] << 8));
    uint16_t payload_len = (uint16_t)(data[4] | (data[5] << 8));

    if (payload_len + 6u > len) {
        LOG("[chunk] ERROR: payload_len %u > available %u\n",
                       (unsigned)payload_len, (unsigned)(len - 6));
        return false;
    }

    const uint8_t* payload = data + 6;

    // First fragment — allocate PSRAM buffer.
    if (seq_num == 0) {
        // Free any previous partial transfer.
        reset(ctx);

        ctx->total_frags  = total_frags;
        ctx->expected_seq = 0;
        ctx->received     = 0;
        ctx->active       = true;
        ctx->last_frag_ms = (uint32_t)millis();

        // Allocate a generous PSRAM buffer. We don't know the total size yet,
        // so allocate based on payload_len * total_frags as an upper bound.
        // The actual content won't exceed this; we'll track exact size via received.
        size_t max_size = (size_t)payload_len * total_frags;
        if (max_size == 0) max_size = 65536; // fallback
        ctx->buf = static_cast<uint8_t*>(
            heap_caps_malloc(max_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!ctx->buf) {
            LOG("[chunk] ERROR: PSRAM alloc failed for %u bytes\n",
                           (unsigned)max_size);
            ctx->active = false;
            if (ctx->on_complete) ctx->on_complete(nullptr, 0, "NACK_TOO_LARGE");
            return false;
        }
        ctx->buf_len = max_size;
    }

    // Check for gap.
    if (seq_num != ctx->expected_seq) {
        LOG("[chunk] gap: expected seq %u got %u\n",
                       (unsigned)ctx->expected_seq, (unsigned)seq_num);
        if (ctx->on_complete) {
            ctx->on_complete(nullptr, 0, "NACK_CHUNK_MISSING");
        }
        reset(ctx);
        return false;
    }

    // Bounds check before copy.
    if (ctx->received + payload_len > ctx->buf_len) {
        LOG("[chunk] ERROR: overflow in buf\n");
        if (ctx->on_complete) ctx->on_complete(nullptr, 0, "NACK_TOO_LARGE");
        reset(ctx);
        return false;
    }

    // Append fragment payload.
    memcpy(ctx->buf + ctx->received, payload, payload_len);
    ctx->received     += payload_len;
    ctx->expected_seq++;
    ctx->last_frag_ms  = (uint32_t)millis();

    LOG("[chunk] frag %u/%u  total_bytes=%u\n",
                   (unsigned)seq_num + 1, (unsigned)total_frags,
                   (unsigned)ctx->received);

    // Per-fragment progress callback (fires before on_complete on the last frag).
    if (ctx->on_fragment) {
        ctx->on_fragment(seq_num, total_frags, payload_len);
    }

    // Last fragment?
    if (seq_num == total_frags - 1) {
        uint8_t* complete_buf = ctx->buf;
        size_t   complete_len = ctx->received;

        // Detach from ctx before callback so reset() doesn't free the buffer.
        ctx->buf     = nullptr;
        ctx->buf_len = 0;
        ctx->active  = false;

        LOG("[chunk] complete: %u bytes\n", (unsigned)complete_len);

        if (ctx->on_complete) {
            ctx->on_complete(complete_buf, complete_len, nullptr);
        } else {
            // No callback — caller is responsible; but we can't leak it here.
            heap_caps_free(complete_buf);
        }
        reset(ctx);
    }

    return true;
}

// ─── Poll all contexts for timeouts ──────────────────────────────────────

int poll_timeouts(Context** ctxs, size_t count) {
    if (!ctxs || count == 0) return 0;
    int timed_out = 0;
    uint32_t now = (uint32_t)millis();
    for (size_t i = 0; i < count; ++i) {
        Context* ctx = ctxs[i];
        if (!ctx || !ctx->active) continue;
        uint32_t elapsed = now - ctx->last_frag_ms;
        if (elapsed >= TIMEOUT_MS) {
            LOG("[chunk] timeout ctx[%u] after %u ms\n",
                           (unsigned)i, (unsigned)elapsed);
            if (ctx->on_complete) ctx->on_complete(nullptr, 0, "NACK_CHUNK_TIMEOUT");
            reset(ctx);
            ++timed_out;
        }
    }
    return timed_out;
}

// ─── Reset a context to idle ──────────────────────────────────────────────

void reset(Context* ctx) {
    if (!ctx) return;
    if (ctx->buf) {
        heap_caps_free(ctx->buf);
        ctx->buf = nullptr;
    }
    ctx->buf_len      = 0;
    ctx->received     = 0;
    ctx->expected_seq = 0;
    ctx->total_frags  = 0;
    ctx->last_frag_ms = 0;
    ctx->active       = false;
}

} // namespace chunked_transfer

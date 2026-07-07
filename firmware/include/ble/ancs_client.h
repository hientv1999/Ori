#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Ori ANCS client — Apple Notification Center Service subscriber.
//
// ANCS is Apple-proprietary. Ori acts as a GATT client and subscribes to:
//   - Notification Source (NS) characteristic: new / modified / removed events
//   - Data Source (DS) characteristic: notification attribute responses
//
// Rules (connectivity.md, firmware.md):
//   - Notification ICONS only — never content or counts displayed on the icon
//   - Tapping an icon fires an ANCS GetNotificationAttributes request for the
//     Title and Body, which are shown in modal_ancs_notification
//   - MAX_ANCS_NOTIFICATIONS = 50 queue depth (PSRAM-backed detail store);
//     MAX_ANCS_ICONS = 5 visible
//
// This module also handles the BLE connection setup for the iPhone link
// (separate from the Orion link).

namespace ancs_client {

// Called once from ble_manager::init() — registers ANCS GATT client profiles
// with NimBLE. Does not start connection (that happens in advertising callbacks).
void init();

// Drain queued NS/DS notifications on the MAIN task. The NimBLE notify
// callbacks run on the host task and only enqueue raw bytes; this does the
// actual work (attribute requests, queue + status-bar updates), where blocking
// GATT writes and LVGL calls are safe. Call once per main-loop iteration.
// `orion_connected` gates the periodic iPhone CTS time-sync (skipped when
// Orion is connected, since Orion's own 10-min Time Sync takes priority).
void poll(bool orion_connected);

// Called when the iPhone BLE link is established and services are discovered.
// Subscribes to NS and DS characteristics.
void on_iphone_connected(uint16_t conn_handle);

// Called when iPhone disconnects — clears queue, notifies state machine.
void on_iphone_disconnected();

// Called from NimBLE notification callback with raw NS data (8 bytes).
void on_notification_source(const uint8_t* data, uint16_t len);

// Called from NimBLE notification callback with raw DS data (attribute response).
void on_data_source(const uint8_t* data, uint16_t len);

// Request attributes (Title, Body) for a given notification UID.
// Posts the ANCS GetNotificationAttributes command over BLE.
void request_attributes(uint32_t notif_uid);

// Dismiss a notification from the live queue by UID and send ANCS
// PerformNotificationAction(Negative) to clear it on the phone.
// Only call when the notification has a negative action (has_neg_action = true).
void dismiss_notification(uint32_t notif_uid);

// Remove a notification from Ori's queue without sending any ANCS action to
// the phone — used when the notification has no negative action available.
void drop_notification(uint32_t notif_uid);

// Accept/answer a notification's positive action via ANCS
// PerformNotificationAction(Positive) — used to answer an incoming call. Does
// NOT remove it from the queue: the call becomes active and stays until it ends.
void answer_notification(uint32_t notif_uid);

// Returns the current live queue as an array of icon tokens.
// Only the first MAX_ANCS_ICONS entries are displayed; the full queue is
// tracked up to MAX_ANCS_NOTIFICATIONS entries.
struct QueueEntry {
    uint32_t    uid;
    char        icon_token[32];
};

// The connected iPhone's device name (GAP Device Name characteristic,
// 0x1800/0x2A00 — e.g. "Xander's iPhone"). Read once per connection over
// the encrypted link; iOS only reveals the personalised name to bonded
// peers. Returns "" when not connected or the read failed. RAM only —
// cleared on disconnect.
const char* phone_name();

// ANCS notification filter level. Applied at DISPLAY TIME — all notifications are
// always stored internally so changing the filter instantly shows/hides them.
//   0x00  DISABLED   — store all; display none (no icons, no call modal, no badge animation)
//   0x01  CALL_ONLY  — display only CategoryID 1 (IncomingCall)
//   0x02  IMPORTANT  — display IncomingCall OR ANCS Important flag set
//   0x03  ALL        — display all (default)
// set_filter() is called from ble_manager::poll() after Orion writes Device Settings (char 000E);
// the value is also persisted to NVS (nvs::set_notif_filter) by that poll handler.
// set_filter() immediately calls publish_queue() to refresh the status bar.
void    set_filter(uint8_t level);
uint8_t get_filter();

} // namespace ancs_client

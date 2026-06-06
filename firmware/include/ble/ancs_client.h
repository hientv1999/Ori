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
//   - MAX_ANCS_NOTIFICATIONS = 20 queue depth; MAX_ANCS_ICONS = 5 visible
//
// This module also handles the BLE connection setup for the iPhone link
// (separate from the Orion link).

namespace ancs_client {

// Called once from ble_manager::init() — registers ANCS GATT client profiles
// with NimBLE. Does not start connection (that happens in advertising callbacks).
void init();

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

// Dismiss a notification from the live queue by UID (after user taps Close).
// Also sends ANCS PerformNotificationAction(Negative) to clear it on the phone.
void dismiss_notification(uint32_t notif_uid);

// Notification data made available after attributes arrive.
struct NotificationInfo {
    uint32_t    uid;
    char        app_id[128];      // bundle ID, e.g. "com.google.gmail.iphone"
    char        title[192];       // notification title
    char        body[512];        // notification body
    const char* icon_token;       // resolved icon token (see ancs_icons.h)
};

// Returns the info for the most recently received attributes response,
// or nullptr if no pending response. Caller should check after request_attributes()
// returns via the state machine event pump.
const NotificationInfo* pending_notification_info();

// Clear the pending notification info after the modal has displayed it.
void clear_pending_notification_info();

// Returns the current live queue as an array of icon tokens.
// Only the first MAX_ANCS_ICONS entries are displayed; the full queue is
// tracked up to MAX_ANCS_NOTIFICATIONS entries.
struct QueueEntry {
    uint32_t    uid;
    char        icon_token[32];
};

// Returns pointer to the internal queue array (do not free).
// count_out is filled with the current live entry count.
const QueueEntry* get_queue(size_t* count_out);

} // namespace ancs_client

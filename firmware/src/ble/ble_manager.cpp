// Ori BLE Manager — top-level NimBLE init, advertising, dual-connection orchestration.
//
// Manages:
//   - NimBLE stack initialization (GATT server + ANCS client coexisting)
//   - Advertising state machine per ble-protocol.md §2
//   - Bond slot enforcement (state-gated pairing)
//   - Passkey display events → screen_setup passkey modal
//   - Deferred event queue (runs LVGL/state-machine actions on Arduino main task)
//
// NimBLE version: h2zero/NimBLE-Arduino@2.5.0

#include "ble/ble_manager.h"
#include "ble/gatt_server.h"
#include "ble/ancs_client.h"

#include <Arduino.h>
#include "ori_log.h"
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEAdvertising.h>
#include <Preferences.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>

#include "nvs_store.h"
#include "nvs_sync.h"
#include "photo_cache.h"
#include "state_machine.h"
#include "factory_reset.h"
#include "screens/screen_setup.h"
#include "widgets/widget_profile_card.h"

// ── BLE device name ────────────────────────────────────────────────────────
// Format: Ori-XX-XX (last 2 bytes of BT MAC address, uppercase hex).
// If the MAC cannot be read, fall back to app_state's ble_name().
#include "app_state.h"

// ANCS service UUID (for advertising).
#define ANCS_SVC_UUID "7905F431-B5CE-4E99-A40F-4B1E122D00D0"
#define ORI_SVC_UUID  "6F726900-0000-4F72-9F00-000000000000"

// ── Deferred event types ───────────────────────────────────────────────────

enum class BleEventType : uint8_t {
    None = 0,
    FactoryReset,
    PresenceUpdate,
    AlbumArt,
    PhotoReceived,      // profile photo JPEG — forward to photo_cache::store()
    PtoPhotoReceived,   // PTO destination JPEG (or len=0 = no photo)
    PasskeyDisplay,
    AuthFailed,        // pairing handshake failed — hide passkey modal
    SyncBegin,         // SyncControl{op:"BEGIN"} — show orioning modal
    SyncEnd,           // SyncControl{op:"END"}   — advance setup / dismiss reconnect overlay
    OrioningProgress,  // sync milestone reached  — update progress ring (0–100)
    OrionConnected,
    OrionBonded,
    OrionDisconnected,
    IphoneConnected,
    IphoneBonded,
    IphoneDisconnected,
};

struct BleEvent {
    BleEventType type;
    union {
        uint32_t passkey;
        widget_profile_card::Presence presence;
        struct { uint8_t* buf; size_t len; } art;
        uint16_t conn_handle;
        uint8_t  pct;   // OrioningProgress
    } data;
    uint8_t peer_addr[6]; // populated for bonded events
};

// ── Event queue (NimBLE task → Arduino main task) ─────────────────────────
// Simple lock-free ring buffer sized for the maximum burst of BLE events.

constexpr size_t EVENT_QUEUE_SIZE = 16;
static BleEvent  s_event_queue[EVENT_QUEUE_SIZE];
static volatile size_t s_eq_head = 0; // consumer (poll)
static volatile size_t s_eq_tail = 0; // producer (NimBLE callbacks)

static void eq_push(const BleEvent& e) {
    size_t next = (s_eq_tail + 1) % EVENT_QUEUE_SIZE;
    if (next == s_eq_head) {
        LOG("[ble] event queue FULL — event dropped\n");
        return;
    }
    s_event_queue[s_eq_tail] = e;
    s_eq_tail = next;
}

static bool eq_pop(BleEvent& e) {
    if (s_eq_head == s_eq_tail) return false;
    e = s_event_queue[s_eq_head];
    s_eq_head = (s_eq_head + 1) % EVENT_QUEUE_SIZE;
    return true;
}

// ── Module state ───────────────────────────────────────────────────────────

namespace {

std::string g_ble_name;           // set once in init(), reused in restart_advertising()
bool     g_orion_connected  = false;
bool     g_iphone_connected = false;
uint16_t g_orion_conn       = BLE_HS_CONN_HANDLE_NONE;
uint16_t g_iphone_conn      = BLE_HS_CONN_HANDLE_NONE;

// Current passkey for display.
uint32_t g_passkey = 0;

// Flag for deferred restart (factory reset from BLE callback).
volatile bool g_restart_pending = false;

// Preferences handle for bond address storage.
Preferences g_prefs;

// Current setup sub-state for bond slot gating.
// Returns true when Orion bonding is allowed.
bool is_orion_pairing_allowed() {
    // Allowed only during Setup Step 2 (pairing sub-state).
    // We check if the firmware is currently on the SETUP screen, not yet provisioned.
    return nvs::is_first_boot() &&
           (state_machine::current_state() == AppState::SETUP);
}

// Returns true when iPhone bonding is allowed.
bool is_iphone_pairing_allowed() {
    AppState s = state_machine::current_state();
    // Allowed during Setup Step 4 (phone pairing) or the runtime re-pair-phone flow.
    // In M5, setup is driven by Device Status advancing. We allow iPhone bonding
    // whenever the device is either in Setup (past Step 3) or Runtime.
    return (s == AppState::SETUP ||
            s == AppState::MEETING_LIST ||
            s == AppState::NO_MEETINGS ||
            s == AppState::CLOCK);
}

} // namespace

// ── NimBLE Server callbacks ────────────────────────────────────────────────

class OriServerCallbacks : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
        uint16_t handle = info.getConnHandle();
        LOG("[ble] peer connected handle=%u addr=%s\n",
                       (unsigned)handle,
                       info.getAddress().toString().c_str());

        // Request maximum MTU.
        NimBLEDevice::setMTU(247);

        // Determine which slot this connection belongs to by checking stored addresses.
        uint8_t orion_addr[6]  = {};
        uint8_t iphone_addr[6] = {};
        ble_manager::load_bond_addr(ble_manager::BOND_KEY_ORION,  orion_addr);
        ble_manager::load_bond_addr(ble_manager::BOND_KEY_IPHONE, iphone_addr);

        // NimBLE 2.5: use getVal() to get the raw address bytes.
        const uint8_t* peer_raw = info.getAddress().getVal();

        if (!ble_manager::is_bond_slot_empty(orion_addr) &&
            memcmp(peer_raw, orion_addr, 6) == 0) {
            // Known Orion reconnecting.
            LOG("[ble] Orion reconnected (bonded peer)\n");
            g_orion_connected = true;
            g_orion_conn      = handle;
            BleEvent ev = {};
            ev.type = BleEventType::OrionConnected;
            ev.data.conn_handle = handle;
            eq_push(ev);
        } else if (!ble_manager::is_bond_slot_empty(iphone_addr) &&
                   memcmp(peer_raw, iphone_addr, 6) == 0) {
            // Known iPhone reconnecting.
            LOG("[ble] iPhone reconnected (bonded peer)\n");
            g_iphone_connected = true;
            g_iphone_conn      = handle;
            BleEvent ev = {};
            ev.type = BleEventType::IphoneConnected;
            ev.data.conn_handle = handle;
            eq_push(ev);
        } else {
            // Unknown peer — fresh connection, awaiting pairing.
            // Passkey modal is shown via onPassKeyDisplay() when NimBLE begins
            // the passkey exchange — not here on connect.
            LOG("[ble] unknown peer — awaiting bond\n");
        }
    }

    void onDisconnect(NimBLEServer* server,
                      NimBLEConnInfo& info, int reason) override {
        uint16_t handle = info.getConnHandle();
        LOG("[ble] peer disconnected handle=%u reason=%d\n",
                       (unsigned)handle, reason);

        if (handle == g_orion_conn) {
            g_orion_connected = false;
            g_orion_conn      = BLE_HS_CONN_HANDLE_NONE;
            BleEvent ev = {};
            ev.type = BleEventType::OrionDisconnected;
            eq_push(ev);
        } else if (handle == g_iphone_conn) {
            g_iphone_connected = false;
            g_iphone_conn      = BLE_HS_CONN_HANDLE_NONE;
            BleEvent ev = {};
            ev.type = BleEventType::IphoneDisconnected;
            eq_push(ev);
        }

        // Restart advertising.
        ble_manager::restart_advertising();
    }

    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        uint16_t handle = info.getConnHandle();
        LOG("[ble] auth complete handle=%u bonded=%d\n",
                       (unsigned)handle, (int)info.isBonded());

        if (!info.isBonded()) {
            LOG("[ble] auth failed — disconnecting, hiding passkey modal\n");
            NimBLEDevice::getServer()->disconnect(handle);
            BleEvent ev = {};
            ev.type = BleEventType::AuthFailed;
            eq_push(ev);
            return;
        }

        // NimBLE 2.5: use getVal() to get the raw address bytes.
        const uint8_t* peer_raw = info.getAddress().getVal();

        // Determine which slot to assign.
        uint8_t orion_addr[6]  = {};
        uint8_t iphone_addr[6] = {};
        ble_manager::load_bond_addr(ble_manager::BOND_KEY_ORION,  orion_addr);
        ble_manager::load_bond_addr(ble_manager::BOND_KEY_IPHONE, iphone_addr);

        bool orion_empty  = ble_manager::is_bond_slot_empty(orion_addr);
        bool iphone_empty = ble_manager::is_bond_slot_empty(iphone_addr);

        if (orion_empty && is_orion_pairing_allowed()) {
            // New Orion bond — Step 2.
            ble_manager::on_orion_bonded(handle, peer_raw);
        } else if (iphone_empty && is_iphone_pairing_allowed()) {
            // New iPhone bond — Step 4 / re-pair.
            ble_manager::on_iphone_bonded(handle, peer_raw);
        } else {
            // Bond slots full or state doesn't allow bonding.
            LOG("[ble] bond rejected: slots full or wrong state\n");
            NimBLEDevice::getServer()->disconnect(handle);
        }
    }

    uint32_t onPassKeyDisplay() override {
        // Regenerate passkey on every bonding attempt — fresh code each time.
        uint32_t r = esp_random();
        g_passkey = r % 1000000;
        if (g_passkey < 100000) g_passkey += 100000;

        BleEvent ev = {};
        ev.type         = BleEventType::PasskeyDisplay;
        ev.data.passkey = g_passkey;
        eq_push(ev);
        return g_passkey;
    }

    void onConfirmPassKey(NimBLEConnInfo& info, uint32_t pin) override {
        // LE Secure Connections numeric comparison — always confirm.
        // User verifies the code matches on the Orion PC side.
        NimBLEDevice::injectConfirmPasskey(info, true);
    }
};

static OriServerCallbacks s_server_cb;

// ── Advertising helpers ────────────────────────────────────────────────────

namespace {

static std::string make_ble_name() {
    // Read BT MAC via IDF — safe to call before NimBLEDevice::init().
    // esp_read_mac returns bytes MSB-first: mac[0]=MSB, mac[5]=LSB.
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_BT);
    char name[16];
    snprintf(name, sizeof(name), "Ori-%02X-%02X",
             (unsigned)mac[4], (unsigned)mac[5]);
    return std::string(name);
}

} // namespace

// ── ble_manager public API ─────────────────────────────────────────────────

namespace ble_manager {

void init() {
    LOG("[ble] init\n");

    // g_passkey starts at 0; regenerated fresh in onPassKeyDisplay() on each
    // bonding attempt so every attempt shows a unique code.

    g_ble_name = make_ble_name();
    NimBLEDevice::init(g_ble_name);
    NimBLEDevice::setMTU(247);

    // Security: LE Secure Connections, Passkey Entry (IO_CAP_DISP_ONLY).
    NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_SC |
                                   BLE_SM_PAIR_AUTHREQ_BOND |
                                   BLE_SM_PAIR_AUTHREQ_MITM);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    // Do NOT call setSecurityPasskey() here — that makes NimBLE use the value
    // directly and skip the onPassKeyDisplay() callback entirely. Instead, g_passkey
    // is returned from onPassKeyDisplay() so the modal fires at the right moment.

    // Create GATT server and register all characteristics.
    gatt_server::init();

    // Register server connection callbacks.
    NimBLEServer* server = NimBLEDevice::getServer();
    server->setCallbacks(&s_server_cb, /*deleteCallbacks=*/false);

    // In NimBLE 2.5, call server->start() to activate GATT service registration.
    server->start();

    // Initialise ANCS client (registers GATT client profiles).
    ancs_client::init();

    // Start advertising based on current bond state.
    restart_advertising();

    LOG("[ble] device name: %s  passkey: %06u\n",
                   g_ble_name.c_str(), (unsigned)g_passkey);
}

void poll() {
    BleEvent ev;
    while (eq_pop(ev)) {
        switch (ev.type) {

            case BleEventType::FactoryReset:
                // Deferred from BLE callback — now safe to wipe NVS and restart.
                factory_reset::execute();
                break;

            case BleEventType::PresenceUpdate: {
                // Update the profile card widget on the current screen.
                // widget_profile_card::set_default_presence already called; no-op needed
                // unless we want to update a live screen's profile card directly.
                widget_profile_card::set_default_presence(ev.data.presence);
                // Trigger a state machine re-evaluate so the newly loaded screen
                // (if any) picks up the new presence in apply_widget_defaults.
                // We call g_force_rebuild-style: post a minimal state change that
                // keeps the current state but refreshes the profile card.
                // state_machine::evaluate() will call apply_widget_defaults() again.
                // For a live already-loaded screen, we need the card reference.
                // In M5 we keep it simple: the next screen load will use the new default.
                break;
            }

            case BleEventType::AlbumArt:
                // Album art JPEG received — post to media mode screen.
                // The buf was allocated in PSRAM by chunked_transfer.
                // Screen will decode via LVGL TJPGD.
                // TODO (M7 integration): call screen_media_mode::set_album_art().
                if (ev.data.art.buf) {
                    LOG("[ble] AlbumArt received %u bytes\n",
                                   (unsigned)ev.data.art.len);
                    // For now, free the buffer — art display wired at M7 integration.
                    heap_caps_free(ev.data.art.buf);
                }
                break;

            case BleEventType::PhotoReceived:
                // photo_cache::store() takes ownership of buf and frees it.
                LOG("[ble] PhotoReceived: %u bytes\n", (unsigned)ev.data.art.len);
                photo_cache::store(ev.data.art.buf, ev.data.art.len);
                break;

            case BleEventType::PtoPhotoReceived:
                // len == 0 means no destination photo set — store_pto clears cache.
                LOG("[ble] PtoPhotoReceived: %u bytes\n", (unsigned)ev.data.art.len);
                photo_cache::store_pto(ev.data.art.buf, ev.data.art.len);
                break;

            case BleEventType::PasskeyDisplay:
                LOG("[ble] passkey display: %06u\n", (unsigned)ev.data.passkey);
                screen_setup::show_passkey_modal(lv_scr_act(), ev.data.passkey);
                break;

            case BleEventType::AuthFailed:
                // Hide the passkey modal — restores the Pairing base screen
                // (BLE name + spinner) so the user can retry pairing.
                screen_setup::hide_passkey_modal(lv_scr_act());
                break;

            case BleEventType::OrionConnected:
                LOG("[ble:poll] Orion connected — begin reconnect sync\n");
                state_machine::set_pc_connected(true);
                state_machine::on_reconnect_begin();
                // The GATT Manifest flow will call on_reconnect_end() when done.
                // Set Device Status to RUNTIME_RECONNECTING.
                gatt_server::set_device_status(0x11); // RUNTIME_RECONNECTING
                // Read Presence Status from Orion (source of truth on reconnect).
                // Orion will push it; until then show Offline.
                widget_profile_card::set_default_presence(
                    widget_profile_card::Presence::Offline);
                break;

            case BleEventType::IphoneConnected:
                LOG("[ble:poll] iPhone connected — starting ANCS\n");
                ancs_client::on_iphone_connected(ev.data.conn_handle);
                break;

            case BleEventType::OrionBonded:
                LOG("[ble:poll] Orion bonded\n");
                g_orion_connected = true;
                g_orion_conn      = ev.data.conn_handle;
                state_machine::set_pc_connected(true);
                gatt_server::set_device_status(0x01); // SETUP_BONDED_AWAITING_SYNC
                // Bonding complete — hide passkey modal, return to Pairing base screen
                // (BLE name + spinner). Orioning modal appears on SyncBegin below.
                screen_setup::hide_passkey_modal(lv_scr_act());
                break;

            case BleEventType::SyncBegin:
                LOG("[ble:poll] sync begin — showing orioning modal\n");
                screen_setup::show_orioning_modal(lv_scr_act());
                break;

            case BleEventType::SyncEnd:
                LOG("[ble:poll] sync end\n");
                if (nvs::is_first_boot()) {
                    // Complete the progress ring before advancing.
                    screen_setup::update_orioning_progress(lv_scr_act(), 100);
                    screen_setup::hide_orioning_modal(lv_scr_act());
                    screen_setup::set_step(lv_scr_act(), screen_setup::Step::PhonePairing);
                } else {
                    state_machine::on_reconnect_end();
                }
                break;

            case BleEventType::OrioningProgress:
                screen_setup::update_orioning_progress(lv_scr_act(), ev.data.pct);
                break;

            case BleEventType::IphoneBonded:
                LOG("[ble:poll] iPhone bonded\n");
                g_iphone_connected = true;
                g_iphone_conn      = ev.data.conn_handle;
                state_machine::set_phone_connected(true);
                // Restart advertising as directed-only now that both slots are filled.
                restart_advertising();
                break;

            case BleEventType::OrionDisconnected:
                LOG("[ble:poll] Orion disconnected\n");
                state_machine::set_pc_connected(false);
                // Force presence to Offline immediately (never show stale presence).
                widget_profile_card::set_default_presence(
                    widget_profile_card::Presence::Offline);
                gatt_server::set_device_status(0xF0); // ERROR_GENERIC until reconnect
                break;

            case BleEventType::IphoneDisconnected:
                LOG("[ble:poll] iPhone disconnected\n");
                ancs_client::on_iphone_disconnected();
                break;

            default:
                break;
        }
    }

    // Factory-reset deferred restart (from remote BLE factory-reset command
    // or from the local long-press path if it posted to the event queue).
    if (g_restart_pending) {
        g_restart_pending = false;
        LOG("[ble] executing deferred factory reset\n");
        nvs::factory_reset();
        delay(200);
        ESP.restart();
    }
}

void quiesce_for_commit() {
    LOG("[ble] quiescing stack for OTA flash commit\n");
    // Stop advertising first so no new connection/bond (and its NVS flash write)
    // can begin, then tear down host + controller. deinit(false) leaves bonds in
    // NVS untouched; the post-commit reboot re-inits BLE from scratch.
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv) adv->stop();
    NimBLEDevice::deinit(false);
}

void restart_advertising() {
    NimBLEServer* server = NimBLEDevice::getServer();
    if (!server) return;

    // Stop any active advertising first.
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->stop();

    uint8_t orion_addr[6]  = {};
    uint8_t iphone_addr[6] = {};
    load_bond_addr(BOND_KEY_ORION,  orion_addr);
    load_bond_addr(BOND_KEY_IPHONE, iphone_addr);

    bool orion_bonded  = !is_bond_slot_empty(orion_addr);
    bool iphone_bonded = !is_bond_slot_empty(iphone_addr);

    if (orion_bonded && iphone_bonded) {
        // Both slots full — directed advertising to Orion (primary sync peer).
        // NimBLE 2.5 directed advertising: setConnectableMode(DIR) + pass address to start().
        // We prioritise Orion; iPhone re-bonds on its own reconnect timer.
        LOG("[ble] adv: directed (both bonded)\n");
        adv->reset();
        adv->setConnectableMode(BLE_GAP_CONN_MODE_DIR);
        // Determine the address type from stored bond — use PUBLIC as stored.
        NimBLEAddress orion_ble_addr(orion_addr, BLE_ADDR_PUBLIC);
        adv->start(0, &orion_ble_addr);
        return;
    }

    // Public undirected advertising.
    // 31-byte adv packet budget:
    //   Flags (auto)   3 B
    //   Ori Sync UUID 18 B  (128-bit)
    //   Mfr data       5 B  (type + 0xFF 0xFF + flag)
    //   Total         26 B  ✓
    //
    // 31-byte scan-response budget:
    //   Device name   11 B  ("Ori-XX-XX" = 9 chars + 2 overhead)
    //   ANCS UUID     18 B  (128-bit)
    //   Total         29 B  ✓
    //
    // Both Orion (active scan) and iPhone (active scan for ANCS) read scan responses.
    adv->reset();
    adv->setConnectableMode(BLE_GAP_CONN_MODE_UND);

    // Adv packet: Ori Sync UUID + manufacturer mode flag.
    adv->addServiceUUID(ORI_SVC_UUID);
    uint8_t mfr_flag = (orion_bonded) ? ADV_FLAG_RUNTIME : ADV_FLAG_SETUP;
    uint8_t mfr_data[3] = { 0xFF, 0xFF, mfr_flag };
    adv->setManufacturerData(std::string((char*)mfr_data, 3));

    // Scan response: device name + ANCS UUID.
    NimBLEAdvertisementData scanData;
    scanData.setName(g_ble_name.c_str());
    scanData.addServiceUUID(ANCS_SVC_UUID);
    adv->setScanResponseData(scanData);

    // Advertising interval.
    uint32_t interval_ms = orion_bonded ? 1000 : 100;
    adv->setMinInterval((uint16_t)(interval_ms * 1000 / 625));
    adv->setMaxInterval((uint16_t)(interval_ms * 1000 / 625));

    adv->start();
    LOG("[ble] adv: public undirected, flag=0x%02X interval=%u ms\n",
                   (unsigned)mfr_flag, (unsigned)interval_ms);
}

void load_bond_addr(const char* key, uint8_t out_addr[6]) {
    memset(out_addr, 0, 6);
    // readOnly=false creates the namespace on first access; readOnly=true returns
    // NOT_FOUND on a fresh device and logs a spurious error.
    if (g_prefs.begin(BOND_NS, /*readOnly=*/false)) {
        g_prefs.getBytes(key, out_addr, 6);
        g_prefs.end();
    }
}

void save_bond_addr(const char* key, const uint8_t addr[6]) {
    if (g_prefs.begin(BOND_NS, /*readOnly=*/false)) {
        g_prefs.putBytes(key, addr, 6);
        g_prefs.end();
    }
}

bool is_bond_slot_empty(const uint8_t addr[6]) {
    for (int i = 0; i < 6; ++i) {
        if (addr[i] != 0) return false;
    }
    return true;
}

void wipe_all_bonds() {
    LOG("[ble] wiping all bonds\n");
    // Clear NVS bond addresses.
    const uint8_t zero[6] = {};
    save_bond_addr(BOND_KEY_ORION,  zero);
    save_bond_addr(BOND_KEY_IPHONE, zero);

    // Clear all NimBLE bond records.
    NimBLEDevice::deleteAllBonds();

    // Also clear any GATT stored data.
    g_orion_connected  = false;
    g_iphone_connected = false;
    g_orion_conn       = BLE_HS_CONN_HANDLE_NONE;
    g_iphone_conn      = BLE_HS_CONN_HANDLE_NONE;
}

void wipe_iphone_bond() {
    LOG("[ble] wiping iPhone bond\n");
    uint8_t iphone_addr[6] = {};
    load_bond_addr(BOND_KEY_IPHONE, iphone_addr);

    if (!is_bond_slot_empty(iphone_addr)) {
        NimBLEAddress addr(iphone_addr, BLE_ADDR_PUBLIC);
        NimBLEDevice::deleteBond(addr);
    }

    const uint8_t zero[6] = {};
    save_bond_addr(BOND_KEY_IPHONE, zero);

    g_iphone_connected = false;
    g_iphone_conn      = BLE_HS_CONN_HANDLE_NONE;
}

bool is_orion_connected()  { return g_orion_connected;  }
bool is_iphone_connected() { return g_iphone_connected; }
uint16_t orion_conn_handle() { return g_orion_conn; }

void on_orion_bonded(uint16_t conn_handle, const uint8_t peer_addr[6]) {
    LOG("[ble] Orion bonded addr=%02X:%02X:%02X:%02X:%02X:%02X\n",
                   peer_addr[5], peer_addr[4], peer_addr[3],
                   peer_addr[2], peer_addr[1], peer_addr[0]);
    save_bond_addr(BOND_KEY_ORION, peer_addr);
    g_orion_connected = true;
    g_orion_conn      = conn_handle;

    BleEvent ev = {};
    ev.type = BleEventType::OrionBonded;
    ev.data.conn_handle = conn_handle;
    memcpy(ev.peer_addr, peer_addr, 6);
    eq_push(ev);
}

void on_iphone_bonded(uint16_t conn_handle, const uint8_t peer_addr[6]) {
    LOG("[ble] iPhone bonded addr=%02X:%02X:%02X:%02X:%02X:%02X\n",
                   peer_addr[5], peer_addr[4], peer_addr[3],
                   peer_addr[2], peer_addr[1], peer_addr[0]);
    save_bond_addr(BOND_KEY_IPHONE, peer_addr);
    g_iphone_connected = true;
    g_iphone_conn      = conn_handle;

    BleEvent ev = {};
    ev.type = BleEventType::IphoneBonded;
    ev.data.conn_handle = conn_handle;
    memcpy(ev.peer_addr, peer_addr, 6);
    eq_push(ev);
}

void on_peer_disconnect(uint16_t conn_handle, int reason) {
    // Called from the server callback — already handled in OriServerCallbacks::onDisconnect.
    (void)conn_handle;
    (void)reason;
}

void on_passkey_display(uint32_t passkey) {
    LOG("[ble] passkey: %06u\n", (unsigned)passkey);
    BleEvent ev = {};
    ev.type       = BleEventType::PasskeyDisplay;
    ev.data.passkey = passkey;
    eq_push(ev);
}

void notify_device_status(uint8_t status_byte) {
    gatt_server::set_device_status(status_byte);
}

} // namespace ble_manager

// ── Functions called from gatt_server.cpp (forward-declared there) ─────────

// Deferred factory reset event (from remote BLE Factory Reset Command).
void ble_post_factory_reset_event() {
    g_restart_pending = true;
    // Also wipe NVS in the main loop context before restart.
    // The event pump in ble_manager::poll() handles the actual wipe+restart.
}

// Deferred presence update (from PresenceStatus write handler).
void ble_post_presence_event(widget_profile_card::Presence p) {
    BleEvent ev = {};
    ev.type = BleEventType::PresenceUpdate;
    ev.data.presence = p;
    eq_push(ev);
}

// Deferred album art delivery (from AlbumArt chunked callback).
void ble_post_album_art_event(uint8_t* buf, size_t len) {
    BleEvent ev = {};
    ev.type         = BleEventType::AlbumArt;
    ev.data.art.buf = buf;
    ev.data.art.len = len;
    eq_push(ev);
}

// Deferred profile photo delivery (from Photo chunked callback).
// photo_cache::store() takes ownership of buf and frees it after decode.
void ble_post_photo_event(uint8_t* buf, size_t len) {
    BleEvent ev = {};
    ev.type         = BleEventType::PhotoReceived;
    ev.data.art.buf = buf;  // reuse art union member — same shape
    ev.data.art.len = len;
    eq_push(ev);
}

// Deferred PTO destination image (len==0 = no photo set; store_pto clears cache).
void ble_post_pto_photo_event(uint8_t* buf, size_t len) {
    BleEvent ev = {};
    ev.type         = BleEventType::PtoPhotoReceived;
    ev.data.art.buf = buf;
    ev.data.art.len = len;
    eq_push(ev);
}

// Deferred SyncControl BEGIN — show orioning modal on main task.
void ble_post_sync_begin_event() {
    BleEvent ev = {};
    ev.type = BleEventType::SyncBegin;
    eq_push(ev);
}

// Deferred SyncControl END — advance setup step or dismiss reconnect overlay.
void ble_post_sync_end_event() {
    BleEvent ev = {};
    ev.type = BleEventType::SyncEnd;
    eq_push(ev);
}

// Deferred sync milestone — update orioning progress ring on main task.
void ble_post_orioning_progress(uint8_t pct) {
    BleEvent ev = {};
    ev.type     = BleEventType::OrioningProgress;
    ev.data.pct = pct;
    eq_push(ev);
}

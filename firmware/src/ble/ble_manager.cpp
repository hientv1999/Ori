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
#include "screens/screen_reconnect_syncing.h"
#include "screens/screen_media_mode.h"
#include "widgets/widget_profile_card.h"

// ── BLE device name ────────────────────────────────────────────────────────
// Format: Ori-XX-XX (last 2 bytes of BT MAC, uppercase hex). The canonical
// builder is app_state::ble_name(); init() copies it into g_ble_name so the
// advertised name and the on-screen pairing pill share one source.
#include "app_state.h"

// ANCS service UUID (for advertising).
#define ANCS_SVC_UUID "7905F431-B5CE-4E99-A40F-4B1E122D00D0"
#define ORI_SVC_UUID  "6F726900-0000-4F72-9F00-000000000000"

// ── Deferred event types ───────────────────────────────────────────────────

enum class BleEventType : uint8_t {
    None = 0,
    FactoryReset,
    PresenceUpdate,
    MediaMetaUpdated,  // MediaMetadata write applied to app_state — repaint media screen
    AlbumArt,
    PhotoReceived,      // profile photo JPEG — forward to photo_cache::store()
    PtoPhotoReceived,   // PTO destination JPEG (or len=0 = no photo)
    PasskeyDisplay,
    AuthFailed,        // pairing handshake failed — hide passkey modal
    SyncBegin,         // SyncControl{op:"BEGIN"} — show orioning modal
    SyncCommit,        // SyncControl{op:"END"}   — apply staged data to NVS/UI (heavy; main task only)
    SyncEnd,           // staged commit done       — advance setup / dismiss reconnect overlay
    OrioningProgress,  // sync milestone reached  — update progress ring (0–100)
    OrionConnected,
    OrionBonded,         // provisional bond formed (passkey OK) — UI only, NOT persisted
    OrionConfirmed,      // bonded peer proved it's Orion (valid SyncControl{BEGIN}) — persist
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
        uint8_t  pct;            // OrioningProgress
        bool     light_refresh;  // SyncEnd — see ble_post_sync_end_event()
        uint32_t total_bytes;    // SyncBegin — declared total from SyncControl{BEGIN}
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

// Deferred iPhone bond wipe. Deleting a bond + writing NVS while the iPhone
// link is still live corrupts NVS state (crash inside the next Preferences
// open). When unpairing a CONNECTED iPhone we disconnect first, stash the
// address here, and finish the delete+NVS write on the IphoneDisconnected event
// (main task, link down). See wipe_iphone_bond() / poll() drain.
bool    g_iphone_wipe_pending  = false;
uint8_t g_iphone_wipe_addr[6]  = {};

// Current passkey for display.
uint32_t g_passkey = 0;

// Minimum SyncControl{BEGIN} declared total (bytes) to show the runtime
// "Refreshing your day" overlay (see SyncBegin handling in poll()). Time Sync
// + Shortcut Config alone — sent unconditionally on every periodic refresh,
// ble-protocol.md §6.3 — run well under 100 bytes combined; any sync that
// also carries Profile/Photo/Meetings/PTO is comfortably over this.
constexpr uint32_t RECONNECT_OVERLAY_MIN_BYTES = 200;

// Flag for deferred restart (factory reset from BLE callback).
volatile bool g_restart_pending = false;

// Set true by quiesce_for_commit() right before the OTA flash commit tears the
// BLE stack down. While set, onDisconnect() must NOT restart advertising or
// touch NVS — we are seconds from reboot, and re-entering the stack (or racing
// the flash write) is exactly what crashes the commit.
volatile bool g_quiescing = false;

// True while the iPhone-pairing UI is on screen (Setup Step 4 or runtime
// re-pair). Gates whether the ANCS service UUID is included in public
// undirected advertising — see set_iphone_pairing_window() / restart_advertising().
bool g_iphone_pairing_window = false;

// Orion-bond handshake gating. A peer that completes passkey bonding during
// Step 2 is only PROVISIONALLY Orion: its address is held in RAM (not persisted
// to the orion_addr slot) until it proves it speaks the Ori protocol by writing
// a valid SyncControl{BEGIN} (confirm_orion_peer()). A peer that bonds but never
// sends BEGIN within ORION_HANDSHAKE_TIMEOUT_MS — a stray phone or non-Orion PC
// someone paired off Ori's passkey screen — is dropped, never saved as Orion.
constexpr uint32_t ORION_HANDSHAKE_TIMEOUT_MS = 5000;
volatile bool g_orion_provisional         = false;
uint8_t       g_provisional_orion_addr[6] = {};
uint16_t      g_provisional_orion_conn    = 0xFFFF; // BLE_HS_CONN_HANDLE_NONE
uint32_t      g_orion_handshake_deadline  = 0;

// "Connect on Orion" minimum-linger (setup only). When the passkey modal closes
// on bonding, the "Connect on Orion" base screen (spinner) is revealed. Orion
// can send SyncControl{BEGIN} almost immediately after, and poll() drains all
// queued BLE events before rendering — so without this, the Orioning modal could
// cover the base screen in the same frame and the user would never see it. We
// stamp the bond time and defer the Orioning modal until CONNECT_ORION_MIN_MS
// has elapsed, guaranteeing the "Connect on Orion" screen is visible for a beat.
constexpr uint32_t CONNECT_ORION_MIN_MS = 700;
uint32_t g_orion_bonded_ms   = 0;
bool     g_orioning_pending  = false;

// Preferences handle for bond address storage. ONLY touched on the main task
// (prime_bond_cache at boot + save/wipe drained through poll()). The NimBLE
// host-task callbacks never open it — see the RAM cache below.
Preferences g_prefs;

// RAM-cached bond addresses. Primed once at boot by prime_bond_cache() and kept
// in sync by save_bond_addr()/wipe_*. ALL hot-path reads (onConnect,
// onAuthenticationComplete, restart_advertising — which run on the NimBLE host
// task) read these instead of opening NVS. Opening the shared Preferences
// handle from the host task races the main task's NVS access and corrupts it,
// crashing deep in the NVS partition manager (the LoadProhibited seen on a
// power-cycle reconnect when both peers are bonded).
uint8_t g_orion_addr_cache[6]  = {};
uint8_t g_iphone_addr_cache[6] = {};

// Current setup sub-state for bond slot gating.
// Returns true when Orion bonding is allowed.
bool is_orion_pairing_allowed() {
    // Allowed only during Setup Step 2 (pairing sub-state).
    // We check if the firmware is currently on the SETUP screen, not yet provisioned.
    return nvs::is_first_boot() &&
           (state_machine::current_state() == AppState::SETUP);
}

// Returns true when a NEW iPhone bond may be accepted.
//
// ONLY while the iPhone pairing window is open — i.e. the PhonePairing screen is
// on display (Setup Step 4 or the runtime re-pair flow, both toggle the window
// via set_iphone_pairing_window()). Outside that window, reject new iPhone
// bonds: an unpaired iPhone that still has Ori bonded on ITS side auto-reconnects
// and would otherwise silently re-pair, so the unpair never sticks. This matches
// the state-gated bond policy in ble-protocol.md §2. (Reconnects of the
// already-bonded iPhone go through the conn_matches() branch above and are not
// affected by this gate.)
bool is_iphone_pairing_allowed() {
    return g_iphone_pairing_window;
}

} // namespace

// ── Peer identity matching ─────────────────────────────────────────────────
// Match a connection against a stored bond address by the RESOLVED IDENTITY
// address (peer_id_addr), with the over-the-air address (peer_ota_addr) as a
// fallback. A bonded peer that uses address privacy (notably iPhone, but also
// many PCs) reconnects with a *different* OTA address each time while its
// identity stays constant — so matching the OTA address fails on reconnect and
// the peer looks "unknown". Matching the identity is what makes reconnection
// reliable. (getIdAddress()/getAddress() return temporaries; only deref inline.)
static bool conn_matches(NimBLEConnInfo& info, const uint8_t addr[6]) {
    if (ble_manager::is_bond_slot_empty(addr)) return false;
    return memcmp(info.getIdAddress().getVal(), addr, 6) == 0 ||
           memcmp(info.getAddress().getVal(),   addr, 6) == 0;
}

// Address to persist for a NEW bond: the resolved identity (stable across
// reconnects), falling back to the OTA address only if no identity was exchanged.
static const uint8_t* bond_addr_to_store(NimBLEConnInfo& info) {
    static uint8_t buf[6];
    memcpy(buf, info.getIdAddress().getVal(), 6);  // temporary valid during memcpy
    bool id_empty = true;
    for (int i = 0; i < 6; ++i) if (buf[i]) { id_empty = false; break; }
    if (id_empty) memcpy(buf, info.getAddress().getVal(), 6);
    return buf;
}

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

        // Throughput tuning for bulk sync (photo, meetings, PTO, album art):
        //   • Data Length Extension — 251-octet LL PDUs so a full 247-byte ATT
        //     write rides in a single radio packet (less per-packet overhead).
        //   • Faster connection-interval range (15–30 ms vs the conservative
        //     default) → more connection events per second. These are requests
        //     the central refines: Orion settles near 15 ms for fast transfer,
        //     while an iPhone keeps its preferred ~30 ms within the same range.
        //     latency 0 (responsive), 6 s supervision timeout.
        server->setDataLen(handle, 251);
        server->updateConnParams(handle, 12, 24, 0, 600);

        // NOTE: do NOT call NimBLEDevice::startSecurity() here. Forcing a
        // peripheral-initiated Security Request on connect races the central's
        // own pairing procedure — the two SMP exchanges collide and pairing
        // fails mid-passkey (auth complete bonded=0 → we disconnect, dropping
        // the link while the user is still entering the code). Let the central
        // drive pairing; it requests encryption right after connecting (it needs
        // it to sync), so the passkey modal still appears promptly.

        // Determine which slot this connection belongs to by checking stored addresses.
        uint8_t orion_addr[6]  = {};
        uint8_t iphone_addr[6] = {};
        ble_manager::load_bond_addr(ble_manager::BOND_KEY_ORION,  orion_addr);
        ble_manager::load_bond_addr(ble_manager::BOND_KEY_IPHONE, iphone_addr);

        if (conn_matches(info, orion_addr)) {
            // Known Orion reconnecting (matched by identity address).
            LOG("[ble] Orion reconnected (bonded peer)\n");
            g_orion_connected = true;
            g_orion_conn      = handle;
            BleEvent ev = {};
            ev.type = BleEventType::OrionConnected;
            ev.data.conn_handle = handle;
            eq_push(ev);
        } else if (conn_matches(info, iphone_addr)) {
            // Known iPhone reconnecting (matched by identity address).
            // Track the link, but do NOT start ANCS here: the ANCS service is
            // encrypted, so discovery + CCCD subscription must wait until the
            // link re-encrypts. The IphoneConnected event (which starts ANCS) is
            // posted from onAuthenticationComplete, after encryption is restored.
            // Starting it here (pre-encryption) made the CCCD writes fail
            // silently, so iOS never delivered notifications.
            LOG("[ble] iPhone reconnected (bonded peer) — awaiting encryption\n");
            g_iphone_connected = true;
            g_iphone_conn      = handle;
        } else {
            // Unknown peer — fresh connection, awaiting pairing.
            // Passkey modal is shown via onPassKeyDisplay() when NimBLE begins
            // the passkey exchange — not here on connect.
            LOG("[ble] unknown peer — awaiting bond\n");

            // During the iPhone-pairing window (Setup Step 4 / runtime re-pair),
            // request security immediately so the passkey prompt appears right
            // away. Without this, a central that doesn't auto-initiate pairing
            // on its own (e.g. nRF Connect standing in for an iPhone) sits
            // connected-but-unencrypted until the user manually writes to an
            // encrypted characteristic. Unlike the Orion case above, there is no
            // central-driven pairing to race here — this window is mutually
            // exclusive with Orion's Step 2 bonding window.
            if (g_iphone_pairing_window && ble_manager::is_bond_slot_empty(iphone_addr)) {
                LOG("[ble] iPhone pairing window open — requesting security\n");
                NimBLEDevice::startSecurity(handle);
            }
        }

        // BLE auto-stops advertising when a connection forms. Re-advertise after
        // a KNOWN peer (re)connects whenever Ori still needs to be discoverable:
        //   • both peers bonded → the other bonded peer can still reconnect;
        //   • iPhone-pairing window open + iPhone slot empty → the ANCS advert
        //     must survive an Orion (re)connect, or the iPhone can never see Ori
        //     (this is the "advertising disappears at the pairing screen" bug).
        // We skip this for an UNKNOWN peer — that's a fresh pairing in progress,
        // and re-advertising mid-bond would invite a second connection.
        // restart_advertising() stops again on its own once both are connected.
        bool known_peer = conn_matches(info, orion_addr) ||
                           conn_matches(info, iphone_addr);
        bool both_bonded = !ble_manager::is_bond_slot_empty(orion_addr) &&
                           !ble_manager::is_bond_slot_empty(iphone_addr);
        bool ancs_window = g_iphone_pairing_window &&
                           ble_manager::is_bond_slot_empty(iphone_addr);
        if (known_peer && (both_bonded || ancs_window)) {
            ble_manager::restart_advertising();
        }
    }

    void onDisconnect(NimBLEServer* server,
                      NimBLEConnInfo& info, int reason) override {
        uint16_t handle = info.getConnHandle();
        LOG("[ble] peer disconnected handle=%u reason=%d\n",
                       (unsigned)handle, reason);

        // OTA flash commit is tearing the stack down — these disconnects are us
        // closing peers in quiesce_for_commit(). Do not restart advertising or
        // post events; the device reboots in a moment.
        if (g_quiescing) return;

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

        // While an iPhone-bond wipe is pending, leave advertising OFF so the
        // still-bonded iPhone can't reconnect and race the wipe — the wipe
        // completion (finish_pending_iphone_wipe) restarts advertising once the
        // bond is deleted.
        if (g_iphone_wipe_pending) return;

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

        uint8_t orion_addr[6]  = {};
        uint8_t iphone_addr[6] = {};
        ble_manager::load_bond_addr(ble_manager::BOND_KEY_ORION,  orion_addr);
        ble_manager::load_bond_addr(ble_manager::BOND_KEY_IPHONE, iphone_addr);

        // RECONNECT of an already-bonded peer: its identity matches a filled slot
        // and the link just re-encrypted with the stored LTK. This fires on every
        // bonded reconnect (encryption restart), so it must NOT be treated as a
        // new bond — previously it hit the reject branch and disconnected Orion.
        if (conn_matches(info, orion_addr)) {
            LOG("[ble] Orion bonded peer reconnected — encryption restored\n");
            if (g_orion_conn != handle) {  // onConnect couldn't resolve identity yet
                g_orion_connected = true;
                g_orion_conn      = handle;
                BleEvent ev = {};
                ev.type = BleEventType::OrionConnected;
                ev.data.conn_handle = handle;
                eq_push(ev);
            }
            return;
        }
        if (conn_matches(info, iphone_addr)) {
            LOG("[ble] iPhone bonded peer reconnected — encryption restored\n");
            // Always (re)post here for the iPhone — onConnect intentionally does
            // NOT post for it, so ANCS discovery + subscription run only now that
            // the link is encrypted. on_iphone_connected() is idempotent if the
            // event somehow arrives twice.
            g_iphone_connected = true;
            g_iphone_conn      = handle;
            {
                BleEvent ev = {};
                ev.type = BleEventType::IphoneConnected;
                ev.data.conn_handle = handle;
                eq_push(ev);
            }
            return;
        }

        // NEW bond — assign to an open slot if the current state permits pairing.
        // Store the resolved IDENTITY address so future reconnects match.
        bool orion_empty  = ble_manager::is_bond_slot_empty(orion_addr);
        bool iphone_empty = ble_manager::is_bond_slot_empty(iphone_addr);

        if (orion_empty && is_orion_pairing_allowed()) {
            // New Orion bond — Step 2.
            ble_manager::on_orion_bonded(handle, bond_addr_to_store(info));
        } else if (iphone_empty && is_iphone_pairing_allowed()) {
            // New iPhone bond — Step 4 / re-pair.
            ble_manager::on_iphone_bonded(handle, bond_addr_to_store(info));
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

        // Only surface the passkey modal when a pairing window is actually open
        // (Setup Step 2 for Orion, or the re-pair iPhone screen). Outside a
        // window the bond is rejected at onAuthenticationComplete anyway — but a
        // central that connects just after the re-pair screen closes (an iPhone
        // that already saw the ANCS solicitation) would still start the SMP
        // exchange and pop a confusing code. Suppress the modal in that case.
        // Bonded reconnects re-encrypt with the stored LTK and never reach here.
        if (is_orion_pairing_allowed() || is_iphone_pairing_allowed()) {
            BleEvent ev = {};
            ev.type         = BleEventType::PasskeyDisplay;
            ev.data.passkey = g_passkey;
            eq_push(ev);
        } else {
            LOG("[ble] passkey exchange outside pairing window — modal suppressed\n");
        }
        return g_passkey;
    }

    void onConfirmPassKey(NimBLEConnInfo& info, uint32_t pin) override {
        // LE Secure Connections numeric comparison. Confirm only inside a
        // pairing window; otherwise reject so a stray / in-flight central can't
        // bond — matching the onAuthenticationComplete gate, but failing the
        // pairing up front instead of forming then tearing down a bond.
        bool allow = is_orion_pairing_allowed() || is_iphone_pairing_allowed();
        NimBLEDevice::injectConfirmPasskey(info, allow);
    }
};

static OriServerCallbacks s_server_cb;

// ── Advertising helpers ────────────────────────────────────────────────────

// ── ble_manager public API ─────────────────────────────────────────────────

namespace ble_manager {

// Defined below — completes a deferred iPhone bond wipe once the link is down.
// Called from the IphoneDisconnected drain in poll().
void finish_pending_iphone_wipe();

void init() {
    LOG("[ble] init\n");

    // g_passkey starts at 0; regenerated fresh in onPassKeyDisplay() on each
    // bonding attempt so every attempt shows a unique code.

    // Single source of truth for the device name (BT-MAC-derived) so the
    // advertised name and the on-screen pairing pill can never disagree.
    g_ble_name = app_state::ble_name();
    NimBLEDevice::init(g_ble_name);
    NimBLEDevice::setMTU(247);

    // Prefer the 2 Mbit PHY on every connection — roughly doubles on-air
    // throughput for bulk sync (photo, meetings, PTO, album art). The link
    // negotiates back to 1M automatically if the central doesn't support 2M,
    // so this is safe for any Orion PC / iPhone.
    NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK,
                                BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK);

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
                // Cache the value in state_machine so apply_widget_defaults()
                // reflects it on future screen rebuilds instead of clobbering it
                // back to a hardcoded value. set_presence() also updates the
                // active card immediately.
                state_machine::set_presence(static_cast<uint8_t>(ev.data.presence));
                break;
            }

            case BleEventType::MediaMetaUpdated: {
                // gatt_server::handle_media_metadata() already wrote the new
                // title/artist/can_seek into app_state (a plain struct copy,
                // safe from the NimBLE host task) but couldn't call
                // screen_media_mode::update_meta() itself — it touches LVGL
                // labels, which must only happen on the main task. Re-read
                // the now-current value and paint it.
                const auto& m = app_state::media();
                screen_media_mode::update_meta(m.title, m.artist);
                break;
            }

            case BleEventType::AlbumArt:
                // Album art JPEG received — buf was allocated in PSRAM by
                // chunked_transfer. set_album_art() decodes it (LVGL TJPGD)
                // and displays it if the media screen is active; it always
                // takes ownership and frees jpeg_buf internally either way.
                if (ev.data.art.buf) {
                    LOG("[ble] AlbumArt received %u bytes\n",
                                   (unsigned)ev.data.art.len);
                    screen_media_mode::set_album_art(ev.data.art.buf, ev.data.art.len);
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
                // store_pto() only updates the cache; the PTO screen (if shown)
                // was built earlier with the placeholder, so ask the state
                // machine to rebuild it now that the real image is decoded.
                state_machine::notify_pto_image_changed();
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
                LOG("[ble:poll] Orion connected\n");
                state_machine::set_pc_connected(true);
                // Notify Device Status = RUNTIME_RECONNECTING per ble-protocol.md
                // §6.2 — this is just the wire-protocol status Orion sees. The
                // "Refreshing your day" overlay itself is NOT shown here: BLE
                // connecting doesn't guarantee Orion (the app) is actually
                // running in the background to ever send a BEGIN — showing the
                // overlay now could leave it stuck with nothing to dismiss it.
                // It's triggered instead by SyncBegin below, on the real
                // SyncControl{BEGIN} frame.
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
                LOG("[ble:poll] Orion bonded (provisional — awaiting handshake)\n");
                g_orion_connected = true;
                g_orion_conn      = ev.data.conn_handle;
                state_machine::set_pc_connected(true);
                gatt_server::set_device_status(0x01); // SETUP_BONDED_AWAITING_SYNC
                // NOTE: bond is provisional — not persisted, and no resume bookmark
                // yet. Both happen on OrionConfirmed (valid SyncControl{BEGIN}).
                // Bonding complete — hide passkey modal, revealing the "Connect on
                // Orion" base screen (BLE name + spinner). Stamp the time so the
                // Orioning modal (on SyncBegin) lingers behind a guaranteed-visible
                // beat of this screen.
                screen_setup::hide_passkey_modal(lv_scr_act());
                g_orion_bonded_ms  = millis();
                g_orioning_pending = false;
                break;

            case BleEventType::OrionConfirmed:
                if (g_orion_provisional) {
                    LOG("[ble:poll] Orion confirmed — persisting bond\n");
                    save_bond_addr(BOND_KEY_ORION, g_provisional_orion_addr);
                    g_orion_provisional = false;
                    // Now safe to set the "bonded, awaiting first sync" resume
                    // bookmark (first boot only): a power cycle mid-Orioning
                    // resumes on the Link-Orion screen, and Orion reconnects via
                    // the now-persisted bond. Cleared at SyncEnd.
                    if (nvs::is_first_boot()) {
                        nvs::mark_orion_bonded();
                    }
                }
                break;

            case BleEventType::SyncBegin:
                LOG("[ble:poll] sync begin (total=%u)\n", (unsigned)ev.data.total_bytes);
                if (nvs::is_first_boot()) {
                    // Setup only: defer the Orioning modal so the "Connect on
                    // Orion" screen gets its guaranteed beat (poll() shows it
                    // once CONNECT_ORION_MIN_MS has elapsed since bonding).
                    g_orioning_pending = true;
                } else if (ev.data.total_bytes > RECONNECT_OVERLAY_MIN_BYTES) {
                    // Runtime: this is the actual SyncControl{BEGIN} frame —
                    // the real start of a sync, as opposed to merely a BLE
                    // connection (Orion's background service might not even
                    // be running yet). Show the "Refreshing your day" overlay
                    // now, but only for syncs big enough to actually be worth
                    // it — Time Sync and Shortcut Config alone (sent
                    // unconditionally every periodic refresh, §6.3) total well
                    // under this, and those were deliberately built to be
                    // invisible (no blackout, no rebuild). Any sync that also
                    // carries Profile/Photo/Meetings/PTO is comfortably larger.
                    state_machine::on_reconnect_begin();
                }
                break;

            case BleEventType::SyncCommit:
                LOG("[ble:poll] sync commit — applying staged data\n");
                gatt_server::run_staged_commit();
                break;

            case BleEventType::SyncEnd:
                LOG("[ble:poll] sync end\n");
                g_orioning_pending = false;  // don't pop the modal after we advance
                if (nvs::is_first_boot()) {
                    // Persist "at phone pairing step" before advancing the UI so
                    // a power cycle between now and setup completion resumes here.
                    nvs::mark_orion_synced();
                    // Complete the progress ring before advancing.
                    screen_setup::update_orioning_progress(lv_scr_act(), 100);
                    screen_setup::hide_orioning_modal(lv_scr_act());
                    screen_setup::set_step(lv_scr_act(), screen_setup::Step::PhonePairing);
                } else if (ev.data.light_refresh) {
                    // Only Shortcut Config changed — apply_shortcuts_cbor()
                    // already updated the shortcuts row in place. Skip the
                    // disruptive full screen rebuild; just redraw the
                    // existing (already-correct) screen to restore it from
                    // run_staged_commit()'s blackout() before the NVS write.
                    lv_obj_invalidate(lv_scr_act());
                } else {
                    state_machine::on_reconnect_end();
                }
                break;

            case BleEventType::OrioningProgress:
                // Only one of these two is ever the live screen at a time — the
                // other no-ops safely (update_orioning_progress checks for a
                // SetupState* user_data; set_progress checks its own module-level
                // ring pointer, cleared on screen delete).
                screen_setup::update_orioning_progress(lv_scr_act(), ev.data.pct);
                screen_reconnect_syncing::set_progress(ev.data.pct);
                break;

            case BleEventType::IphoneBonded: {
                LOG("[ble:poll] iPhone bonded\n");
                // Capture BEFORE dismiss_phone_pairing() below — on first-boot
                // setup it synchronously builds the Complete screen, whose
                // build_complete() calls nvs::mark_setup_complete() immediately,
                // flipping is_first_boot() to false right then. Checking after
                // that call would always see false and never fire this fix.
                bool was_first_boot = nvs::is_first_boot();
                // Persist the iPhone bond slot here, on the main task. The host
                // task already updated the RAM cache in on_iphone_bonded(); this
                // commits it to NVS off the host-task stack.
                save_bond_addr(BOND_KEY_IPHONE, ev.peer_addr);
                g_iphone_connected = true;
                g_iphone_conn      = ev.data.conn_handle;
                state_machine::set_phone_connected(true);
                // Start ANCS on the fresh-bond connection too. Without this,
                // NS/DS were only subscribed on a bonded RECONNECT — after the
                // very first pairing no notifications arrived until the iPhone
                // dropped and re-connected. Idempotent if already subscribed.
                ancs_client::on_iphone_connected(ev.data.conn_handle);
                restart_advertising();
                // Pairing done → leave the phone-pairing screen: hides the
                // passkey modal, then first-boot setup advances to the Setup
                // Complete screen while a runtime re-pair returns to the screen
                // that launched it. Without this a successful runtime re-pair
                // left the passkey modal stuck on screen forever (and first-boot
                // only advanced via the Skip button).
                screen_setup::dismiss_phone_pairing(lv_scr_act());

                // iOS quirk: it doesn't reliably flush the ANCS notification
                // backlog (the "PreExisting" replay) on the SAME connection
                // where the bond was just created — only on connections after
                // that (a power-cycle reconnect proves the firmware-side
                // backlog handling above already works correctly). Force that
                // "after the first" condition immediately: drop this fresh
                // bond a moment after NS/DS subscribe, then let the existing
                // bonded-disconnect → restart_advertising() → iOS auto-
                // reconnect path bring it straight back. The status bar is
                // hidden during setup, so this brief blip is invisible.
                //
                // Only on initial setup (Step 4) — NOT a runtime re-pair.
                // The backlog-on-first-bond quirk is specifically a first-boot
                // condition; a runtime re-pair hasn't been confirmed to need
                // (or want) a surprise reconnect blip on an otherwise-live device.
                if (was_first_boot) {
                    uint16_t handle = ev.data.conn_handle;
                    lv_timer_t* t = lv_timer_create([](lv_timer_t* timer) {
                        uint16_t h = (uint16_t)(uintptr_t)lv_timer_get_user_data(timer);
                        LOG("[ble] forcing iPhone reconnect to flush ANCS backlog (handle=%u)\n",
                            (unsigned)h);
                        NimBLEDevice::getServer()->disconnect(h);
                        lv_timer_delete(timer);
                    }, 500, (void*)(uintptr_t)handle);
                    lv_timer_set_repeat_count(t, 1);
                }
                break;
            }

            case BleEventType::OrionDisconnected:
                LOG("[ble:poll] Orion disconnected\n");
                state_machine::set_pc_connected(false);
                // A provisional (unconfirmed) peer that drops was never Orion —
                // clear the handshake state so the timeout path doesn't fire on a
                // stale handle. Nothing was persisted, so there's nothing to undo.
                g_orion_provisional = false;
                g_orioning_pending  = false;  // cancel any pending Orioning modal
                // Force presence to Offline immediately (never show stale presence).
                widget_profile_card::set_default_presence(
                    widget_profile_card::Presence::Offline);
                gatt_server::set_device_status(0xF0); // ERROR_GENERIC until reconnect
                // Discard any in-progress sync staging — link dropped before END.
                gatt_server::abort_sync_stage();
                break;

            case BleEventType::IphoneDisconnected:
                LOG("[ble:poll] iPhone disconnected\n");
                ancs_client::on_iphone_disconnected();
                // If an unpair is waiting on this disconnect, delete the bond +
                // clear NVS now that the link is down (safe — see wipe_iphone_bond).
                finish_pending_iphone_wipe();
                break;

            default:
                break;
        }
    }

    // Deferred Orioning modal: SyncControl{BEGIN} arrived, but hold the modal
    // back until the "Connect on Orion" screen has been visible for at least
    // CONNECT_ORION_MIN_MS since bonding (see g_orioning_pending). is_first_boot
    // guards it to setup; show_orioning_modal must only run on the setup screen.
    if (g_orioning_pending &&
        (int32_t)(millis() - g_orion_bonded_ms) >= (int32_t)CONNECT_ORION_MIN_MS) {
        g_orioning_pending = false;
        screen_setup::show_orioning_modal(lv_scr_act());
    }

    // Orion handshake timeout: a peer bonded (passkey OK) but never sent a valid
    // SyncControl{BEGIN} — it isn't Orion. Drop it (disconnect + delete its LTK
    // bond) so it's never saved as the Orion slot, and keep advertising for the
    // real Orion. onDisconnect() handles re-advertising and connection cleanup.
    if (g_orion_provisional &&
        (int32_t)(millis() - g_orion_handshake_deadline) >= 0) {
        LOG("[ble] Orion handshake timeout — dropping unconfirmed peer\n");
        g_orion_provisional = false;
        NimBLEServer* server = NimBLEDevice::getServer();
        if (server && g_provisional_orion_conn != BLE_HS_CONN_HANDLE_NONE) {
            server->disconnect(g_provisional_orion_conn);
        }
        NimBLEAddress addr(g_provisional_orion_addr, BLE_ADDR_PUBLIC);
        NimBLEDevice::deleteBond(addr);
        // onDisconnect() (fired by the disconnect above) resets connection state
        // and restarts advertising for the real Orion.
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

    // Drain ANCS notifications captured by the host-task notify callbacks. Done
    // here (main task) so the attribute-request CP write and the status-bar
    // LVGL refresh run off the host task — see ancs_client::poll().
    ancs_client::poll(g_orion_connected);
}

void quiesce_for_commit() {
    LOG("[ble] quiescing stack for OTA flash commit\n");

    // Make disconnect handling inert for the rest of this routine: the
    // disconnects we issue below must not restart advertising or open NVS
    // (onDisconnect checks g_quiescing and bails).
    g_quiescing = true;

    // Stop advertising first so no new connection/bond (and its NVS flash write)
    // can begin.
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv) adv->stop();

    // Close every active connection WHILE the host is still fully initialised.
    // deinit() with a live link aborts any in-flight GATT operation — e.g. the
    // ANCS client's service discovery — and on this NimBLE build that abort
    // callback (NimBLEClient::serviceDiscoveredCB → taskRelease) runs against
    // host/task state deinit() has already freed, dereferencing a null pointer
    // (InstrFetchProhibited, PC=0). Disconnecting here lets those callbacks run
    // against a live stack, then we wait for the links to actually close so
    // deinit() has nothing left to tear down.
    NimBLEServer* server = NimBLEDevice::getServer();
    if (server) {
        for (uint16_t h : server->getPeerDevices()) {
            server->disconnect(h);
        }
        uint32_t t0 = millis();
        while (server->getConnectedCount() > 0 && (millis() - t0) < 600) {
            delay(10);
        }
    }

    // Tear down host + controller. deinit(false) leaves bonds in NVS untouched;
    // the post-commit reboot re-inits BLE from scratch.
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

    if (orion_bonded && iphone_bonded &&
        g_orion_connected && g_iphone_connected) {
        // Both bonded peers are already connected — nothing to advertise for.
        // Re-armed by onDisconnect when either drops (so the missing peer can
        // reconnect), and by onConnect after the first of the two reconnects.
        LOG("[ble] adv: stopped (both bonded peers connected)\n");
        adv->stop();
        return;
    }

    // Orion is bonded + connected and there is no iPhone bond — nothing to
    // advertise for: Orion is already here, and a fresh iPhone is only solicited
    // from the re-pair screen (which opens the pairing window). Stay silent until
    // then. If Orion later drops, onDisconnect re-arms undirected advertising so
    // it can reconnect; opening the re-pair screen re-arms it for the iPhone.
    if (orion_bonded && g_orion_connected && !iphone_bonded && !g_iphone_pairing_window) {
        LOG("[ble] adv: stopped (Orion connected, iPhone slot empty, not pairing)\n");
        adv->stop();
        return;
    }
    // NOTE: when both peers are bonded but one (or both) is disconnected — e.g.
    // after a power cycle — we fall through to UNDIRECTED connectable advertising
    // below, NOT directed. Undirected lets EITHER central reconnect on its own:
    // the iPhone auto-reconnects off the advert, and Orion reconnects via its own
    // scan (matching the Ori Sync UUID). A directed advert can only target one
    // address, which previously left the iPhone unable to reconnect. No accept-
    // list: bonding is state-gated and every data characteristic is encrypted, so
    // a stranger that connects can neither bond nor read/write anything — and an
    // accept-list keyed on the iPhone's rotating private address is fragile.

    // Public undirected advertising, in one of two flavours (two 128-bit UUIDs
    // don't fit one 31-byte packet, so they're mutually exclusive):
    //
    //  • iPhone-pairing advert — pairing window open + iPhone slot empty. iOS
    //    engages an accessory that SOLICITS its ANCS service: the ANCS UUID must
    //    be in the primary advert as a "list of 128-bit Service Solicitation
    //    UUIDs" (AD type 0x15) — meaning "I want a device that PROVIDES ANCS" —
    //    NOT as a provided-service list (0x06/0x07). Advertising it as a provided
    //    service is why the device showed in nRF ("Services: ANCS") but iOS never
    //    listed/engaged it. Built by hand since NimBLE has no solicitation setter.
    //      adv: Flags 3 + ANCS solicitation 18 + Appearance 4 = 25 B ✓ (name in scan resp)
    //
    //  • Orion-discovery advert (default) — Ori Sync UUID + manufacturer mode
    //    flag so Orion can discover and classify Ori.
    //      adv: Flags 3 + Ori Sync UUID 18 + Mfr 5 = 26 B ✓ (name in scan resp)
    bool advertise_ancs = !iphone_bonded && g_iphone_pairing_window;
    uint8_t mfr_flag = (orion_bonded) ? ADV_FLAG_RUNTIME : ADV_FLAG_SETUP;

    adv->reset();
    adv->setConnectableMode(BLE_GAP_CONN_MODE_UND);

    if (advertise_ancs) {
        // ANCS Service Solicitation (AD type 0x15). getValue() yields the 16-byte
        // UUID little-endian, exactly the on-air order for the AD field.
        NimBLEUUID ancs(ANCS_SVC_UUID);
        uint8_t sol[18];
        sol[0] = 0x11;  // length = 1 (AD type) + 16 (UUID)
        sol[1] = 0x15;  // AD type: list of 128-bit Service Solicitation UUIDs
        memcpy(&sol[2], ancs.getValue(), 16);

        NimBLEAdvertisementData advData;
        advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
        advData.addData(sol, sizeof(sol));
        advData.setAppearance(0x0180);       // generic — helps iOS surface Ori
        adv->setAdvertisementData(advData);
    } else {
        adv->addServiceUUID(ORI_SVC_UUID);
        uint8_t mfr_data[3] = { 0xFF, 0xFF, mfr_flag };
        adv->setManufacturerData(std::string((char*)mfr_data, 3));
    }

    // Scan response: device name (iOS reads it via active scan).
    NimBLEAdvertisementData scanData;
    scanData.setName(g_ble_name.c_str());
    adv->setScanResponseData(scanData);

    // Advertising interval. All public undirected advertising runs at 100 ms:
    // it only happens when a bond slot is still open (Orion unbonded, or Orion
    // bonded but iPhone slot empty), so keeping it fast means iPhone (ANCS)
    // discovery stays snappy in every case the user might be pairing. Once both
    // peers are bonded we switch to directed advertising (early-return above)
    // and this path is never reached.
    uint32_t interval_ms = 100;
    adv->setMinInterval((uint16_t)(interval_ms * 1000 / 625));
    adv->setMaxInterval((uint16_t)(interval_ms * 1000 / 625));

    adv->start();
    LOG("[ble] adv: public undirected %s flag=0x%02X interval=%u ms\n",
                   advertise_ancs ? "(ANCS pairing)" : "(Orion)",
                   (unsigned)mfr_flag, (unsigned)interval_ms);
}

void set_iphone_pairing_window(bool active) {
    if (g_iphone_pairing_window == active) return;  // no change → no adv churn
    g_iphone_pairing_window = active;
    LOG("[ble] iPhone pairing window %s\n", active ? "OPEN" : "closed");
    restart_advertising();
}

// Map a bond-slot key to its RAM cache array. Returns nullptr for an unknown key.
static uint8_t* cache_for_key(const char* key) {
    if (strcmp(key, BOND_KEY_ORION)  == 0) return g_orion_addr_cache;
    if (strcmp(key, BOND_KEY_IPHONE) == 0) return g_iphone_addr_cache;
    return nullptr;
}

void prime_bond_cache() {
    // One-time NVS read on the main task at boot, BEFORE the BLE stack starts
    // accepting connections. After this every load_bond_addr() reads RAM, so the
    // host-task callbacks never open NVS. Must run before state_machine::init()
    // (which reads the iPhone slot) and before ble_manager::init().
    memset(g_orion_addr_cache,  0, 6);
    memset(g_iphone_addr_cache, 0, 6);
    // readOnly=false creates the namespace on first access; readOnly=true returns
    // NOT_FOUND on a fresh device and logs a spurious error.
    if (g_prefs.begin(BOND_NS, /*readOnly=*/false)) {
        g_prefs.getBytes(BOND_KEY_ORION,  g_orion_addr_cache,  6);
        g_prefs.getBytes(BOND_KEY_IPHONE, g_iphone_addr_cache, 6);
        g_prefs.end();
    }
    LOG("[ble] bond cache primed: orion=%d iphone=%d\n",
        (int)!is_bond_slot_empty(g_orion_addr_cache),
        (int)!is_bond_slot_empty(g_iphone_addr_cache));
}

void load_bond_addr(const char* key, uint8_t out_addr[6]) {
    // Reads the RAM cache (primed by prime_bond_cache()). Does NOT open NVS, so
    // it is safe to call from the NimBLE host-task callbacks.
    const uint8_t* src = cache_for_key(key);
    if (src) memcpy(out_addr, src, 6);
    else     memset(out_addr, 0, 6);
}

void save_bond_addr(const char* key, const uint8_t addr[6]) {
    // MAIN-TASK ONLY. Update the RAM cache first (so subsequent reads — including
    // host-task callbacks — see the new value immediately), then persist to NVS.
    uint8_t* dst = cache_for_key(key);
    if (dst) memcpy(dst, addr, 6);
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

// Actually delete the NimBLE bond record and zero the NVS slot. MAIN-TASK ONLY,
// and only safe when the iPhone link is DOWN — doing this on a live link
// corrupts NVS (the next Preferences open crashes in the namespace walk).
static void finish_iphone_bond_wipe(const uint8_t iphone_addr[6]) {
    if (!is_bond_slot_empty(iphone_addr)) {
        NimBLEAddress addr(iphone_addr, BLE_ADDR_PUBLIC);
        NimBLEDevice::deleteBond(addr);
    }
    const uint8_t zero[6] = {};
    save_bond_addr(BOND_KEY_IPHONE, zero);
    LOG("[ble] iPhone bond deleted + NVS slot cleared\n");
}

void finish_pending_iphone_wipe() {
    if (!g_iphone_wipe_pending) return;
    g_iphone_wipe_pending = false;
    // Link is down AND advertising is stopped (onDisconnect left it off while a
    // wipe was pending), so the iPhone can't have reconnected — delete the bond
    // + clear NVS with no concurrent NimBLE store activity, then re-advertise.
    finish_iphone_bond_wipe(g_iphone_wipe_addr);
    restart_advertising();
}

void wipe_iphone_bond() {
    LOG("[ble] wiping iPhone bond\n");
    uint8_t iphone_addr[6] = {};
    load_bond_addr(BOND_KEY_IPHONE, iphone_addr);

    // Stop advertising up front so the still-bonded iPhone can't reconnect while
    // we tear down the bond. A live link during deleteBond + the NVS write races
    // the NimBLE host task's bond-store NVS and corrupts it (Guru: LoadProhibited
    // deep in nvs_*). Advertising is restarted only after the bond is gone.
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv) adv->stop();

    if (g_iphone_conn != BLE_HS_CONN_HANDLE_NONE) {
        // Connected (the unpair modal only shows while connected): disconnect
        // first; the delete + NVS write + advertising restart run on the
        // IphoneDisconnected event, by which point the link is down and we never
        // restarted advertising (onDisconnect skips it while a wipe is pending),
        // so no reconnect can race the wipe.
        memcpy(g_iphone_wipe_addr, iphone_addr, sizeof(g_iphone_wipe_addr));
        g_iphone_wipe_pending = true;
        g_iphone_connected    = false;   // reflect intent immediately
        NimBLEServer* server = NimBLEDevice::getServer();
        if (server) server->disconnect(g_iphone_conn);
        return;
    }

    // Already disconnected (e.g. re-pair path) — delete + persist now (no link,
    // not advertising), then re-advertise with the iPhone slot now empty.
    finish_iphone_bond_wipe(iphone_addr);
    g_iphone_connected = false;
    g_iphone_conn      = BLE_HS_CONN_HANDLE_NONE;
    restart_advertising();
}

bool is_orion_connected()  { return g_orion_connected;  }
bool is_iphone_connected() { return g_iphone_connected; }
uint16_t orion_conn_handle() { return g_orion_conn; }

void on_orion_bonded(uint16_t conn_handle, const uint8_t peer_addr[6]) {
    LOG("[ble] Orion bonded (provisional) addr=%02X:%02X:%02X:%02X:%02X:%02X\n",
                   peer_addr[5], peer_addr[4], peer_addr[3],
                   peer_addr[2], peer_addr[1], peer_addr[0]);
    // PROVISIONAL: passkey bonding succeeded, but we do NOT yet persist this as
    // the Orion bond. Hold it in RAM until the peer proves it's Orion by writing
    // a valid SyncControl{BEGIN} (confirm_orion_peer()); a timeout drops it. This
    // stops a phone / non-Orion PC paired off Ori's passkey screen from being
    // saved as Orion.
    memcpy(g_provisional_orion_addr, peer_addr, 6);
    g_provisional_orion_conn   = conn_handle;
    g_orion_handshake_deadline = millis() + ORION_HANDSHAKE_TIMEOUT_MS;
    g_orion_provisional        = true;
    g_orion_connected          = true;
    g_orion_conn               = conn_handle;

    BleEvent ev = {};
    ev.type = BleEventType::OrionBonded;
    ev.data.conn_handle = conn_handle;
    memcpy(ev.peer_addr, peer_addr, 6);
    eq_push(ev);
}

// Called from the GATT server (NimBLE host task) when a bonded peer writes a
// valid SyncControl{BEGIN}. If that peer is the provisional Orion, hand off to
// the main task (OrionConfirmed) to persist the bond — keeps the NVS write off
// the host task, mirroring the SyncCommit deferral. No-op once confirmed or for
// runtime/reconnect BEGINs.
void confirm_orion_peer() {
    if (!g_orion_provisional) return;
    LOG("[ble] Orion handshake (SyncControl BEGIN) — confirming bond\n");
    BleEvent ev = {};
    ev.type = BleEventType::OrionConfirmed;
    eq_push(ev);
}

void on_iphone_bonded(uint16_t conn_handle, const uint8_t peer_addr[6]) {
    LOG("[ble] iPhone bonded addr=%02X:%02X:%02X:%02X:%02X:%02X\n",
                   peer_addr[5], peer_addr[4], peer_addr[3],
                   peer_addr[2], peer_addr[1], peer_addr[0]);
    // Runs on the NimBLE host task: update the RAM cache now so onConnect/
    // onAuthenticationComplete immediately recognise this peer as bonded, but
    // do NOT write NVS here. The NVS persist is done on the main task in the
    // IphoneBonded poll handler (never open Preferences from the host task).
    memcpy(g_iphone_addr_cache, peer_addr, 6);
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

// Deferred media-screen repaint after handle_media_metadata() updates
// app_state on the NimBLE host task (no payload — the main task re-reads
// app_state::media()).
void ble_post_media_meta_event() {
    BleEvent ev = {};
    ev.type = BleEventType::MediaMetaUpdated;
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

// Deferred SyncControl BEGIN — show orioning modal / reconnect overlay on main task.
void ble_post_sync_begin_event(uint32_t total_bytes) {
    BleEvent ev = {};
    ev.type = BleEventType::SyncBegin;
    ev.data.total_bytes = total_bytes;
    eq_push(ev);
}

// Deferred SyncControl END — apply staged sync data to NVS/UI on main task.
// stage_commit() does multiple NVS flash writes + LVGL profile-card updates;
// running it from the NimBLE host task risks colliding with LCD_CAM DMA and
// tripping the interrupt watchdog (see state_machine.cpp's deferred-NVS-write
// note). gatt_server::run_staged_commit() posts SyncEnd once it's done.
void ble_post_sync_commit_event() {
    BleEvent ev = {};
    ev.type = BleEventType::SyncCommit;
    eq_push(ev);
}

// Posted by gatt_server::run_staged_commit() — advance setup step or dismiss
// reconnect overlay now that staged data has been committed. light_refresh
// is true when Shortcut Config was the only item with a screen-visible
// effect in this sync — see stage_commit()'s doc comment.
void ble_post_sync_end_event(bool light_refresh) {
    BleEvent ev = {};
    ev.type = BleEventType::SyncEnd;
    ev.data.light_refresh = light_refresh;
    eq_push(ev);
}

// Deferred sync milestone — update orioning progress ring on main task.
void ble_post_orioning_progress(uint8_t pct) {
    BleEvent ev = {};
    ev.type     = BleEventType::OrioningProgress;
    ev.data.pct = pct;
    eq_push(ev);
}

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

#include "nvs_store.h"
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
    ClockFaceUpdate,   // Clock Face char write — 0=Digital, 1=Analog (state_machine::set_clock_face)
    TimeFormatUpdate,  // Device Settings "h" — 0=24-hour, 1=12-hour (state_machine::set_time_format)
    MediaMetaUpdated,  // MediaMetadata write applied to app_state — repaint media screen
    AlbumArt,
    PhotoReceived,      // profile photo JPEG — forward to photo_cache::store()
    TimeOffPhotoReceived,   // Time Off destination JPEG (or len=0 = no photo)
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
    UnpairPhone,       // Orion wrote the iPhone unpair magic to Device Command (char 0008)
    AncsFilterUpdate,  // Orion wrote ANCS Notification Filter via Device Settings (char 000E)
    ShortcutUpdate,    // Orion wrote shortcut slots via Device Settings (char 000E) — repaint shortcuts row
    WeatherUpdate,     // Orion wrote weather condition + temp via Device Settings (char 000E)
    AncsAction,        // Orion wrote ANCS Notification Action (char 0012, ble-protocol.md §13) —
                       // deferred so the blocking ANCS CP write can run safely on the main task
    AncsResubscribed,  // Orion (re)subscribed to char 0010 or 0011's CCCD — the correctly-timed
                       // signal to resync that characteristic's mirror; see gatt_server.cpp's
                       // onSubscribe() and ancs_client.h's resync_orion_relay()/
                       // resync_orion_call_state() doc comments for why OrionConnected fires
                       // too early for this.
};

struct BleEvent {
    BleEventType type;
    union {
        uint32_t passkey;
        widget_profile_card::Presence presence;
        uint8_t  clock_face;     // ClockFaceUpdate — 0=Digital, 1=Analog
        uint8_t  time_format;    // TimeFormatUpdate — 0=24-hour, 1=12-hour
        uint8_t  ancs_filter;   // AncsFilterUpdate — 0=Disabled..3=All
        struct { uint8_t* buf; size_t len; } art;
        uint16_t conn_handle;
        uint8_t  pct;            // OrioningProgress
        uint32_t total_bytes;    // SyncBegin — declared total from SyncControl{BEGIN}
        struct { uint8_t condition; int16_t temp_f; uint8_t unit; } weather; // WeatherUpdate
        struct { uint32_t uid; uint8_t action; } ancs_action; // AncsAction — 0=Positive, 1=Negative
        bool     ancs_resubscribed_call_state; // AncsResubscribed — true=char 0011, false=char 0010
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

// First-boot-only ANCS backlog-flush reconnect (see IphoneBonded handling
// below), deferred until the Setup Complete screen hands off to runtime —
// state_machine::poll() calls run_pending_ancs_backlog_reconnect() there.
// Cleared without acting if the iPhone disconnects on its own first (nothing
// to force; the stashed handle may otherwise be stale/reused by then).
bool     g_ancs_backlog_reconnect_pending = false;
uint16_t g_ancs_backlog_reconnect_conn    = BLE_HS_CONN_HANDLE_NONE;

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
// alone — sent unconditionally inside every BEGIN/END session (Device Settings
// writes happen outside the pipeline and don't count toward total),
// ble-protocol.md §6.3 — runs well under 100 bytes; any sync that also carries
// Profile/Photo/Meetings/Time Off is comfortably over this.
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

// Unknown-peer connect timeout. BLE auto-stops advertising the instant ANY
// connection forms (see onConnect()'s "BLE auto-stops advertising" comment),
// and onConnect() deliberately does NOT re-advertise for an unknown/unbonded
// peer (a fresh pairing might be in progress) — so a connection that never
// even starts the passkey exchange (onPassKeyDisplay never fires) leaves Ori
// silently undiscoverable for as long as that peer just sits there. Observed
// in practice: Windows' own BLE stack silently reconnecting to a previously-
// seen address after a factory reset, independent of any app (confirmed by
// direct log evidence that PC_app/orion's own reconnect-supervisor never
// attempted anything) — never proceeds to pairing, so it would otherwise
// block rediscovery indefinitely. Disconnect any unknown peer that hasn't
// started bonding within UNKNOWN_PEER_BOND_TIMEOUT_MS; onDisconnect()
// already unconditionally restarts advertising, so this self-heals cleanly.
// 2s is tight on purpose — a genuine Orion pairing attempt reaches
// onPassKeyDisplay() almost immediately after connecting (start_pairing()
// kicks off WinRT's PairAsync() ceremony right after connect, nothing
// blocks in front of it) — if real pairing attempts ever get caught by
// this, raise the constant.
//
// Scoped to Orion's own bonding window only (armed in onConnect() only
// while orion_addr is still empty) — NOT the iPhone pairing window (Setup
// Step 3/4, or a runtime re-pair) that opens once Orion is already bonded.
// A real iPhone can legitimately take longer than 2s to work through iOS's
// own Bluetooth pairing flow, so this tight a deadline must never apply there.
constexpr uint32_t UNKNOWN_PEER_BOND_TIMEOUT_MS = 2000;
volatile bool g_unknown_peer_pending  = false;
uint16_t      g_unknown_peer_conn     = 0xFFFF; // BLE_HS_CONN_HANDLE_NONE
uint32_t      g_unknown_peer_deadline = 0;

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
    // Allowed only during Setup Step 2 (pairing sub-state) — ble-protocol.md §2.
    // AppState::SETUP alone covers the ENTIRE first-boot flow (Welcome, Install,
    // Pairing, PhonePairing, Complete are all one flat AppState — state_machine.h),
    // so it can't distinguish Step 2 on its own; screen_setup tracks which visual
    // sub-state is actually on screen and is_pairing_step_active() exposes that.
    return nvs::is_first_boot() &&
           (state_machine::current_state() == AppState::SETUP) &&
           screen_setup::is_pairing_step_active();
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

// Loads both bond-slot addresses at once — the common first step in every
// function below that needs to classify a peer or decide advertising mode.
// Purely mechanical: identical to calling load_bond_addr() twice inline, in
// the same order, with the same arguments.
static void load_both_bond_addrs(uint8_t orion_addr[6], uint8_t iphone_addr[6]) {
    ble_manager::load_bond_addr(ble_manager::BOND_KEY_ORION,  orion_addr);
    ble_manager::load_bond_addr(ble_manager::BOND_KEY_IPHONE, iphone_addr);
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

// Forward declaration — the real definition is the pre-existing
// ble_manager::delete_bond_matching_addr() further down (near the other
// bond-management helpers). Must be declared inside the SAME namespace as
// that definition, or this creates an unrelated, never-defined global-scope
// function of the same name — a silent redeclaration that compiles but fails
// to link. Needed here because OriServerCallbacks (just below) rejects
// unauthenticated/slot-full bonds and must delete the just-formed NimBLE
// bond record when it does, or the rejection leaks a bond-store slot.
namespace ble_manager {
static bool delete_bond_matching_addr(const uint8_t addr[6]);
}

// Peer-slot resolution for a fresh connection, extracted verbatim from
// OriServerCallbacks::onConnect() — same statements, same order, just named.
// Called AFTER the MTU/data-length/connection-parameter tuning and the
// documented "do NOT call startSecurity() here" window (see onConnect()'s own
// comments on the SMP-collision race and the conn-param-update pool
// exhaustion fix) — this function does not touch or reorder anything in that
// section, it only wraps what already ran strictly after it.
static void classify_and_handle_peer(NimBLEConnInfo& info, uint16_t handle,
                                      const uint8_t orion_addr[6],
                                      const uint8_t iphone_addr[6]) {
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

        // Arm the "never even started pairing" watchdog — see
        // UNKNOWN_PEER_BOND_TIMEOUT_MS's doc comment. Cleared by
        // onPassKeyDisplay() (bonding actually started) or onDisconnect()
        // (peer went away on its own); enforced in poll().
        //
        // Scoped to Orion's own pairing window only — while orion_addr is
        // still empty, i.e. before Orion has ever bonded. Once Orion is
        // bonded, an unknown peer connecting is the iPhone pairing window
        // (Setup Step 3/4 or a runtime re-pair) instead, and a real
        // iPhone can legitimately take longer than 2s to work through
        // iOS's own Bluetooth pairing flow — this watchdog must not
        // apply there, only to Orion's own bonding window.
        if (ble_manager::is_bond_slot_empty(orion_addr)) {
            g_unknown_peer_pending  = true;
            g_unknown_peer_conn     = handle;
            g_unknown_peer_deadline = millis() + UNKNOWN_PEER_BOND_TIMEOUT_MS;
        }

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
}

// Advertising-restart decision for a fresh connection, extracted verbatim
// from OriServerCallbacks::onConnect() — runs strictly after
// classify_and_handle_peer() above, same as before extraction.
static void maybe_restart_advertising_after_connect(NimBLEConnInfo& info,
                                                      const uint8_t orion_addr[6],
                                                      const uint8_t iphone_addr[6]) {
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

        // Throughput tuning for bulk sync (photo, meetings, Time Off, album art):
        //   • Data Length Extension — 251-octet LL PDUs so a full 247-byte ATT
        //     write rides in a single radio packet (less per-packet overhead).
        server->setDataLen(handle, 251);

        // Faster connection-interval range (15-30 ms vs the conservative
        // default) → more connection events per second. These are requests
        // the central refines: Orion settles near 15 ms for fast transfer,
        // while an iPhone keeps its preferred ~30 ms within the same range.
        // Latency 0 (responsive), 6 s supervision timeout.
        //
        // NOTE: an earlier version of this code tried to detect a second
        // simultaneous connection and request a more conservative interval
        // on both links, theorizing that two connections both pushing for a
        // fast interval was starving the shared radio schedule. That was
        // wrong and made things worse — the real bug (see platformio.ini's
        // MYNEWT_VAL_BLE_GAP_MAX_PENDING_CONN_PARAM_UPDATE comment) is that
        // NimBLE's conn-param-update pool only had room for 1 pending
        // request globally, so the second of any two concurrent
        // updateConnParams() calls always failed (BLE_HS_ENOMEM or
        // BLE_HS_EALREADY) and silently never applied — confirmed against
        // the vendored NimBLE source, not inferred. The old dual-handle
        // version doubled the number of concurrent requests per connect
        // event, making the pool exhaustion reliably worse. Fixed at the
        // actual resource constraint instead; this call is back to the
        // simple single-handle form that was here before that detour.
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
        load_both_bond_addrs(orion_addr, iphone_addr);

        classify_and_handle_peer(info, handle, orion_addr, iphone_addr);

        maybe_restart_advertising_after_connect(info, orion_addr, iphone_addr);
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

        // The unknown-peer watchdog's target went away on its own (or we just
        // disconnected it ourselves from poll()) — clear it either way so a
        // later, unrelated connection doesn't inherit a stale pending flag.
        if (handle == g_unknown_peer_conn) {
            g_unknown_peer_pending = false;
            g_unknown_peer_conn    = 0xFFFF;
        }

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
        load_both_bond_addrs(orion_addr, iphone_addr);

        // RECONNECT of an already-bonded peer: its identity matches a filled slot
        // and the link just re-encrypted with the stored LTK. This fires on every
        // bonded reconnect (encryption restart), so it must NOT be treated as a
        // new bond — previously it hit the reject branch and disconnected Orion.
        //
        // Checked BEFORE the Just-Works/MITM gate below, on purpose: MITM
        // protection was already established when this bond was first formed
        // via Passkey Entry, so a reconnect doesn't need to re-derive it from
        // info.isAuthenticated() — which NimBLE does not reliably keep
        // reporting true on a bonded reconnect (re-encryption via the stored
        // LTK) under dual-connection SM contention. That's the same
        // underlying flakiness already confirmed to intermittently trip
        // chars 0002-000F's WRITE_AUTHEN/READ_AUTHEN checks on Orion's own
        // reconnect (ble-protocol.md's char security notes). Gating
        // reconnects on this flag meant a single flaky read didn't just fail
        // one read — it deleted the peer's bond outright via
        // delete_bond_matching_addr() below, an unrecoverable failure
        // (no self-heal; needs a fresh pairing window) for whichever peer's
        // reconnect lost the SM race in a given moment. Only a genuinely NEW
        // bond attempt (below, once neither conn_matches() branch fires)
        // still needs the Just-Works rejection.
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

        // MITM enforcement (NEW bonds only — reconnects of an already-bonded
        // peer return above before ever reaching this check): setSecurityAuth()
        // REQUESTS MITM protection, but per the BLE SM spec a peer that
        // declares NoInputNoOutput IO capability still falls back to
        // unauthenticated Just Works — silently, with no passkey ever shown.
        // isBonded() alone doesn't distinguish that from a real Passkey Entry
        // bond, so check isAuthenticated() explicitly and drop anything that
        // snuck in via Just Works.
        if (!info.isAuthenticated()) {
            LOG("[ble] auth complete but NOT authenticated (Just Works?) — rejecting\n");
            // The SMP procedure already committed an LTK to NimBLE's bond store
            // by this point (isBonded() was true above) — delete it too, or an
            // unauthenticated bond silently squats on a bond-store slot forever.
            ble_manager::delete_bond_matching_addr(bond_addr_to_store(info));
            NimBLEDevice::getServer()->disconnect(handle);
            BleEvent ev = {};
            ev.type = BleEventType::AuthFailed;
            eq_push(ev);
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
            // Bond slots full or state doesn't allow bonding. The SMP procedure
            // already committed an LTK to NimBLE's bond store (isBonded() was
            // true above) before we ever get to look at slot/state — disconnect
            // alone leaves that bond orphaned in the store, so every rejected
            // attempt permanently eats one of NimBLE's finite bond-store slots.
            // Delete it too.
            LOG("[ble] bond rejected: slots full or wrong state\n");
            ble_manager::delete_bond_matching_addr(bond_addr_to_store(info));
            NimBLEDevice::getServer()->disconnect(handle);
        }
    }

    uint32_t onPassKeyDisplay() override {
        // Bonding has genuinely started for the connected unknown peer —
        // disarm the connect-timeout watchdog (UNKNOWN_PEER_BOND_TIMEOUT_MS)
        // regardless of whether the modal ends up suppressed below; either
        // way this connection is no longer just an idle, non-pairing peer.
        g_unknown_peer_pending = false;

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

// Defined below — reverts a confirmed-but-never-synced Orion bond. Called
// from the OrionDisconnected handler in poll().
static void revert_unconfirmed_orion_bond();

// Finds and deletes a NimBLE bond by its raw 6-byte address, regardless of
// address type. Ori's own NVS bond slots only ever store the 6 raw bytes, but
// iPhones (and some PCs) resolve to a Random identity address, not Public —
// reconstructing a NimBLEAddress with a hardcoded BLE_ADDR_PUBLIC type doesn't
// match NimBLE's own bond-store key (address+type together), so deleteBond()
// on that reconstructed address silently no-ops: ble_gap_unpair() fails its
// internal ble_store_read() lookup and returns an error nobody checked. The
// app-level NVS slot still clears, but the real LTK/IRK stays in NimBLE's own
// bond store, so the next connection from the same peer silently resumes the
// OLD bond via startSecurity() (re-encrypt with the still-valid stored key)
// instead of negotiating a fresh pairing — no passkey, no onConfirmPassKey.
// Looking the address up in NimBLE's own bond list first gets the type NimBLE
// actually stored it under, so deleteBond() always matches.
static bool delete_bond_matching_addr(const uint8_t addr[6]) {
    int n = NimBLEDevice::getNumBonds();
    for (int i = 0; i < n; ++i) {
        NimBLEAddress bonded = NimBLEDevice::getBondedAddress(i);
        if (memcmp(bonded.getVal(), addr, 6) == 0) {
            return NimBLEDevice::deleteBond(bonded);
        }
    }
    return false;
}

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
    // throughput for bulk sync (photo, meetings, Time Off, album art). The link
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

// ── poll() event handlers ───────────────────────────────────────────────────
// One function per BleEventType, extracted verbatim from poll()'s switch
// cases — same statements, same order, same globals touched, same
// break/return/fallthrough behavior (none of these fall through). Splitting
// for readability only; see each comment for the hardware-race context that
// makes exact ordering matter.

static void handle_factory_reset() {
    // Deferred from BLE callback — now safe to wipe NVS and restart.
    factory_reset::execute();
}

static void handle_presence_update(const BleEvent& ev) {
    // Cache the value in state_machine so apply_widget_defaults()
    // reflects it on future screen rebuilds instead of clobbering it
    // back to a hardcoded value. set_presence() also updates the
    // active card immediately.
    state_machine::set_presence(static_cast<uint8_t>(ev.data.presence));
}

static void handle_clock_face_update(const BleEvent& ev) {
    // set_clock_face() writes NVS and (if the Clock state is on
    // screen) rebuilds it — both must run on the main task.
    state_machine::set_clock_face(ev.data.clock_face);
}

static void handle_time_format_update(const BleEvent& ev) {
    // set_time_format() writes NVS and rebuilds the on-screen clock /
    // meeting list so times re-render immediately — main task only.
    state_machine::set_time_format(ev.data.time_format);
}

static void handle_weather_update(const BleEvent& ev) {
    // Cache in state_machine (mirrors PresenceUpdate) so
    // apply_widget_defaults() reflects it on future screen
    // rebuilds and the PC-link-down fallback can hide it.
    state_machine::set_weather(ev.data.weather.condition, ev.data.weather.temp_f,
                                ev.data.weather.unit);
}

static void handle_media_meta_updated() {
    // gatt_server::handle_media_metadata() already wrote the new
    // title/artist/can_seek/playing/position into app_state (plain
    // struct copies, safe from the NimBLE host task) but couldn't
    // touch LVGL — defer that to here (main task only).
    const auto& m = app_state::media();
    screen_media_mode::update_meta(m.title, m.artist);
    // Sync play/pause icon + paused-overlay with whatever the OS
    // reported — this is the authoritative source of truth, so it
    // overrides any local toggle the user made via tap.
    screen_media_mode::update_playing(app_state::media_playing());
    // Always call update_seek — it controls tl_overlay visibility
    // and hides the bar when duration_s == 0 or can_seek is false.
    screen_media_mode::update_seek(m.position_s, m.duration_s);
}

static void handle_album_art(const BleEvent& ev) {
    // Album art JPEG received — buf was allocated in PSRAM by
    // chunked_transfer. set_album_art() decodes it (LVGL TJPGD)
    // and displays it if the media screen is active; it always
    // takes ownership and frees jpeg_buf internally either way.
    if (ev.data.art.buf) {
        LOG("[ble] AlbumArt received %u bytes\n",
                       (unsigned)ev.data.art.len);
        screen_media_mode::set_album_art(ev.data.art.buf, ev.data.art.len);
    }
}

static void handle_photo_received(const BleEvent& ev) {
    // photo_cache::store() takes ownership of buf and frees it.
    LOG("[ble] PhotoReceived: %u bytes\n", (unsigned)ev.data.art.len);
    photo_cache::store(ev.data.art.buf, ev.data.art.len);
}

static void handle_time_off_photo_received(const BleEvent& ev) {
    // len == 0 means no destination photo set — store_time_off clears cache.
    LOG("[ble] TimeOffPhotoReceived: %u bytes\n", (unsigned)ev.data.art.len);
    photo_cache::store_time_off(ev.data.art.buf, ev.data.art.len);
    // store_time_off() only updates the cache; the Time Off screen (if shown)
    // was built earlier with the placeholder, so ask the state
    // machine to rebuild it now that the real image is decoded.
    state_machine::notify_time_off_image_changed();
}

static void handle_passkey_display(const BleEvent& ev) {
    LOG("[ble] passkey display: %06u\n", (unsigned)ev.data.passkey);
    screen_setup::show_passkey_modal(lv_scr_act(), ev.data.passkey);
}

static void handle_auth_failed() {
    // Hide the passkey modal — restores the Pairing base screen
    // (BLE name + spinner) so the user can retry pairing.
    screen_setup::hide_passkey_modal(lv_scr_act());
}

static void handle_orion_connected() {
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
    // The ANCS relay resync (chars 0010/0011) does NOT happen
    // here — this event fires on encryption complete, well before
    // Orion has even started run_sync, let alone spawned the
    // watcher tasks that subscribe to those two characteristics.
    // A notify sent this early is silently dropped (not
    // subscribed yet) and never arrives — confirmed as the actual
    // cause of a resync that looked correct in code but never
    // worked in practice. See BleEventType::AncsResubscribed /
    // gatt_server.cpp's onSubscribe() for the fix: resync exactly
    // when Orion's own CCCD write for that characteristic lands.
}

static void handle_iphone_connected(const BleEvent& ev) {
    LOG("[ble:poll] iPhone connected — starting ANCS\n");
    ancs_client::on_iphone_connected(ev.data.conn_handle);
    // Phone name is read synchronously inside on_iphone_connected().
    gatt_server::notify_phone_bond_status(true, true, ancs_client::phone_name());
}

static void handle_orion_bonded(const BleEvent& ev) {
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
}

static void handle_orion_confirmed() {
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
}

static void handle_sync_begin(const BleEvent& ev) {
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
        // it — Time Sync alone (Device Settings is outside BEGIN/END
        // and doesn't count toward total, §6.3) runs well under
        // this, and was deliberately built to be invisible (no
        // blackout, no rebuild). Any sync that also carries
        // Profile/Photo/Meetings/Time Off is comfortably larger.
        state_machine::on_reconnect_begin();
    }
}

static void handle_sync_commit() {
    LOG("[ble:poll] sync commit — applying staged data\n");
    gatt_server::run_staged_commit();
}

static void handle_sync_end() {
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
    } else {
        state_machine::on_reconnect_end();
    }
}

static void handle_orioning_progress(const BleEvent& ev) {
    // Only one of these two is ever the live screen at a time — the
    // other no-ops safely (update_orioning_progress checks for a
    // SetupState* user_data; set_progress checks its own module-level
    // ring pointer, cleared on screen delete).
    screen_setup::update_orioning_progress(lv_scr_act(), ev.data.pct);
    screen_reconnect_syncing::set_progress(ev.data.pct);
}

static void handle_iphone_bonded(const BleEvent& ev) {
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
    // Phone name is read synchronously inside on_iphone_connected().
    gatt_server::notify_phone_bond_status(true, true, ancs_client::phone_name());
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
    // backlog handling above already works correctly). This is
    // iOS-side, not a first-boot-specific condition — ANY fresh
    // bond (first-boot setup Step 4 OR a later runtime re-pair)
    // leaves ANCS icons empty until the device reconnects at least
    // once (confirmed on hardware: a runtime re-pair without this
    // fix never loads ANCS icons). Force that "after the first"
    // condition: drop this fresh bond after NS/DS subscribe, then
    // let the existing bonded-disconnect → restart_advertising() →
    // iOS auto-reconnect path bring it straight back.
    g_ancs_backlog_reconnect_pending = true;
    g_ancs_backlog_reconnect_conn    = ev.data.conn_handle;

    if (was_first_boot) {
        // Deferred to AFTER the Setup Complete screen hands off to
        // runtime (state_machine::poll()'s g_setup_complete_pending
        // drain calls run_pending_ancs_backlog_reconnect()) rather
        // than fired here on a short fixed timer. ANCS backlog
        // processing — per-notification parsing plus icon-registry
        // lookups across up to 48 apps — is comparatively heavy, and
        // running it while Setup Complete's checkmark-ring/countdown-
        // bar animations are live competes with LVGL for the same
        // core. Trade-off: the phone icon may briefly show
        // disconnected/reconnecting on the runtime status bar
        // (Setup's status bar is hidden, so this used to be invisible).
    } else {
        // Runtime re-pair: dismiss_phone_pairing() above already
        // returned straight to the screen that launched the re-pair —
        // no animation-heavy hand-off screen to wait for — so fire
        // immediately instead of deferring to state_machine::poll()'s
        // first-boot-only drain point.
        run_pending_ancs_backlog_reconnect();
    }
}

static void handle_orion_disconnected() {
    LOG("[ble:poll] Orion disconnected\n");
    state_machine::set_pc_connected(false);
    // A provisional (unconfirmed) peer that drops was never Orion —
    // clear the handshake state so the timeout path doesn't fire on a
    // stale handle. Nothing was persisted, so there's nothing to undo.
    g_orion_provisional = false;
    g_orioning_pending  = false;  // cancel any pending Orioning modal
    // A CONFIRMED bond (BEGIN was received, orion_addr persisted at
    // OrionConfirmed) that drops before its first sync ever reaches
    // SyncEnd is a different, worse case than the provisional one
    // above: the bond IS persisted, and nothing else in this file
    // ever reverts it — left alone, Ori is stuck advertising
    // RUNTIME forever with no way back short of a factory reset
    // (see nvs::clear_orion_bonded()'s doc comment). Scoped to
    // first-boot's still-outstanding first sync only
    // (is_awaiting_sync() only ever holds there), so an
    // established bond's routine periodic reconnects/re-syncs at
    // runtime are never affected by this.
    if (nvs::is_first_boot() && nvs::is_awaiting_sync()) {
        revert_unconfirmed_orion_bond();
    }
    // Force presence to Offline immediately (never show stale presence).
    widget_profile_card::set_default_presence(
        widget_profile_card::Presence::Offline);
    gatt_server::set_device_status(0xF0); // ERROR_GENERIC until reconnect
    // Discard any in-progress sync staging — link dropped before END.
    gatt_server::abort_sync_stage();
}

static void handle_iphone_disconnected() {
    LOG("[ble:poll] iPhone disconnected\n");
    // Disconnected before the deferred backlog-flush reconnect fired
    // (e.g. phone walked out of range during the Setup Complete
    // linger) — drop the pending request rather than act on a
    // conn_handle that's now stale (and could be reused by a later,
    // unrelated connection).
    g_ancs_backlog_reconnect_pending = false;
    ancs_client::on_iphone_disconnected();
    // If an unpair is waiting on this disconnect, delete the bond +
    // clear NVS now that the link is down (safe — see wipe_iphone_bond).
    bool wipe_was_pending = g_iphone_wipe_pending;
    finish_pending_iphone_wipe();
    // If the wipe just completed, the bond is gone (bonded=false).
    // A plain disconnect (e.g. phone out of range) keeps the bond.
    gatt_server::notify_phone_bond_status(!wipe_was_pending, false, "");
}

static void handle_unpair_phone() {
    LOG("[ble:poll] remote iPhone unpair command\n");
    // Delegate to the same path the UI uses: state_machine::on_unpair_phone()
    // sets g_unpair_phone_pending → state_machine::poll() calls wipe_iphone_bond().
    state_machine::on_unpair_phone();
}

static void handle_ancs_filter_update(const BleEvent& ev) {
    LOG("[ble:poll] ANCS filter -> %u\n", (unsigned)ev.data.ancs_filter);
    ancs_client::set_filter(ev.data.ancs_filter);
    nvs::set_notif_filter(ev.data.ancs_filter);
}

static void handle_shortcut_update() {
    LOG("[ble:poll] shortcut update\n");
    screen_media_mode::update_shortcuts();
    const app_state::ShortcutSlot* s = app_state::shortcuts();
    nvs::set_shortcut_slots(s[0].icon_token, s[1].icon_token, s[2].icon_token);
}

static void handle_ancs_action(const BleEvent& ev) {
    uint32_t uid    = ev.data.ancs_action.uid;
    uint8_t  action = ev.data.ancs_action.action;
    // "Is it still live" is checked here (main task) rather than in
    // the NimBLE host-task write handler — ancs_client's queue is
    // only ever touched from the main task (ancs_client::poll()),
    // so this is the one safe place to read it.
    if (!ancs_client::is_queued(uid)) {
        LOG("[ble:poll] AncsNotificationAction: uid=%u not live -> NACK\n",
            (unsigned)uid);
        gatt_server::nack_sync_control("NACK_CBOR_DECODE");
    } else if (action == 0) {
        LOG("[ble:poll] AncsNotificationAction: answer uid=%u\n", (unsigned)uid);
        ancs_client::answer_notification(uid);
    } else {
        // Negative action. Mirror Ori's own on-device swipe
        // (modal_ancs_list.cpp's commit_row_delete): only send the
        // ANCS Negative when the notification actually HAS one;
        // otherwise just drop it from Ori's queue locally.
        // Sending PerformNotificationAction(Negative) for a
        // notification with no negative action is a no-op the
        // phone can (and on some iOS versions does) answer by
        // re-asserting the notification via a Modified/Added
        // event — which re-queues it on Ori and bounces the
        // PhoneBondStatus count right back up, so an Orion swipe
        // looks like it "didn't reduce the count." drop_notification
        // never touches the phone, so nothing re-asserts it.
        bool has_neg = app_state::ancs_notification_by_uid(uid).has_neg_action;
        if (has_neg) {
            LOG("[ble:poll] AncsNotificationAction: dismiss uid=%u\n", (unsigned)uid);
            ancs_client::dismiss_notification(uid);
        } else {
            LOG("[ble:poll] AncsNotificationAction: drop (no neg action) uid=%u\n",
                (unsigned)uid);
            ancs_client::drop_notification(uid);
        }
    }
}

static void handle_ancs_resubscribed(const BleEvent& ev) {
    // Orion just (re)subscribed to char 0010 or 0011 — the first
    // moment a notify is guaranteed to actually reach it, unlike
    // OrionConnected (fires before run_sync even starts). See
    // gatt_server.cpp's onSubscribe() and ancs_client.h's
    // resync_orion_relay()/resync_orion_call_state() doc comments.
    if (ev.data.ancs_resubscribed_call_state) {
        LOG("[ble:poll] Orion subscribed to char 0011 — resyncing call state\n");
        ancs_client::resync_orion_call_state();
    } else {
        LOG("[ble:poll] Orion subscribed to char 0010 — resyncing ANCS mirror\n");
        ancs_client::resync_orion_relay();
    }
}

// ── poll() trailing deferred/timeout checks ─────────────────────────────────
// Independent, unrelated checks that run once per poll() after the event
// queue drains — extracted verbatim, in the exact same order as before.

static void check_orioning_modal_deferred_show() {
    // Deferred Orioning modal: SyncControl{BEGIN} arrived, but hold the modal
    // back until the "Connect on Orion" screen has been visible for at least
    // CONNECT_ORION_MIN_MS since bonding (see g_orioning_pending). is_first_boot
    // guards it to setup; show_orioning_modal must only run on the setup screen.
    if (g_orioning_pending &&
        (int32_t)(millis() - g_orion_bonded_ms) >= (int32_t)CONNECT_ORION_MIN_MS) {
        g_orioning_pending = false;
        screen_setup::show_orioning_modal(lv_scr_act());
    }
}

static void check_orion_handshake_timeout() {
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
        delete_bond_matching_addr(g_provisional_orion_addr);
        // onDisconnect() (fired by the disconnect above) resets connection state
        // and restarts advertising for the real Orion.
    }
}

static void check_unknown_peer_timeout() {
    // Unknown-peer connect timeout: a peer connected but never even started
    // pairing (onPassKeyDisplay never fired) — see UNKNOWN_PEER_BOND_TIMEOUT_MS's
    // doc comment. Disconnect it; onDisconnect() already unconditionally
    // restarts advertising, which is the actual point — this is what makes
    // Ori rediscoverable again instead of silently blocked by a connection
    // that was never going to pair in the first place.
    if (g_unknown_peer_pending &&
        (int32_t)(millis() - g_unknown_peer_deadline) >= 0) {
        LOG("[ble] unknown-peer bond timeout — disconnecting non-pairing peer\n");
        g_unknown_peer_pending = false;
        NimBLEServer* server = NimBLEDevice::getServer();
        if (server && g_unknown_peer_conn != BLE_HS_CONN_HANDLE_NONE) {
            server->disconnect(g_unknown_peer_conn);
        }
    }
}

static void check_deferred_factory_reset() {
    // Factory-reset deferred restart (from remote BLE factory-reset command
    // or from the local long-press path if it posted to the event queue).
    //
    // BUG FIX: this used to reimplement the wipe inline as just
    // `nvs::factory_reset(); delay(200); ESP.restart();` — wiping the "ori"
    // NVS namespace (profile/settings/first-boot flag) but never the BLE
    // bonds or cached photos, directly contradicting factory_reset.h's own
    // documented contract ("both paths converge here so the exact same
    // NVS-wipe + bond-wipe + restart sequence runs regardless of trigger
    // source"). Concretely: every remote (Orion-triggered) factory reset
    // correctly showed the setup wizard again (first_boot flag cleared) but
    // silently left `orion_addr` bonded — Ori kept advertising RUNTIME
    // forever and was never selectable in Orion's scan again, no matter how
    // many times it was "reset." Routing through the shared
    // factory_reset::execute() (already used by the local long-press path)
    // fixes this and keeps both paths from ever drifting apart again.
    if (g_restart_pending) {
        g_restart_pending = false;
        LOG("[ble] executing deferred factory reset\n");
        factory_reset::execute();
    }
}

static void drain_ancs_notifications() {
    // Drain ANCS notifications captured by the host-task notify callbacks. Done
    // here (main task) so the attribute-request CP write and the status-bar
    // LVGL refresh run off the host task — see ancs_client::poll().
    ancs_client::poll(g_orion_connected);
}

static void check_chunk_reassembly_timeouts() {
    // Chunk-reassembly stall check (ble-protocol.md §5, NACK_CHUNK_TIMEOUT) —
    // gated to ~1 Hz since the 10 s timeout doesn't need finer resolution and
    // poll() itself runs every main-loop iteration.
    static uint32_t s_last_chunk_poll_ms = 0;
    uint32_t now_ms = (uint32_t)millis();
    if (now_ms - s_last_chunk_poll_ms >= 1000) {
        s_last_chunk_poll_ms = now_ms;
        gatt_server::poll_chunk_timeouts();
    }
}

void poll() {
    BleEvent ev;
    while (eq_pop(ev)) {
        switch (ev.type) {

            case BleEventType::FactoryReset:
                handle_factory_reset();
                break;

            case BleEventType::PresenceUpdate:
                handle_presence_update(ev);
                break;

            case BleEventType::ClockFaceUpdate:
                handle_clock_face_update(ev);
                break;

            case BleEventType::TimeFormatUpdate:
                handle_time_format_update(ev);
                break;

            case BleEventType::WeatherUpdate:
                handle_weather_update(ev);
                break;

            case BleEventType::MediaMetaUpdated:
                handle_media_meta_updated();
                break;

            case BleEventType::AlbumArt:
                handle_album_art(ev);
                break;

            case BleEventType::PhotoReceived:
                handle_photo_received(ev);
                break;

            case BleEventType::TimeOffPhotoReceived:
                handle_time_off_photo_received(ev);
                break;

            case BleEventType::PasskeyDisplay:
                handle_passkey_display(ev);
                break;

            case BleEventType::AuthFailed:
                handle_auth_failed();
                break;

            case BleEventType::OrionConnected:
                handle_orion_connected();
                break;

            case BleEventType::IphoneConnected:
                handle_iphone_connected(ev);
                break;

            case BleEventType::OrionBonded:
                handle_orion_bonded(ev);
                break;

            case BleEventType::OrionConfirmed:
                handle_orion_confirmed();
                break;

            case BleEventType::SyncBegin:
                handle_sync_begin(ev);
                break;

            case BleEventType::SyncCommit:
                handle_sync_commit();
                break;

            case BleEventType::SyncEnd:
                handle_sync_end();
                break;

            case BleEventType::OrioningProgress:
                handle_orioning_progress(ev);
                break;

            case BleEventType::IphoneBonded:
                handle_iphone_bonded(ev);
                break;

            case BleEventType::OrionDisconnected:
                handle_orion_disconnected();
                break;

            case BleEventType::IphoneDisconnected:
                handle_iphone_disconnected();
                break;

            case BleEventType::UnpairPhone:
                handle_unpair_phone();
                break;

            case BleEventType::AncsFilterUpdate:
                handle_ancs_filter_update(ev);
                break;

            case BleEventType::ShortcutUpdate:
                handle_shortcut_update();
                break;

            case BleEventType::AncsAction:
                handle_ancs_action(ev);
                break;

            case BleEventType::AncsResubscribed:
                handle_ancs_resubscribed(ev);
                break;

            default:
                break;
        }
    }

    check_orioning_modal_deferred_show();
    check_orion_handshake_timeout();
    check_unknown_peer_timeout();
    check_deferred_factory_reset();
    drain_ancs_notifications();
    check_chunk_reassembly_timeouts();
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
    load_both_bond_addrs(orion_addr, iphone_addr);

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

    // clearData() — NOT reset(). NimBLEAdvertising::reset() is
    // `if (!stop()) return false; *this = NimBLEAdvertising();` — its OWN
    // payload-clearing step is conditional on its internal stop() call
    // actually succeeding. But we always call adv->stop() ourselves a few
    // lines above (and again in the early-return branches above this point),
    // so by the time we get here advertising is already stopped — reset()'s
    // internal stop() then finds nothing to stop, fails, and reset() returns
    // early WITHOUT clearing m_advData. Silent, because this call's return
    // value was never checked.
    //
    // The fallout: the "else" branch below calls adv->addServiceUUID()
    // directly on the (never-actually-cleared) persistent advertising
    // object, and NimBLEAdvertisementData::addServiceUUID() has NO dedup —
    // if a 128-bit-UUID AD field is already present it just appends another
    // 16 bytes onto it (NimBLEAdvertisementData.cpp). So every subsequent
    // restart_advertising() call (each connect/disconnect of EITHER bonded
    // peer triggers one) grew the Ori Sync UUID field by another 16 bytes,
    // until it blew past the 31-byte legacy adv PDU cap: "Cannot add UUID,
    // data length exceeded!" — which then failed ble_gap_adv_rsp_set_data
    // too and left the NimBLE host desynced ("Host not synced!"), destabilizing
    // the whole stack enough to also break the encryption/auth handshake for
    // both Orion and the iPhone (2026-07 hardware log: repeated "auth failed —
    // disconnecting" for both peers, traced back to this).
    //
    // clearData() has no stop() dependency and no failure path — it
    // unconditionally wipes m_advData/m_scanData every call, so the UUID/
    // manufacturer-data fields set below always start from an empty payload.
    adv->clearData();
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
        delete_bond_matching_addr(iphone_addr);
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
    // No IphoneDisconnected event fires for this path, so notify Orion here.
    gatt_server::notify_phone_bond_status(false, false, "");
}

// Reverts an Orion bond that was confirmed (SyncControl{BEGIN} received,
// orion_addr persisted — OrionConfirmed) but whose first sync never reached
// SyncEnd before the link dropped — see nvs::clear_orion_bonded()'s doc
// comment for the exact failure window this closes. Called only from
// OrionDisconnected, by which point the link is already down — unlike
// wipe_iphone_bond() there's no live-link race to defer around, since the
// disconnect that triggers this has already happened.
static void revert_unconfirmed_orion_bond() {
    uint8_t orion_addr[6] = {};
    load_bond_addr(BOND_KEY_ORION, orion_addr);
    if (!is_bond_slot_empty(orion_addr)) {
        delete_bond_matching_addr(orion_addr);
    }
    const uint8_t zero[6] = {};
    save_bond_addr(BOND_KEY_ORION, zero);
    nvs::clear_orion_bonded();
    LOG("[ble] Orion bond reverted (disconnected before first sync completed)\n");

    // Reset the setup screen back to its pre-bond base state. We're still on
    // the Pairing step here (is_awaiting_sync() only ever holds there),
    // just possibly with the passkey or Orioning modal stuck on top from
    // the interrupted attempt — both hide_* calls are no-ops if their modal
    // isn't the one currently showing.
    lv_obj_t* screen = lv_scr_act();
    screen_setup::hide_passkey_modal(screen);
    screen_setup::hide_orioning_modal(screen);
    screen_setup::set_step(screen, screen_setup::Step::Pairing);

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

// Fires the ANCS backlog-flush reconnect deferred by the IphoneBonded handler
// in poll(). Two callers: the IphoneBonded handler itself, immediately, for a
// runtime re-pair; state_machine::poll() once the Setup Complete screen's own
// timer has handed off to runtime, for first-boot setup. No-op if nothing is
// pending, or if the iPhone already disconnected on its own in the meantime
// (IphoneDisconnected clears the pending flag).
void run_pending_ancs_backlog_reconnect() {
    if (!g_ancs_backlog_reconnect_pending) return;
    g_ancs_backlog_reconnect_pending = false;
    if (g_iphone_connected && g_iphone_conn == g_ancs_backlog_reconnect_conn) {
        LOG("[ble] forcing iPhone reconnect to flush ANCS backlog (handle=%u)\n",
            (unsigned)g_iphone_conn);
        NimBLEDevice::getServer()->disconnect(g_iphone_conn);
    }
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

// Deferred clock-face update (from Clock Face write handler).
void ble_post_clock_face_event(uint8_t face) {
    BleEvent ev = {};
    ev.type = BleEventType::ClockFaceUpdate;
    ev.data.clock_face = face;
    eq_push(ev);
}

// Deferred time-format update (from Device Settings "h" write handler).
void ble_post_time_format_event(uint8_t fmt) {
    BleEvent ev = {};
    ev.type = BleEventType::TimeFormatUpdate;
    ev.data.time_format = fmt;
    eq_push(ev);
}

// Deferred weather update (from Device Settings write handler — only posted
// when "w", "d", and "u" were all present in the same write, ble-protocol.md §6.4).
void ble_post_weather_event(uint8_t condition, int16_t temp_f, uint8_t unit) {
    BleEvent ev = {};
    ev.type = BleEventType::WeatherUpdate;
    ev.data.weather.condition = condition;
    ev.data.weather.temp_f    = temp_f;
    ev.data.weather.unit      = unit;
    eq_push(ev);
}

// Deferred iPhone unpair command (from Unpair Phone Command write handler).
void ble_post_unpair_phone_event() {
    BleEvent ev = {};
    ev.type = BleEventType::UnpairPhone;
    eq_push(ev);
}

// Deferred ANCS filter update (from ANCS Notification Filter write handler).
void ble_post_ancs_filter_event(uint8_t level) {
    BleEvent ev = {};
    ev.type = BleEventType::AncsFilterUpdate;
    ev.data.ancs_filter = level;
    eq_push(ev);
}

// Deferred ANCS Notification Action (from char 0012 write handler,
// ble-protocol.md §13) — the actual answer_notification()/dismiss_notification()
// dispatch (and the is-uid-still-live check) happens in poll() on the main
// task; see gatt_server.cpp's handle_ancs_action() for why this can't run
// directly on the NimBLE host task.
void ble_post_ancs_action_event(uint32_t uid, uint8_t action) {
    BleEvent ev = {};
    ev.type = BleEventType::AncsAction;
    ev.data.ancs_action.uid    = uid;
    ev.data.ancs_action.action = action;
    eq_push(ev);
}

// Deferred ANCS relay resync (from gatt_server.cpp's onSubscribe(), fired
// when Orion writes char 0010 or 0011's CCCD) — resync_orion_relay()/
// resync_orion_call_state() read ancs_client's queue/call-state, which is
// only safe to touch from the main task, not this NimBLE host-task callback.
void ble_post_ancs_resubscribed_event(bool call_state) {
    BleEvent ev = {};
    ev.type = BleEventType::AncsResubscribed;
    ev.data.ancs_resubscribed_call_state = call_state;
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

// Deferred Time Off destination image (len==0 = no photo set; store_time_off clears cache).
void ble_post_time_off_photo_event(uint8_t* buf, size_t len) {
    BleEvent ev = {};
    ev.type         = BleEventType::TimeOffPhotoReceived;
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
// reconnect overlay now that staged data has been committed.
void ble_post_sync_end_event(bool /*unused*/) {
    BleEvent ev = {};
    ev.type = BleEventType::SyncEnd;
    eq_push(ev);
}

// Deferred shortcut row repaint — update_shortcuts() touches LVGL labels and
// must run on the main task, not the NimBLE host task.
void ble_post_shortcut_update_event() {
    BleEvent ev = {};
    ev.type = BleEventType::ShortcutUpdate;
    eq_push(ev);
}

// Deferred sync milestone — update orioning progress ring on main task.
void ble_post_orioning_progress(uint8_t pct) {
    BleEvent ev = {};
    ev.type     = BleEventType::OrioningProgress;
    ev.data.pct = pct;
    eq_push(ev);
}

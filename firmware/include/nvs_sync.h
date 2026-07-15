#pragma once

#include <stdint.h>
#include <stddef.h>

// Ori NVS Sync Data — extension to nvs_store for the BLE hash-manifest sync.
//
// Stores per-item SHA-256 hashes (for hash-manifest delta reconnect per
// ble-protocol.md §6.2) and the synced string payload for Profile Info.
//
// Large blobs (profile photo JPEG, Time Off image JPEG, meeting list CBOR)
// are held in PSRAM at runtime and written to NVS only as SHA-256 hashes.
// On power-up the large blobs are re-fetched from Orion on the next
// reconnect (the manifest will detect the mismatch because the PSRAM cache
// is empty, producing a non-matching hash). Only the string profile fields,
// meeting CBOR (small enough), and Time Off non-image fields are persisted as
// full values.
//
// Key layout (all in the "ori" Preferences namespace):
//   "p_sha"  — bytes(32): SHA-256 of the last-written Profile Info CBOR
//   "ph_sha" — bytes(32): SHA-256 of the last-written Profile Photo JPEG
//   "m_sha"  — bytes(32): SHA-256 of the last-written Meeting List CBOR
//   "to_sha" — bytes(32): SHA-256 of the last-written Time Off Entry CBOR
//   "p_name" — string: profile name (≤64 bytes)
//   "p_titl" — string: profile title (≤64 bytes)
//   "p_email"— string: profile email (≤128 bytes, optional)
//   "p_phone"— string: profile phone (≤32 bytes, optional)
//   "epoch"  — uint32: last synced UTC epoch, written on every sync END but
//              not currently read back by anything — the "SYNCED · X min
//              ago" pill is driven by the RAM-only app_state::set_last_sync_time()/
//              synced_pill_text() instead (a persisted epoch can't be trusted
//              across a power cycle anyway, since local time itself isn't
//              restored from flash — meeting-list.md)

namespace nvs_sync {

// SHA-256 hash operations. Backed by a RAM cache (see prime_hash_cache()) —
// load_hash() never touches NVS, so it's safe to call from the NimBLE host
// task (e.g. handle_manifest_write() on every reconnect).
// Returns false if the key is missing (first boot or after factory reset).
bool load_hash(const char* key, uint8_t out_hash[32]);
void save_hash(const char* key, const uint8_t hash[32]);

// Pre-load all sync hashes into RAM while the heap is clean (before any
// screen is created). Call once at startup alongside prime_time_off_cache().
void prime_hash_cache();

// Profile string fields.
void save_profile(const char* name, const char* title,
                  const char* email, const char* phone);
bool load_profile(char* out_name,  size_t name_sz,
                  char* out_title, size_t title_sz,
                  char* out_email, size_t email_sz,
                  char* out_phone, size_t phone_sz);

// Last synced epoch. See the "epoch" key note above — write-only today.
void save_epoch(uint32_t epoch_utc);

// Time Off metadata (start/end epoch + destination string).
// Returns true if a valid Time Off entry exists (start != 0).
void prime_time_off_cache();   // call once at startup (clean heap) before any screen is created
void save_time_off_meta(uint32_t start, uint32_t end, const char* destination);
bool load_time_off_meta(uint32_t* out_start, uint32_t* out_end,
                        char* out_dest, size_t dest_sz);

// Hash key constants — used by gatt_server for manifest comparison.
extern const char* const HASH_KEY_PROFILE;
extern const char* const HASH_KEY_PHOTO;
extern const char* const HASH_KEY_MEETINGS;
extern const char* const HASH_KEY_TIME_OFF;

} // namespace nvs_sync

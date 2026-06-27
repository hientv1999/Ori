#pragma once

#include <stdint.h>
#include <stddef.h>

// Ori NVS Sync Data — M5 extension to nvs_store.
//
// Stores per-item SHA-256 hashes (for hash-manifest delta reconnect per
// ble-protocol.md §6.2) and the synced string payload for Profile Info.
//
// Large blobs (profile photo JPEG, PTO image JPEG, meeting list CBOR)
// are held in PSRAM at runtime and written to NVS only as SHA-256 hashes.
// On power-up the large blobs are re-fetched from Orion on the next
// reconnect (the manifest will detect the mismatch because the PSRAM cache
// is empty, producing a non-matching hash). Only the string profile fields,
// meeting CBOR (small enough), and PTO non-image fields are persisted as
// full values.
//
// Key layout (all in the "ori" Preferences namespace):
//   "p_sha"  — bytes(32): SHA-256 of the last-written Profile Info CBOR
//   "ph_sha" — bytes(32): SHA-256 of the last-written Profile Photo JPEG
//   "m_sha"  — bytes(32): SHA-256 of the last-written Meeting List CBOR
//   "pto_sha"— bytes(32): SHA-256 of the last-written PTO Entry CBOR
//   "p_name" — string: profile name (≤64 bytes)
//   "p_titl" — string: profile title (≤64 bytes)
//   "p_email"— string: profile email (≤128 bytes, optional)
//   "p_phone"— string: profile phone (≤32 bytes, optional)
//   "epoch"  — uint32: last synced UTC epoch (for displaying SYNCED pill)

namespace nvs_sync {

// SHA-256 hash operations. Backed by a RAM cache (see prime_hash_cache()) —
// load_hash() never touches NVS, so it's safe to call from the NimBLE host
// task (e.g. handle_manifest_write() on every reconnect).
// Returns false if the key is missing (first boot or after factory reset).
bool load_hash(const char* key, uint8_t out_hash[32]);
void save_hash(const char* key, const uint8_t hash[32]);

// Pre-load all sync hashes into RAM while the heap is clean (before any
// screen is created). Call once at startup alongside prime_pto_cache().
void prime_hash_cache();

// Profile string fields.
void save_profile(const char* name, const char* title,
                  const char* email, const char* phone);
bool load_profile(char* out_name,  size_t name_sz,
                  char* out_title, size_t title_sz,
                  char* out_email, size_t email_sz,
                  char* out_phone, size_t phone_sz);

// Last synced epoch (for the "SYNCED · X min ago" pill).
void  save_epoch(uint32_t epoch_utc);
uint32_t load_epoch();

// PTO metadata (start/end epoch + destination string).
// Returns true if a valid PTO entry exists (start != 0).
void prime_pto_cache();   // call once at startup (clean heap) before any screen is created
void save_pto_meta(uint32_t start, uint32_t end, const char* destination);
bool load_pto_meta(uint32_t* out_start, uint32_t* out_end,
                   char* out_dest, size_t dest_sz);

// Meeting list CBOR blob (raw bytes, ≤ ~4 KB).
// save returns true on success; load returns actual bytes written (0 = not found).
bool   save_meetings_blob(const uint8_t* buf, size_t len);
size_t load_meetings_blob(uint8_t* out, size_t max_len);

// Hash key constants — used by gatt_server for manifest comparison.
extern const char* const HASH_KEY_PROFILE;
extern const char* const HASH_KEY_PHOTO;
extern const char* const HASH_KEY_MEETINGS;
extern const char* const HASH_KEY_PTO;

} // namespace nvs_sync

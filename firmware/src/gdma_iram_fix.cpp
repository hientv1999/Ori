// IRAM-resident wrappers for three GDMA HAL functions that live in flash
// (libhal.a) but are invoked from ISR context (BLE GDMA + LCD_CAM DMA paths).
//
// During NVS and LittleFS writes the ESP32-S3 disables the MSPI instruction
// cache system-wide. If a GDMA completion ISR fires in that window and tries
// to fetch gdma_hal_append / gdma_hal_reset / gdma_hal_start_with_desc from
// flash, the CPU panics with EXCCAUSE 0x7 ("cache disabled but cached memory
// region accessed"). The --wrap linker flags in platformio.ini redirect every
// call site to these IRAM functions, which call the static-inline LL register
// accessors directly, bypassing the flash gdma_ahb_hal_* dispatch.
#include <esp_attr.h>
#include "hal/gdma_hal.h"
#include "hal/gdma_ll.h"

extern "C" {

IRAM_ATTR void __wrap_gdma_hal_append(
    gdma_hal_context_t *hal, int chan_id, gdma_channel_direction_t dir)
{
    if (dir == GDMA_CHANNEL_DIRECTION_TX) {
        gdma_ll_tx_restart(hal->dev, (uint32_t)chan_id);
    } else {
        gdma_ll_rx_restart(hal->dev, (uint32_t)chan_id);
    }
}

IRAM_ATTR void __wrap_gdma_hal_reset(
    gdma_hal_context_t *hal, int chan_id, gdma_channel_direction_t dir)
{
    if (dir == GDMA_CHANNEL_DIRECTION_TX) {
        gdma_ll_tx_reset_channel(hal->dev, (uint32_t)chan_id);
    } else {
        gdma_ll_rx_reset_channel(hal->dev, (uint32_t)chan_id);
    }
}

IRAM_ATTR void __wrap_gdma_hal_start_with_desc(
    gdma_hal_context_t *hal, int chan_id, gdma_channel_direction_t dir, intptr_t desc_base_addr)
{
    if (dir == GDMA_CHANNEL_DIRECTION_TX) {
        gdma_ll_tx_set_desc_addr(hal->dev, (uint32_t)chan_id, (uint32_t)desc_base_addr);
        gdma_ll_tx_start(hal->dev, (uint32_t)chan_id);
    } else {
        gdma_ll_rx_set_desc_addr(hal->dev, (uint32_t)chan_id, (uint32_t)desc_base_addr);
        gdma_ll_rx_start(hal->dev, (uint32_t)chan_id);
    }
}

} // extern "C"

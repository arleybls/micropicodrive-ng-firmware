// BTstack <-> CYW43 glue — vendored from pico-sdk 2.2.0
// src/rp2_common/pico_cyw43_driver/btstack_cyw43.c (BSD-3-Clause, Raspberry Pi
// Ltd), replacing the SDK's pico_btstack_cyw43 library so the TLV bond store
// uses uiext_flash_bank_instance() (partition-boot-safe physical reads) instead
// of the stock pico_flash_bank_instance() whose XIP_BASE reads hang under QMI
// address translation. cyw43_arch_init() calls btstack_cyw43_init() by name.
#include "ble/le_device_db_tlv.h"
#include "btstack_tlv.h"
#include "btstack_tlv_flash_bank.h"
#include "btstack_memory.h"
#include "hci.h"

#include "pico/btstack_hci_transport_cyw43.h"
#include "pico/btstack_run_loop_async_context.h"
#include "pico/btstack_cyw43.h"

const hal_flash_bank_t *uiext_flash_bank_instance(void);

static void setup_tlv(void) {
    static btstack_tlv_flash_bank_t btstack_tlv_flash_bank_context;
    const btstack_tlv_t *btstack_tlv_impl = btstack_tlv_flash_bank_init_instance(
            &btstack_tlv_flash_bank_context,
            uiext_flash_bank_instance(),
            NULL);
    btstack_tlv_set_instance(btstack_tlv_impl, &btstack_tlv_flash_bank_context);
    le_device_db_tlv_configure(btstack_tlv_impl, &btstack_tlv_flash_bank_context);
}

bool btstack_cyw43_init(async_context_t *context) {
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_async_context_get_instance(context));
    hci_init(hci_transport_cyw43_instance(), NULL);
    setup_tlv();
    return true;
}

void btstack_cyw43_deinit(__unused async_context_t *context) {
    hci_power_control(HCI_POWER_OFF);
    hci_close();
    btstack_run_loop_async_context_deinit();
    btstack_run_loop_deinit();
    btstack_memory_deinit();
}

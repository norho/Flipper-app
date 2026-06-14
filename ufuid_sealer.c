#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <dialogs/dialogs.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <string.h>

#define TAG "UFUID_Sealer"

// =========================================================
// HELPER RADIO
// =========================================================

static void append_crc(uint8_t* data, size_t len) {
    uint16_t crc = 0x6363;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(uint8_t j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0x8408) : (crc >> 1);
        }
    }
    data[len]     = crc & 0xFF;
    data[len + 1] = (crc >> 8) & 0xFF;
}

static bool nfc_send_recv(uint8_t* tx, size_t tx_bits, uint8_t* rx, size_t rx_max, size_t* rx_bits) {
    furi_thread_flags_clear(0xFFFFFFFF);
    if(furi_hal_nfc_poller_tx(tx, tx_bits) != FuriHalNfcErrorNone) return false;

    bool rx_success = false;
    uint32_t start_time = furi_get_tick();
    while(furi_get_tick() - start_time < 150) {
        FuriHalNfcEvent event = furi_hal_nfc_poller_wait_event(20);
        if(event & FuriHalNfcEventRxEnd) { rx_success = true; break; }
        if((event & FuriHalNfcEventTimeout)) break;
    }
    if(!rx_success) return false;
    if(furi_hal_nfc_poller_rx(rx, rx_max, rx_bits) != FuriHalNfcErrorNone) return false;
    return (*rx_bits > 0);
}

// =========================================================
// MOTORE UFUID (Sblocco testato e funzionante)
// =========================================================

static bool try_ufuid_unlock(void) {
    uint8_t rx[32]; size_t rx_bits = 0;
    
    // Wake & Select
    uint8_t wupa = 0x52; nfc_send_recv(&wupa, 7, rx, sizeof(rx), &rx_bits);
    uint8_t anticoll[] = {0x93, 0x20}; nfc_send_recv(anticoll, 16, rx, sizeof(rx), &rx_bits);
    uint8_t sel[] = {0x93, 0x70, rx[0], rx[1], rx[2], rx[3], rx[4], 0, 0};
    append_crc(sel, 7); nfc_send_recv(sel, 72, rx, sizeof(rx), &rx_bits);

    // Auth Gen2
    uint8_t auth[] = {0x60, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
    append_crc(auth, 8);
    if(!nfc_send_recv(auth, 80, rx, sizeof(rx), &rx_bits)) return false;
    
    uint8_t c1[] = {0x40}; nfc_send_recv(c1, 7, rx, sizeof(rx), &rx_bits);
    uint8_t c2[] = {0x43}; nfc_send_recv(c2, 8, rx, sizeof(rx), &rx_bits);
    
    return true;
}
// =========================================================
// MOTORE DI SCRITTURA (Intoccabile)
// =========================================================

static bool nfc_write_block(uint8_t b, uint8_t* d) {
    uint8_t rx[32]; size_t rb = 0;
    uint8_t c[4] = {0xA0, b, 0, 0}; append_crc(c, 2);
    if(!nfc_send_recv(c, 32, rx, sizeof(rx), &rb)) return false;
    
    uint8_t wd[18]; memcpy(wd, d, 16); append_crc(wd, 16);
    if(!nfc_send_recv(wd, 144, rx, sizeof(rx), &rb)) return false;
    return (rb >= 4 && (rx[0] & 0x0F) == 0x0A);
}

static bool execute_ufuid_process(const char* path, bool seal) {
    Storage* s = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(s);
    uint8_t* dump = malloc(1024);
    if(!dump || !storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        free(dump); storage_file_free(f); furi_record_close(RECORD_STORAGE); return false;
    }
    storage_file_read(f, dump, 1024);
    
    furi_hal_nfc_acquire();
    furi_hal_nfc_poller_field_on();
    furi_delay_ms(100);

    bool res = false;
    if(try_ufuid_unlock()) {
        res = true;
        for(int i = 1; i < 64; i++) {
            if(!nfc_write_block(i, &dump[i*16])) { res = false; break; }
            furi_delay_ms(5);
        }
        if(res) nfc_write_block(0, &dump[0]);
        if(res && seal) {
            uint8_t c3[] = {0xE1, 0x00, 0, 0}; append_crc(c3, 2);
            uint8_t rx[32]; size_t rb;
            nfc_send_recv(c3, 32, rx, sizeof(rx), &rb);
        }
    }

    furi_hal_nfc_poller_field_off();
    furi_hal_nfc_release();
    free(dump); storage_file_close(f); storage_file_free(f); furi_record_close(RECORD_STORAGE);
    return res;
}

// ... Inserisci qui la tua GUI (l'importante è la funzione execute_ufuid_process)

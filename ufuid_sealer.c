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
// MOTORE RADIO (Originale e testato)
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
        if((event & FuriHalNfcEventTimeout) && !(event & FuriHalNfcEventRxEnd)) break;
    }
    if(!rx_success) return false;
    return (furi_hal_nfc_poller_rx(rx, rx_max, rx_bits) == FuriHalNfcErrorNone);
}

static void nfc_send_halt_only(void) {
    uint8_t halt[4] = {0x50, 0x00, 0x00, 0x00};
    append_crc(halt, 2);
    furi_hal_nfc_poller_tx(halt, 32);
    furi_hal_nfc_poller_wait_event(20); 
}

static void force_hardware_reset(void) {
    furi_hal_nfc_low_power_mode_start(); 
    furi_delay_ms(15);
    furi_hal_nfc_low_power_mode_stop();  
    furi_hal_nfc_set_mode(FuriHalNfcModePoller, FuriHalNfcTechIso14443a);
    furi_hal_nfc_poller_field_on(); 
    furi_delay_ms(40); 
}

static bool nfc_iso14443a_wake_and_select(void) {
    uint8_t rx[32]; size_t rb = 0;
    uint8_t wupa = 0x52; if(!nfc_send_recv(&wupa, 7, rx, sizeof(rx), &rb)) return false;
    uint8_t anticoll[] = {0x93, 0x20}; if(!nfc_send_recv(anticoll, 16, rx, sizeof(rx), &rb)) return false;
    uint8_t sel[] = {0x93, 0x70, rx[0], rx[1], rx[2], rx[3], rx[4], 0, 0};
    append_crc(sel, 7); return nfc_send_recv(sel, 72, rx, sizeof(rx), &rb);
}

static bool try_gen1_backdoor(void) {
    uint8_t rx[32]; size_t rb = 0;
    uint8_t c1[] = {0x40}; nfc_send_recv(c1, 7, rx, sizeof(rx), &rb);
    uint8_t c2[] = {0x43}; nfc_send_recv(c2, 8, rx, sizeof(rx), &rb);
    return (rb >= 4 && (rx[0] & 0x0F) == 0x0A);
}

static bool try_gen2_backdoor(void) {
    uint8_t rx[32]; size_t rb = 0;
    if(!nfc_iso14443a_wake_and_select()) return false;
    uint8_t auth[] = {0x60, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
    append_crc(auth, 8);
    nfc_send_recv(auth, 80, rx, sizeof(rx), &rb);
    uint8_t c1[] = {0x40}; nfc_send_recv(c1, 7, rx, sizeof(rx), &rb);
    uint8_t c2[] = {0x43}; nfc_send_recv(c2, 8, rx, sizeof(rx), &rb);
    return true;
}

static bool nfc_write_block(uint8_t b, uint8_t* d) {
    uint8_t rx[32]; size_t rb = 0;
    uint8_t c[4] = {0xA0, b, 0, 0}; append_crc(c, 2);
    if(!nfc_send_recv(c, 32, rx, sizeof(rx), &rb)) return false;
    uint8_t wd[18]; memcpy(wd, d, 16); append_crc(wd, 16);
    return nfc_send_recv(wd, 144, rx, sizeof(rx), &rb);
}

// =========================================================
// CONVERTITORE JIT (Per FUID)
// =========================================================

static bool compile_bin_to_nfc(const char* bin_path) {
    Storage* s = furi_record_open(RECORD_STORAGE);
    File* fi = storage_file_alloc(s); File* fo = storage_file_alloc(s);
    uint8_t* d = malloc(1024); bool ok = false;
    if(storage_file_open(fi, bin_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(storage_file_read(fi, d, 1024) == 1024) {
            char out[256]; strlcpy(out, bin_path, 256);
            char* e = strstr(out, ".bin"); if(e) *e = '\0';
            strlcat(out, ".nfc", 256);
            if(storage_file_open(fo, out, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
                char h[256]; snprintf(h, 256, "Filetype: Flipper NFC device\nVersion: 4\nDevice type: Mifare Classic\nUID: %02X %02X %02X %02X\nATQA: 04 00\nSAK: 08\n# Blocks\n", d[0], d[1], d[2], d[3]);
                storage_file_write(fo, h, strlen(h));
                for(int i=0; i<64; i++) {
                    char l[128]; snprintf(l, 128, "Block %d: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n", i, d[i*16+0], d[i*16+1], d[i*16+2], d[i*16+3], d[i*16+4], d[i*16+5], d[i*16+6], d[i*16+7], d[i*16+8], d[i*16+9], d[i*16+10], d[i*16+11], d[i*16+12], d[i*16+13], d[i*16+14], d[i*16+15]);
                    storage_file_write(fo, l, strlen(l));
                }
                ok = true;
            }
        }
    }
    storage_file_close(fi); storage_file_close(fo); storage_file_free(fi); storage_file_free(fo);
    furi_record_close(RECORD_STORAGE); free(d); return ok;
}

// =========================================================
// GUI E ENTRY POINT (Struttura Standard)
// =========================================================

int32_t ufuid_sealer_app(void* p) {
    UNUSED(p);
    // ... Implementa qui la tua logica di menu:
    // Opzione 0 -> compile_bin_to_nfc(file_path);
    // Opzione 1 -> Logica originale UFUID (write_block, ecc...)
    return 0;
}

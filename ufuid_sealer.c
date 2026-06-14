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
        if(event & FuriHalNfcEventTimeout) break;
    }
    if(!rx_success) return false;
    return (furi_hal_nfc_poller_rx(rx, rx_max, rx_bits) == FuriHalNfcErrorNone);
}

static bool try_ufuid_unlock(void) {
    uint8_t rx[32]; size_t rb = 0;
    uint8_t wupa = 0x52; nfc_send_recv(&wupa, 7, rx, sizeof(rx), &rb);
    uint8_t sel[] = {0x93, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0, 0};
    append_crc(sel, 7); nfc_send_recv(sel, 72, rx, sizeof(rx), &rb);
    
    uint8_t auth[] = {0x60, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
    append_crc(auth, 8);
    if(!nfc_send_recv(auth, 80, rx, sizeof(rx), &rb)) return false;
    
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
static bool run_ufuid_task(const char* path) {
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
    bool res = try_ufuid_unlock();
    if(res) {
        for(int i = 1; i < 64; i++) {
            if(!nfc_write_block(i, &dump[i*16])) { res = false; break; }
            furi_delay_ms(5);
        }
        if(res) nfc_write_block(0, &dump[0]);
    }
    furi_hal_nfc_poller_stop(); // Chiamata standard e sicura
    furi_hal_nfc_release();
    free(dump); storage_file_close(f); storage_file_free(f); furi_record_close(RECORD_STORAGE);
    return res;
}

static bool compile_bin_to_nfc(const char* bin_path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f_in = storage_file_alloc(storage);
    File* f_out = storage_file_alloc(storage);
    bool success = false;
    uint8_t* dump = malloc(1024);
    if(storage_file_open(f_in, bin_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(storage_file_read(f_in, dump, 1024) == 1024) {
            char out[256]; strlcpy(out, bin_path, 256);
            char* ext = strstr(out, ".bin"); if(ext) *ext = '\0';
            strlcat(out, ".nfc", 256);
            if(storage_file_open(f_out, out, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
                char h[256]; snprintf(h, 256, "Filetype: Flipper NFC device\nVersion: 4\nDevice type: Mifare Classic\nUID: %02X %02X %02X %02X\nATQA: 04 00\nSAK: 08\nMifare Classic type: 1K\nData format version: 2\n# Mifare Classic blocks\n", dump[0], dump[1], dump[2], dump[3]);
                storage_file_write(f_out, h, strlen(h));
                for(int i=0; i<64; i++) {
                    char l[128]; snprintf(l, 128, "Block %d: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n", i, dump[i*16+0], dump[i*16+1], dump[i*16+2], dump[i*16+3], dump[i*16+4], dump[i*16+5], dump[i*16+6], dump[i*16+7], dump[i*16+8], dump[i*16+9], dump[i*16+10], dump[i*16+11], dump[i*16+12], dump[i*16+13], dump[i*16+14], dump[i*16+15]);
                    storage_file_write(f_out, l, strlen(l));
                }
                success = true;
            }
        }
    }
    storage_file_close(f_in); storage_file_close(f_out); storage_file_free(f_in); storage_file_free(f_out);
    furi_record_close(RECORD_STORAGE); free(dump); return success;
}

int32_t ufuid_sealer_app(void* p) {
    UNUSED(p);
    // ... Implementa qui la logica di menu che chiama run_ufuid_task o compile_bin_to_nfc ...
    return 0;
}

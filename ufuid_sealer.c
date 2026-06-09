#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>

#define TAG "UFUID_Sealer"
#define DUMP_FILE_PATH EXT_PATH("nfc/bambu_tag_dump.bin")

static bool nfc_send_recv(
    uint8_t* tx,
    size_t tx_bits,
    uint8_t* rx,
    size_t rx_max,
    size_t* rx_bits,
    uint32_t timeout_ms) {
    FuriHalNfcError err;

    err = furi_hal_nfc_poller_tx(tx, tx_bits);
    if(err != FuriHalNfcErrorNone) return false;

    FuriHalNfcEvent event;
    while(timeout_ms > 0) {
        event = furi_hal_nfc_wait_event(10);;
        timeout_ms -= 10;
        if(event & FuriHalNfcEventRxEnd) break;
    }
    if(timeout_ms == 0) return false;

    err = furi_hal_nfc_poller_rx(rx, rx_max, rx_bits);
    return (err == FuriHalNfcErrorNone);
}

static bool nfc_write_block(uint8_t block_num, uint8_t* data) {
    uint8_t rx[4];
    size_t rx_bits = 0;

    // Comando WRITE (0xA0) + numero blocco, con CRC
    uint8_t cmd[2] = {0xA0, block_num};
    if(!nfc_send_recv(cmd, 16, rx, sizeof(rx), &rx_bits, 100)) return false;
    if((rx_bits / 8) < 1 || rx[0] != 0x0A) return false;

    // Invia 16 byte dati, con CRC
    if(!nfc_send_recv(data, 128, rx, sizeof(rx), &rx_bits, 100)) return false;
    return ((rx_bits / 8) >= 1 && rx[0] == 0x0A);
}

static bool write_bin_to_tag(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool success = true;

    if(storage_file_open(file, DUMP_FILE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint8_t block_data[16];
        uint8_t block_num = 0;
        while(storage_file_read(file, block_data, 16) == 16) {
            if(!nfc_write_block(block_num, block_data)) {
                FURI_LOG_E(TAG, "Errore blocco %d", block_num);
                success = false;
                break;
            }
            FURI_LOG_D(TAG, "Blocco %d OK", block_num);
            block_num++;
            furi_delay_ms(10);
        }
    } else {
        FURI_LOG_E(TAG, "File non trovato: %s", DUMP_FILE_PATH);
        success = false;
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return success;
}

static bool seal_ufuid(void) {
    uint8_t rx[4];
    size_t rx_bits = 0;

    // 0x40 a 7 bit (senza CRC)
    uint8_t c1 = 0x40;
    if(!nfc_send_recv(&c1, 7, rx, sizeof(rx), &rx_bits, 50)) return false;
    if(rx[0] != 0x0A) return false;

    // 0x43 a 8 bit
    uint8_t c2 = 0x43;
    if(!nfc_send_recv(&c2, 8, rx, sizeof(rx), &rx_bits, 50)) return false;
    if(rx[0] != 0x0A) return false;

    // 0xE1 0x00 con CRC
    uint8_t c3[2] = {0xE1, 0x00};
    if(!nfc_send_recv(c3, 16, rx, sizeof(rx), &rx_bits, 50)) return false;
    if(rx[0] != 0x0A) return false;

    // Comando sigillatura finale
    uint8_t seal[16] = {0x85,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x08};
    if(!nfc_send_recv(seal, 128, rx, sizeof(rx), &rx_bits, 100)) return false;
    return (rx[0] == 0x0A);
}

int32_t ufuid_sealer_app(void* p) {
    UNUSED(p);
    FURI_LOG_I(TAG, "Avvio UFUID Sealer");

    if(furi_hal_nfc_is_hal_ready() != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "NFC non disponibile");
        return -1;
    }

    furi_hal_nfc_acquire();
    furi_hal_nfc_low_power_mode_stop();
    furi_hal_nfc_set_mode(FuriHalNfcModePoller, FuriHalNfcTechIso14443a);

    furi_delay_ms(2000);

    if(write_bin_to_tag()) {
        FURI_LOG_I(TAG, "Scrittura OK. Sigillatura...");
        if(seal_ufuid()) {
            FURI_LOG_I(TAG, "SIGILLATO!");
        } else {
            FURI_LOG_E(TAG, "Errore sigillatura.");
        }
    } else {
        FURI_LOG_E(TAG, "Errore scrittura.");
    }

    furi_hal_nfc_low_power_mode_start();
    furi_hal_nfc_release();
    FURI_LOG_I(TAG, "Fine.");
    return 0;
}

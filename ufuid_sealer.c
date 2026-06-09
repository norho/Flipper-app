#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <furi_hal_nfc.h>

#define TAG "UFUID_Sealer"
#define DUMP_FILE_PATH EXT_PATH("nfc/bambu_tag_dump.bin")

static bool nfc_write_block(uint8_t block_num, uint8_t* data) {
    uint8_t tx[2] = {0xA0, block_num};
    uint8_t rx[1] = {0};
    uint16_t rx_bits = 0;

    furi_hal_nfc_set_tx_crc(true);
    furi_hal_nfc_set_rx_crc(true);

    if(furi_hal_nfc_tx_rx(tx, 2, rx, sizeof(rx), &rx_bits, 100) != FuriHalNfcReturnOk)
        return false;
    if(rx[0] != 0x0A) return false;

    if(furi_hal_nfc_tx_rx(data, 16, rx, sizeof(rx), &rx_bits, 100) != FuriHalNfcReturnOk)
        return false;
    return (rx[0] == 0x0A);
}

static bool write_bin_to_tag(const char* file_path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool success = true;

    if(storage_file_open(file, file_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint8_t block_data[16];
        uint8_t block_num = 0;
        while(storage_file_read(file, block_data, 16) == 16) {
            if(!nfc_write_block(block_num, block_data)) {
                FURI_LOG_E(TAG, "Errore blocco %d", block_num);
                success = false;
                break;
            }
            block_num++;
            furi_delay_ms(10);
        }
    } else {
        FURI_LOG_E(TAG, "File non trovato");
        success = false;
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return success;
}

static bool seal_ufuid(void) {
    uint8_t buf[1] = {0};
    uint16_t rx_bits = 0;

    furi_hal_nfc_set_tx_crc(false);
    furi_hal_nfc_set_rx_crc(false);

    uint8_t cmd1 = 0x40;
    if(furi_hal_nfc_tx_rx_bits(&cmd1, 7, buf, sizeof(buf), &rx_bits, 50) != FuriHalNfcReturnOk)
        return false;
    if(buf[0] != 0x0A) return false;

    uint8_t cmd2 = 0x43;
    if(furi_hal_nfc_tx_rx_bits(&cmd2, 8, buf, sizeof(buf), &rx_bits, 50) != FuriHalNfcReturnOk)
        return false;
    if(buf[0] != 0x0A) return false;

    furi_hal_nfc_set_tx_crc(true);
    furi_hal_nfc_set_rx_crc(true);

    uint8_t cmd3[2] = {0xE1, 0x00};
    if(furi_hal_nfc_tx_rx(cmd3, 2, buf, sizeof(buf), &rx_bits, 50) != FuriHalNfcReturnOk)
        return false;
    if(buf[0] != 0x0A) return false;

    uint8_t seal[16] = {0x85,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x08};
    if(furi_hal_nfc_tx_rx(seal, 16, buf, sizeof(buf), &rx_bits, 100) != FuriHalNfcReturnOk)
        return false;
    return (buf[0] == 0x0A);
}

int32_t ufuid_sealer_app(void* p) {
    UNUSED(p);
    FURI_LOG_I(TAG, "Avvio...");

    furi_hal_nfc_acquire();
    furi_hal_nfc_set_mode(FuriHalNfcModePoller, FuriHalNfcTechIso14443a);
    furi_hal_nfc_start();

    furi_delay_ms(2000);

    if(write_bin_to_tag(DUMP_FILE_PATH)) {
        FURI_LOG_I(TAG, "Scrittura OK. Avvio sigillatura...");
        if(seal_ufuid()) {
            FURI_LOG_I(TAG, "SIGILLATO!");
        } else {
            FURI_LOG_E(TAG, "Errore sigillatura.");
        }
    } else {
        FURI_LOG_E(TAG, "Errore scrittura.");
    }

    furi_hal_nfc_stop();
    furi_hal_nfc_release();
    FURI_LOG_I(TAG, "Fine.");
    return 0;
} 

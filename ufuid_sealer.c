#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>

#define TAG "UFUID_Sealer"
#define DUMP_FILE_PATH EXT_PATH("nfc/bambu_tag_dump.bin")

// Helper per gestire l'invio e la ricezione con le nuove API Poller
static bool nfc_send_recv(
    uint8_t* tx,
    size_t tx_bits,
    uint8_t* rx,
    size_t rx_max,
    size_t* rx_bits,
    uint32_t timeout_ms) {

    FuriHalNfcError err = furi_hal_nfc_poller_tx(tx, tx_bits);
    if(err != FuriHalNfcErrorNone) return false;

    FuriHalNfcEvent event = furi_hal_nfc_event_wait_for_specific_mask(
        FuriHalNfcEventRxEnd | FuriHalNfcEventTimeout, timeout_ms);
    if(!(event & FuriHalNfcEventRxEnd)) return false;

    err = furi_hal_nfc_poller_rx(rx, rx_max, rx_bits);
    return (err == FuriHalNfcErrorNone);
}

// Scrittura del singolo blocco MIFARE
static bool nfc_write_block(uint8_t block_num, uint8_t* data) {
    uint8_t rx[4];
    size_t rx_bits = 0;

    // ATTENZIONE: le API raw potrebbero richiedere la gestione manuale del CRC e della parità.
    // Se la scrittura fallisce, potrebbe essere necessario configurare il poller per aggiungere il CRC.
    uint8_t cmd[2] = {0xA0, block_num};
    if(!nfc_send_recv(cmd, 16, rx, sizeof(rx), &rx_bits, 100)) return false;
    if((rx_bits / 8) < 1 || rx[0] != 0x0A) return false;

    if(!nfc_send_recv(data, 128, rx, sizeof(rx), &rx_bits, 100)) return false;
    return ((rx_bits / 8) >= 1 && rx[0] == 0x0A);
}

// Lettura dal file system e scrittura iterativa
static bool write_bin_to_tag(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool success = true;

    if(storage_file_open(file, DUMP_FILE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint8_t block_data[16];
        uint8_t block_num = 0;
        
        while(storage_file_read(file, block_data, 16) == 16) {
            if(!nfc_write_block(block_num, block_data)) {
                FURI_LOG_E(TAG, "Errore di scrittura al blocco %d", block_num);
                success = false;
                break;
            }
            FURI_LOG_D(TAG, "Blocco %d scritto correttamente", block_num);
            block_num++;
            
            // Pausa per stabilizzare il campo RF tra un blocco e l'altro
            furi_delay_ms(10);
        }
    } else {
        FURI_LOG_E(TAG, "File non trovato o inaccessibile: %s", DUMP_FILE_PATH);
        success = false;
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return success;
}
#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>

#define TAG "UFUID_Sealer"
#define DUMP_FILE_PATH EXT_PATH("nfc/bambu_tag_dump.bin")

// Helper per gestire l'invio e la ricezione con le nuove API Poller
static bool nfc_send_recv(
    uint8_t* tx,
    size_t tx_bits,
    uint8_t* rx,
    size_t rx_max,
    size_t* rx_bits,
    uint32_t timeout_ms) {

    FuriHalNfcError err = furi_hal_nfc_poller_tx(tx, tx_bits);
    if(err != FuriHalNfcErrorNone) return false;

    FuriHalNfcEvent event = furi_hal_nfc_event_wait_for_specific_mask(
        FuriHalNfcEventRxEnd | FuriHalNfcEventTimeout, timeout_ms);
    if(!(event & FuriHalNfcEventRxEnd)) return false;

    err = furi_hal_nfc_poller_rx(rx, rx_max, rx_bits);
    return (err == FuriHalNfcErrorNone);
}

// Scrittura del singolo blocco MIFARE
static bool nfc_write_block(uint8_t block_num, uint8_t* data) {
    uint8_t rx[4];
    size_t rx_bits = 0;

    // ATTENZIONE: le API raw potrebbero richiedere la gestione manuale del CRC e della parità.
    // Se la scrittura fallisce, potrebbe essere necessario configurare il poller per aggiungere il CRC.
    uint8_t cmd[2] = {0xA0, block_num};
    if(!nfc_send_recv(cmd, 16, rx, sizeof(rx), &rx_bits, 100)) return false;
    if((rx_bits / 8) < 1 || rx[0] != 0x0A) return false;

    if(!nfc_send_recv(data, 128, rx, sizeof(rx), &rx_bits, 100)) return false;
    return ((rx_bits / 8) >= 1 && rx[0] == 0x0A);
}

// Lettura dal file system e scrittura iterativa
static bool write_bin_to_tag(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool success = true;

    if(storage_file_open(file, DUMP_FILE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint8_t block_data[16];
        uint8_t block_num = 0;
        
        while(storage_file_read(file, block_data, 16) == 16) {
            if(!nfc_write_block(block_num, block_data)) {
                FURI_LOG_E(TAG, "Errore di scrittura al blocco %d", block_num);
                success = false;
                break;
            }
            FURI_LOG_D(TAG, "Blocco %d scritto correttamente", block_num);
            block_num++;
            
            // Pausa per stabilizzare il campo RF tra un blocco e l'altro
            furi_delay_ms(10);
        }
    } else {
        FURI_LOG_E(TAG, "File non trovato o inaccessibile: %s", DUMP_FILE_PATH);
        success = false;
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return success;
}

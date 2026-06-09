#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>

#define TAG "UFUID_Sealer"
#define DUMP_FILE_PATH EXT_PATH("nfc/bambu_tag_dump.bin") // Percorso sulla MicroSD del Flipper

// Helper: Calcolo e accodamento del CRC-A (Ci svincola dalle impostazioni HAL)
static void append_crc(uint8_t* data, size_t len) {
    uint16_t crc = 0x6363; // ITU-V.41
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(uint8_t j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0x8408) : (crc >> 1);
        }
    }
    data[len] = crc & 0xFF;
    data[len+1] = (crc >> 8) & 0xFF;
}

// Helper: Gestione trasmissione e ricezione Raw con il nuovo Poller API
static bool nfc_send_recv(
    uint8_t* tx, size_t tx_bits,
    uint8_t* rx, size_t rx_max, size_t* rx_bits,
    uint32_t timeout_ms) {
    
    // Pulisce la coda degli interrupt del thread
    furi_thread_flags_clear(0xFFFFFFFF);
    
    // Invia i bit raw esatti richiesti
    FuriHalNfcError err = furi_hal_nfc_poller_tx(tx, tx_bits);
    if(err != FuriHalNfcErrorNone) return false;

    // Attesa della risposta sfruttando l'interrupt hardware di sistema
    uint32_t start = furi_get_tick();
    while((furi_get_tick() - start) < timeout_ms) {
        furi_thread_flags_wait(0xFFFFFFFF, FuriFlagWaitAny, timeout_ms);
        
        err = furi_hal_nfc_poller_rx(rx, rx_max, rx_bits);
        // Se riceviamo dati, la transazione è completata
        if(err == FuriHalNfcErrorNone && *rx_bits > 0) {
            return true;
        }
    }
    return false;
}

// Scrittura del singolo blocco MIFARE con CRC manuale
static bool nfc_write_block(uint8_t block_num, uint8_t* data) {
    uint8_t rx[4];
    size_t rx_bits = 0;

    // Comando Write (0xA0) + Numero Blocco + 2 byte di CRC
    uint8_t cmd[4] = {0xA0, block_num, 0, 0};
    append_crc(cmd, 2);
    
    if(!nfc_send_recv(cmd, 32, rx, sizeof(rx), &rx_bits, 20)) return false;
    if(rx_bits < 4 || (rx[0] & 0x0F) != 0x0A) return false; // Controllo ACK

    // Dati (16 byte) + 2 byte di CRC
    uint8_t write_data[18];
    memcpy(write_data, data, 16);
    append_crc(write_data, 16);
    
    if(!nfc_send_recv(write_data, 144, rx, sizeof(rx), &rx_bits, 20)) return false;
    return (rx_bits >= 4 && (rx[0] & 0x0F) == 0x0A);
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
            
            furi_delay_ms(10); // Pausa per stabilità RF
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
// Sequenza di blocco irreversibile per chip UFUID
static bool seal_ufuid(void) {
    uint8_t rx[4];
    size_t rx_bits = 0;

    // 1. Comando di sblocco a 7 bit (Nessun CRC)
    uint8_t c1 = 0x40;
    if(!nfc_send_recv(&c1, 7, rx, sizeof(rx), &rx_bits, 20)) return false;
    if((rx[0] & 0x0F) != 0x0A) return false;

    // 2. Secondo step di sblocco a 8 bit (Nessun CRC)
    uint8_t c2 = 0x43;
    if(!nfc_send_recv(&c2, 8, rx, sizeof(rx), &rx_bits, 20)) return false;
    if((rx[0] & 0x0F) != 0x0A) return false;

    // 3. Preparazione alla sigillatura (Richiede CRC)
    uint8_t c3[4] = {0xE1, 0x00, 0, 0};
    append_crc(c3, 2);
    if(!nfc_send_recv(c3, 32, rx, sizeof(rx), &rx_bits, 20)) return false;
    if((rx[0] & 0x0F) != 0x0A) return false;

    // 4. Invio della chiave di sigillatura (Richiede CRC)
    uint8_t seal[18] = {0x85, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0, 0};
    append_crc(seal, 16);
    if(!nfc_send_recv(seal, 144, rx, sizeof(rx), &rx_bits, 50)) return false;
    
    return ((rx[0] & 0x0F) == 0x0A);
}

// Entry point dell'applicazione per Flipper Zero
int32_t ufuid_sealer_app(void* p) {
    UNUSED(p);
    FURI_LOG_I(TAG, "Avvio UFUID Sealer...");

    // Controllo che l'hardware NFC sia libero
    if(furi_hal_nfc_is_hal_ready() != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "Hardware NFC non disponibile");
        return -1;
    }

    // Inizializzazione hardware RF
    furi_hal_nfc_acquire();
    furi_hal_nfc_low_power_mode_stop();
    furi_hal_nfc_set_mode(FuriHalNfcModePoller, FuriHalNfcTechIso14443a);

    FURI_LOG_I(TAG, "Avvicina il tag al lettore...");
    furi_delay_ms(2000); // Tempo pratico per posizionare il tag

    // Esecuzione sequenziale
    if(write_bin_to_tag()) {
        FURI_LOG_I(TAG, "Scrittura completata. Avvio sigillatura...");
        
        if(seal_ufuid()) {
            FURI_LOG_I(TAG, "SUCCESSO: Tag sigillato definitivamente!");
        } else {
            FURI_LOG_W(TAG, "Sigillatura fallita. (Se e' un FUID si e' auto-bloccato)");
        }
    } else {
        FURI_LOG_E(TAG, "Errore critico durante la scrittura. Annullato.");
    }

    // Rilascio sicuro dell'hardware
    furi_hal_nfc_low_power_mode_start();
    furi_hal_nfc_release();
    FURI_LOG_I(TAG, "Terminato.");
    
    return 0;
}

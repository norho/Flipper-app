#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <lib/nfc/nfc.h>

#define TAG "UFUID_Sealer"
#define DUMP_FILE_PATH EXT_PATH("nfc/bambu_tag_dump.bin") // Percorso sulla MicroSD del Flipper

// Funzione helper per scrivere un singolo blocco MIFARE tramite FuriHalNfc
bool furi_write_mifare_block(FuriHalNfc* nfc, uint8_t block_num, uint8_t* data) {
    uint8_t tx_buf[18];
    uint8_t rx_buf[18];
    size_t rx_bits;
    
    // Comando Write MIFARE Classic (0xA0) + Numero Blocco
    tx_buf[0] = 0xA0;
    tx_buf[1] = block_num;
    
    furi_hal_nfc_set_tx_crc(nfc, true);
    furi_hal_nfc_set_rx_crc(nfc, true);
    
    // Invio comando di scrittura
    furi_hal_nfc_tx_rx(nfc, tx_buf, 2, rx_buf, sizeof(rx_buf), &rx_bits, 100);
    if(rx_buf[0] != 0x0A) return false; // ACK = 0x0A
    
    // Invio dei 16 byte di dati
    furi_hal_nfc_tx_rx(nfc, data, 16, rx_buf, sizeof(rx_buf), &rx_bits, 100);
    if(rx_buf[0] != 0x0A) return false;

    return true;
}

// Funzione per leggere il file .bin dalla SD e scrivere i dati sul tag
bool write_bin_to_tag(FuriHalNfc* nfc, const char* file_path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool success = true;

    FURI_LOG_I(TAG, "Apertura file: %s", file_path);

    if(storage_file_open(file, file_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint8_t block_data[16];
        uint8_t block_num = 0;
        uint16_t bytes_read = 0;

        // Legge 16 byte alla volta dal file
        while((bytes_read = storage_file_read(file, block_data, 16)) == 16) {
            // Ignora il settore trailer (es. blocchi 3, 7, 11...) se non devi sovrascrivere le chiavi,
            // oppure scrivilo se il tuo dump .bin è pronto e formattato per l'intera memoria.
            if(!furi_write_mifare_block(nfc, block_num, block_data)) {
                FURI_LOG_E(TAG, "Errore scrittura al blocco %d", block_num);
                success = false;
                break;
            }
            FURI_LOG_D(TAG, "Blocco %d scritto con successo.", block_num);
            block_num++;
            
            // Pausa per stabilizzare la comunicazione RF
            furi_delay_ms(10); 
        }
    } else {
        FURI_LOG_E(TAG, "Impossibile aprire il file .bin dalla scheda SD");
        success = false;
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    return success;
}
// Equivalente Flipper Zero (C) della nostra sigillatura UFUID
bool furi_seal_ufuid(FuriHalNfc* nfc) {
    uint8_t buf[18];
    size_t rx_bits;
    
    // 1. Setup per inviare senza CRC e a 7 bit
    furi_hal_nfc_set_tx_crc(nfc, false);
    furi_hal_nfc_set_rx_crc(nfc, false);
    
    // 2. Invio 0x40
    buf[0] = 0x40;
    furi_hal_nfc_tx_rx_bits(nfc, buf, 7, buf, sizeof(buf), &rx_bits, 50);
    if(buf[0] != 0x0A) return false; // Controllo ACK

    // 3. Invio 0x43 a 8 bit
    buf[0] = 0x43;
    furi_hal_nfc_tx_rx_bits(nfc, buf, 8, buf, sizeof(buf), &rx_bits, 50);
    if(buf[0] != 0x0A) return false;

    // 4. Invio E1 00 con CRC riattivato
    furi_hal_nfc_set_tx_crc(nfc, true);
    furi_hal_nfc_set_rx_crc(nfc, true);
    buf[0] = 0xE1; buf[1] = 0x00;
    furi_hal_nfc_tx_rx(nfc, buf, 2, buf, sizeof(buf), &rx_bits, 50);
    if(buf[0] != 0x0A) return false;

    // 5. Sigillatura finale 0x85...
    uint8_t sealCmd[16] = {0x85,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x08};
    furi_hal_nfc_tx_rx(nfc, sealCmd, 16, buf, sizeof(buf), &rx_bits, 100);
    
    return (buf[0] == 0x0A); // Sigillato con successo!
}

// Entry point dell'applicazione Flipper (FAP)
int32_t ufuid_sealer_app(void* p) {
    UNUSED(p);
    
    FuriHalNfc* nfc = furi_hal_nfc_alloc();
    
    FURI_LOG_I(TAG, "Avvio Applicazione Scrittura e Sigillatura...");
    
    // Acquisizione esclusiva dell'hardware NFC del Flipper
    furi_hal_nfc_acquire();
    
    // Avvia il campo RF e imposta la tecnologia a ISO14443A (Mifare)
    furi_hal_nfc_tx_rx_on();
    furi_hal_nfc_set_technology(nfc, FuriHalNfcTechIso14443a);
    
    FURI_LOG_I(TAG, "Avvicina il tag UFUID...");
    
    // Attesa per rilevare il tag
    furi_delay_ms(2000); 

    FURI_LOG_I(TAG, "Inizio procedura di scrittura dal file .bin");
    
    // Step 1: Scrittura dei dati dalla SD al Tag
    if(write_bin_to_tag(nfc, DUMP_FILE_PATH)) {
        FURI_LOG_I(TAG, "Scrittura dati terminata con successo. Avvio sigillatura...");
        
        // Step 2: Esecuzione del comando di blocco irreversibile UFUID
        if(furi_seal_ufuid(nfc)) {
            FURI_LOG_I(TAG, "SIGILLATURA COMPLETATA! Il tag non e' piu' scrivibile.");
        } else {
            FURI_LOG_E(TAG, "ERRORE durante la sigillatura UFUID.");
        }
    } else {
        FURI_LOG_E(TAG, "ERRORE durante la scrittura dei dati. Sigillatura annullata.");
    }
    
    // Spegne il campo RF e rilascia l'hardware
    furi_hal_nfc_tx_rx_off();
    furi_hal_nfc_release();
    furi_hal_nfc_free(nfc);
    
    FURI_LOG_I(TAG, "Chiusura applicazione.");
    return 0;
}

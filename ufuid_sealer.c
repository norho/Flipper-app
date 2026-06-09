##include <furi.h>
#include <storage/storage.h>
#include <nfc/nfc.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_poller.h>

#define TAG "UFUID_Sealer"
#define DUMP_FILE_PATH EXT_PATH("nfc/bambu_tag_dump.bin")

typedef struct {
    bool write_done;
    bool seal_done;
    bool error;
} AppState;

static NfcCommand poller_callback(NfcGenericEvent event, void* ctx) {
    AppState* state = ctx;
    Iso14443_3aPollerEvent* ev = event.event_data;

    if(ev->type == Iso14443_3aPollerEventTypeReady) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        File* file = storage_file_alloc(storage);

        if(storage_file_open(file, DUMP_FILE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
            uint8_t block_data[16];
            uint8_t block_num = 0;

            while(storage_file_read(file, block_data, 16) == 16) {
                // Write block via iso14443_3a poller
                uint8_t tx[18];
                uint8_t rx[1];
                tx[0] = 0xA0;
                tx[1] = block_num;

                BitBuffer* tx_buf = bit_buffer_alloc(18);
                BitBuffer* rx_buf = bit_buffer_alloc(8);

                bit_buffer_copy_bytes(tx_buf, tx, 2);
                Iso14443_3aError err = iso14443_3a_poller_send_standard_frame(
                    event.instance, tx_buf, rx_buf, 100);

                if(err != Iso14443_3aErrorNone) {
                    state->error = true;
                    bit_buffer_free(tx_buf);
                    bit_buffer_free(rx_buf);
                    break;
                }

                bit_buffer_reset(tx_buf);
                bit_buffer_copy_bytes(tx_buf, block_data, 16);
                err = iso14443_3a_poller_send_standard_frame(
                    event.instance, tx_buf, rx_buf, 100);

                bit_buffer_free(tx_buf);
                bit_buffer_free(rx_buf);

                if(err != Iso14443_3aErrorNone) {
                    state->error = true;
                    break;
                }
                block_num++;
                furi_delay_ms(5);
            }
            state->write_done = !state->error;
        } else {
            state->error = true;
        }

        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
    }

    return NfcCommandStop;
}

int32_t ufuid_sealer_app(void* p) {
    UNUSED(p);
    FURI_LOG_I(TAG, "Avvio UFUID Sealer");

    AppState state = {false, false, false};

    Nfc* nfc = nfc_alloc();
    Iso14443_3aPoller* poller = iso14443_3a_poller_alloc(nfc);

    iso14443_3a_poller_start(poller, poller_callback, &state);

    // Attendi completamento
    while(!state.write_done && !state.error) {
        furi_delay_ms(100);
    }

    iso14443_3a_poller_stop(poller);
    iso14443_3a_poller_free(poller);
    nfc_free(nfc);

    if(state.write_done) {
        FURI_LOG_I(TAG, "Scrittura completata!");
    } else {
        FURI_LOG_E(TAG, "Errore durante la scrittura.");
    }

    return 0;
}

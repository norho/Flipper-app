#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <dialogs/dialogs.h>
#include <string.h>

#define TAG "UFUID_Sealer"

// Helper: Calcolo e accodamento del CRC-A
static void append_crc(uint8_t* data, size_t len) {
    uint16_t crc = 0x6363; 
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(uint8_t j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0x8408) : (crc >> 1);
        }
    }
    data[len] = crc & 0xFF;
    data[len+1] = (crc >> 8) & 0xFF;
}

// Helper: Gestione trasmissione e ricezione Raw con Poller API
static bool nfc_send_recv(
    uint8_t* tx, size_t tx_bits,
    uint8_t* rx, size_t rx_max, size_t* rx_bits,
    uint32_t timeout_ms) {
    
    furi_thread_flags_clear(0xFFFFFFFF);
    FuriHalNfcError err = furi_hal_nfc_poller_tx(tx, tx_bits);
    if(err != FuriHalNfcErrorNone) return false;

    uint32_t start = furi_get_tick();
    while((furi_get_tick() - start) < timeout_ms) {
        furi_thread_flags_wait(0xFFFFFFFF, FuriFlagWaitAny, timeout_ms);
        err = furi_hal_nfc_poller_rx(rx, rx_max, rx_bits);
        if(err == FuriHalNfcErrorNone && *rx_bits > 0) {
            return true;
        }
    }
    return false;
}

// Scrittura singolo blocco MIFARE con CRC
static bool nfc_write_block(uint8_t block_num, uint8_t* data) {
    uint8_t rx[4];
    size_t rx_bits = 0;

    uint8_t cmd[4] = {0xA0, block_num, 0, 0};
    append_crc(cmd, 2);
    if(!nfc_send_recv(cmd, 32, rx, sizeof(rx), &rx_bits, 20)) return false;
    if(rx_bits < 4 || (rx[0] & 0x0F) != 0x0A) return false;

    uint8_t write_data[18];
    memcpy(write_data, data, 16);
    append_crc(write_data, 16);
    if(!nfc_send_recv(write_data, 144, rx, sizeof(rx), &rx_bits, 20)) return false;
    
    return (rx_bits >= 4 && (rx[0] & 0x0F) == 0x0A);
}

// =========================================================
// MOTORE CONDIVISO CON FILE PATH DINAMICO
// =========================================================
static bool core_write_mifare_bin(const char* file_path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool success = true;

    if(storage_file_open(file, file_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint8_t block_data[16];
        uint8_t block0_data[16]; 
        uint8_t block_num = 0;
        bool has_block0 = false;
        
        while(storage_file_read(file, block_data, 16) == 16) {
            if(block_num == 0) {
                memcpy(block0_data, block_data, 16);
                has_block0 = true;
            } else {
                if(!nfc_write_block(block_num, block_data)) {
                    success = false;
                    break;
                }
            }
            block_num++;
            furi_delay_ms(10); 
        }

        if(success && has_block0) {
            if(!nfc_write_block(0, block0_data)) {
                success = false;
            }
        }
    } else {
        FURI_LOG_E(TAG, "File non trovato: %s", file_path);
        success = false;
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return success;
}

// =========================================================
// WRAPPER LOGICI E FUNZIONI NFC
// =========================================================
static bool write_fuid(const char* file_path) {
    return core_write_mifare_bin(file_path);
}

static bool write_ufuid(const char* file_path) {
    return core_write_mifare_bin(file_path);
}

static bool seal_ufuid(void) {
    uint8_t rx[4];
    size_t rx_bits = 0;

    uint8_t c1 = 0x40;
    if(!nfc_send_recv(&c1, 7, rx, sizeof(rx), &rx_bits, 20)) return false;
    if((rx[0] & 0x0F) != 0x0A) return false;

    uint8_t c2 = 0x43;
    if(!nfc_send_recv(&c2, 8, rx, sizeof(rx), &rx_bits, 20)) return false;
    if((rx[0] & 0x0F) != 0x0A) return false;

    uint8_t c3[4] = {0xE1, 0x00, 0, 0};
    append_crc(c3, 2);
    if(!nfc_send_recv(c3, 32, rx, sizeof(rx), &rx_bits, 20)) return false;
    if((rx[0] & 0x0F) != 0x0A) return false;

    uint8_t seal[18] = {0x85, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0, 0};
    append_crc(seal, 16);
    if(!nfc_send_recv(seal, 144, rx, sizeof(rx), &rx_bits, 50)) return false;
    
    return ((rx[0] & 0x0F) == 0x0A);
}
// =========================================================
// STRUTTURE DATI GUI E STATI
// =========================================================
typedef enum {
    AppMenu,
    AppProcessing,
    AppSuccess,
    AppError
} AppState;

typedef struct {
    AppState state;
    uint8_t menu_index;
    const char* menu_items[3];
    FuriMessageQueue* event_queue;
} AppContext;

// Funzione helper per l'esecuzione hardware isolata
static bool execute_nfc_action(uint8_t action_index, const char* file_path) {
    bool result = false;
    if(furi_hal_nfc_is_hal_ready() != FuriHalNfcErrorNone) return false;

    furi_hal_nfc_acquire();
    furi_hal_nfc_low_power_mode_stop();
    furi_hal_nfc_set_mode(FuriHalNfcModePoller, FuriHalNfcTechIso14443a);
    furi_delay_ms(1500); // Attesa fisiologica per stabilizzare il tag nel campo

    if(action_index == 0) result = write_fuid(file_path);
    else if(action_index == 1) result = write_ufuid(file_path);
    else if(action_index == 2) result = seal_ufuid();

    furi_hal_nfc_low_power_mode_start();
    furi_hal_nfc_release();
    return result;
}

// Callback di disegno su schermo
static void draw_callback(Canvas* canvas, void* ctx) {
    AppContext* context = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);

    if(context->state == AppMenu) {
        canvas_draw_str_aligned(canvas, 64, 5, AlignCenter, AlignTop, "Seleziona Azione");
        canvas_set_font(canvas, FontSecondary);
        for(uint8_t i = 0; i < 3; i++) {
            if(i == context->menu_index) {
                canvas_draw_box(canvas, 10, 20 + (i * 12), 108, 11);
                canvas_set_color(canvas, ColorWhite);
            } else {
                canvas_set_color(canvas, ColorBlack);
            }
            canvas_draw_str(canvas, 14, 29 + (i * 12), context->menu_items[i]);
            canvas_set_color(canvas, ColorBlack);
        }
    } else if(context->state == AppProcessing) {
        canvas_draw_str_aligned(canvas, 64, 25, AlignCenter, AlignCenter, "Avvicina il TAG...");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "Elaborazione in corso");
    } else if(context->state == AppSuccess) {
        canvas_draw_str_aligned(canvas, 64, 25, AlignCenter, AlignCenter, "SUCCESSO!");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "Premi BACK per tornare");
    } else if(context->state == AppError) {
        canvas_draw_str_aligned(canvas, 64, 25, AlignCenter, AlignCenter, "ERRORE");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "File errato o tag perso");
        canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignCenter, "Premi BACK");
    }
}

// Callback per la ricezione degli input hardware
static void input_callback(InputEvent* input_event, void* ctx) {
    AppContext* context = ctx;
    furi_message_queue_put(context->event_queue, input_event, FuriWaitForever);
}

// =========================================================
// ENTRY POINT DELL'APPLICAZIONE
// =========================================================
int32_t ufuid_sealer_app(void* p) {
    UNUSED(p);

    AppContext* context = malloc(sizeof(AppContext));
    context->state = AppMenu;
    context->menu_index = 0;
    context->menu_items[0] = "1. Scrivi tag FUID";
    context->menu_items[1] = "2. Scrivi tag UFUID";
    context->menu_items[2] = "3. Sigilla tag UFUID";
    context->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, context);
    view_port_input_callback_set(view_port, input_callback, context);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent event;
    bool running = true;

    // Main Loop GUI
    while(running) {
        if(furi_message_queue_get(context->event_queue, &event, 100) == FuriStatusOk) {
            if(event.type == InputTypeShort) {
                if(context->state == AppMenu) {
                    if(event.key == InputKeyBack) {
                        running = false;
                    } else if(event.key == InputKeyUp) {
                        if(context->menu_index > 0) context->menu_index--;
                    } else if(event.key == InputKeyDown) {
                        if(context->menu_index < 2) context->menu_index++;
                    } else if(event.key == InputKeyOk) {
                        
                        bool go_ahead = true;
                        FuriString* file_path = furi_string_alloc();
                        
                        // Avvia il File Browser se l'utente deve selezionare un file (Azione 1 o 2)
                        if (context->menu_index == 0 || context->menu_index == 1) {
                            furi_string_set(file_path, EXT_PATH("nfc"));
                            
                            DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
                            DialogsFileBrowserOptions browser_options;
                            dialog_file_browser_set_basic_options(&browser_options, ".bin", NULL);
                            browser_options.base_path = EXT_PATH("nfc");
                            
                            // Mostra l'esplora risorse. Se l'utente preme BACK nel browser, ritorna false
                            go_ahead = dialog_file_browser_show(dialogs, file_path, file_path, &browser_options);
                            furi_record_close(RECORD_DIALOGS);
                        }

                        // Se l'utente ha scelto un file (o se ha scelto "Sigilla"), proseguiamo
                        if (go_ahead) {
                            context->state = AppProcessing;
                            view_port_update(view_port);
                            
                            // Esegue l'azione NFC vera e propria
                            bool success = execute_nfc_action(context->menu_index, furi_string_get_cstr(file_path));
                            
                            context->state = success ? AppSuccess : AppError;
                            view_port_update(view_port);
                        }
                        
                        furi_string_free(file_path);
                    }
                } else if(context->state == AppSuccess || context->state == AppError) {
                    if(event.key == InputKeyBack || event.key == InputKeyOk) {
                        context->state = AppMenu;
                        view_port_update(view_port);
                    }
                }
            } else if(event.type == InputTypeLong && event.key == InputKeyBack) {
                running = false;
            }
        }
        view_port_update(view_port);
    }

    // Cleanup e rilascio memoria
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(context->event_queue);
    furi_record_close(RECORD_GUI);
    free(context);

    return 0;
}

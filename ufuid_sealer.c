#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <dialogs/dialogs.h>
#include <string.h>

#define TAG "UFUID_Sealer"

// =========================================================
// HELPER NFC E CALCOLI
// =========================================================

// Calcolo e accodamento del CRC-A standard
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

// Spedizione e ricezione raw sincronizzata con l'HAL del Flipper
static bool nfc_send_recv(uint8_t* tx, size_t tx_bits, uint8_t* rx, size_t rx_max, size_t* rx_bits) {
    furi_thread_flags_clear(0xFFFFFFFF);
    if(furi_hal_nfc_poller_tx(tx, tx_bits) != FuriHalNfcErrorNone) return false;
    
    // Attesa hardware nativa della risposta del tag
    if(furi_hal_nfc_poller_rx(rx, rx_max, rx_bits) != FuriHalNfcErrorNone) return false;
    return (*rx_bits > 0);
}

// Handshake ISO14443A per agganciare il tag ed entrare in stato ACTIVE
static bool nfc_iso14443a_wake_and_select(void) {
    uint8_t rx[32];
    size_t rx_bits = 0;
    
    // 1. WUPA (Wake-Up All - 0x52 a 7 bit) - Risveglia i tag nel campo RF
    uint8_t wupa = 0x52;
    if(!nfc_send_recv(&wupa, 7, rx, sizeof(rx), &rx_bits)) return false;
    
    // 2. Anti-collisione (0x93 0x20)
    uint8_t anticoll[2] = {0x93, 0x20};
    if(!nfc_send_recv(anticoll, 16, rx, sizeof(rx), &rx_bits)) return false;
    if(rx_bits < 40) return false; 
    
    // 3. Select (0x93 0x70 + UID + BCC + CRC)
    uint8_t select_cmd[9];
    select_cmd[0] = 0x93;
    select_cmd[1] = 0x70;
    memcpy(&select_cmd[2], rx, 5); 
    append_crc(select_cmd, 7);
    
    if(!nfc_send_recv(select_cmd, 72, rx, sizeof(rx), &rx_bits)) return false;
    
    return true;
}

// Scrittura singolo blocco MIFARE Classic
static bool nfc_write_block(uint8_t block_num, uint8_t* data) {
    uint8_t rx[4];
    size_t rx_bits = 0;

    uint8_t cmd[4] = {0xA0, block_num, 0, 0};
    append_crc(cmd, 2);
    if(!nfc_send_recv(cmd, 32, rx, sizeof(rx), &rx_bits)) return false;
    if(rx_bits < 4 || (rx[0] & 0x0F) != 0x0A) return false; 

    uint8_t write_data[18];
    memcpy(write_data, data, 16);
    append_crc(write_data, 16);
    if(!nfc_send_recv(write_data, 144, rx, sizeof(rx), &rx_bits)) return false;
    
    return (rx_bits >= 4 && (rx[0] & 0x0F) == 0x0A);
}

// =========================================================
// LOGICA DI COPIA E FILTRAGGIO CHIP
// =========================================================

static bool core_write_mifare_bin(const char* file_path, bool is_ufuid) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    uint8_t dump[1024];
    size_t bytes_read = 0;

    if(storage_file_open(file, file_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        bytes_read = storage_file_read(file, dump, sizeof(dump));
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    if(bytes_read != 1024) return false;

    // Se stiamo operando su UFUID, apriamo la backdoor Gen1 prima di scrivere
    if(is_ufuid) {
        uint8_t rx[4]; size_t rx_bits = 0;
        uint8_t c1 = 0x40;
        if(!nfc_send_recv(&c1, 7, rx, sizeof(rx), &rx_bits)) return false;
        uint8_t c2 = 0x43;
        if(!nfc_send_recv(&c2, 8, rx, sizeof(rx), &rx_bits)) return false;
    }

    // Scrittura sequenziale dei settori 1-63
    for(uint8_t i = 1; i < 64; i++) {
        if(!nfc_write_block(i, &dump[i * 16])) return false;
        furi_delay_ms(2);
    }
    
    // Scrittura finale del Blocco 0 (Previene il brick immediato del FUID)
    if(!nfc_write_block(0, &dump[0])) return false;

    return true;
}

static bool seal_ufuid(void) {
    uint8_t rx[4];
    size_t rx_bits = 0;

    uint8_t c1 = 0x40;
    if(!nfc_send_recv(&c1, 7, rx, sizeof(rx), &rx_bits)) return false;
    if((rx[0] & 0x0F) != 0x0A) return false;

    uint8_t c2 = 0x43;
    if(!nfc_send_recv(&c2, 8, rx, sizeof(rx), &rx_bits)) return false;
    if((rx[0] & 0x0F) != 0x0A) return false;

    uint8_t c3[4] = {0xE1, 0x00, 0, 0};
    append_crc(c3, 2);
    if(!nfc_send_recv(c3, 32, rx, sizeof(rx), &rx_bits)) return false;
    if((rx[0] & 0x0F) != 0x0A) return false;

    uint8_t seal[18] = {0x85, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0, 0};
    append_crc(seal, 16);
    if(!nfc_send_recv(seal, 144, rx, sizeof(rx), &rx_bits)) return false;
    
    return ((rx[0] & 0x0F) == 0x0A);
}

// =========================================================
// INTERFACCIA UTENTE E MACCHINA A STATI
// =========================================================

typedef enum { AppMenu, AppProcessing, AppSuccess, AppError } AppState;

typedef struct {
    AppState state;
    uint8_t menu_index;
    const char* menu_items[3];
    FuriMessageQueue* event_queue;
} AppContext;

// Funzione hardware riscritta con le API ufficiali
static bool execute_nfc_action(uint8_t action_index, const char* file_path) {
    bool result = false;
    if(furi_hal_nfc_is_hal_ready() != FuriHalNfcErrorNone) return false;

    // Inizializza l'hardware radio
    furi_hal_nfc_acquire();
    furi_hal_nfc_low_power_mode_stop();
    furi_hal_nfc_set_mode(FuriHalNfcModePoller, FuriHalNfcTechIso14443a);

    uint32_t start_time = furi_get_tick();
    bool tag_ready = false;

    // Ciclo di aggancio tag (massimo 4 secondi)
    while(furi_get_tick() - start_time < 4000) {
        if(nfc_iso14443a_wake_and_select()) {
            tag_ready = true;
            break;
        }
        furi_delay_ms(50); // Attesa prima del prossimo tentativo WUPA
    }

    if(tag_ready) {
        if(action_index == 0) result = core_write_mifare_bin(file_path, false); // FUID
        else if(action_index == 1) result = core_write_mifare_bin(file_path, true);  // UFUID
        else if(action_index == 2) result = seal_ufuid(); // Sigillatura
    }

    // Spegnimento sicuro dell'antenna e rilascio
    furi_hal_nfc_low_power_mode_start();
    furi_hal_nfc_release();
    return result;
}

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
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "Ricerca in corso...");
    } else if(context->state == AppSuccess) {
        canvas_draw_str_aligned(canvas, 64, 25, AlignCenter, AlignCenter, "SUCCESSO!");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "Premi BACK per tornare");
    } else if(context->state == AppError) {
        canvas_draw_str_aligned(canvas, 64, 25, AlignCenter, AlignCenter, "ERRORE");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "Tag non rilevato o fallito");
        canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignCenter, "Premi BACK");
    }
}

static void input_callback(InputEvent* input_event, void* ctx) {
    AppContext* context = ctx;
    furi_message_queue_put(context->event_queue, input_event, FuriWaitForever);
}

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
                        
                        if (context->menu_index == 0 || context->menu_index == 1) {
                            furi_string_set(file_path, EXT_PATH("nfc"));
                            
                            DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
                            DialogsFileBrowserOptions browser_options;
                            dialog_file_browser_set_basic_options(&browser_options, ".bin", NULL);
                            browser_options.base_path = EXT_PATH("nfc");
                            
                            go_ahead = dialog_file_browser_show(dialogs, file_path, file_path, &browser_options);
                            furi_record_close(RECORD_DIALOGS);
                        }

                        if (go_ahead) {
                            context->state = AppProcessing;
                            view_port_update(view_port);
                            
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

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(context->event_queue);
    furi_record_close(RECORD_GUI);
    free(context);

    return 0;
}

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
// HELPER E PROTOCOLLI RADIO ISO14443A
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
        
        if(event & FuriHalNfcEventRxEnd) {
            rx_success = true;
            break;
        }
        if((event & FuriHalNfcEventTimeout) && !(event & FuriHalNfcEventRxEnd)) {
            break; 
        }
    }

    if(!rx_success) return false;
    if(furi_hal_nfc_poller_rx(rx, rx_max, rx_bits) != FuriHalNfcErrorNone) return false;
    return (*rx_bits > 0);
}

static void nfc_send_halt_only(void) {
    uint8_t halt[4] = {0x50, 0x00, 0x00, 0x00};
    append_crc(halt, 2);
    furi_thread_flags_clear(0xFFFFFFFF);
    furi_hal_nfc_poller_tx(halt, 32);
    furi_hal_nfc_poller_wait_event(20); 
}

static void force_hardware_reset(void) {
    furi_hal_nfc_low_power_mode_start(); 
    furi_delay_ms(15);
    furi_hal_nfc_low_power_mode_stop();  
    furi_hal_nfc_set_mode(FuriHalNfcModePoller, FuriHalNfcTechIso14443a);
    furi_hal_nfc_poller_field_on(); 
    
    // FIX: Raddoppiato il tempo di carica condensatori RF per i FUID ostinati
    furi_delay_ms(80); 
}

static bool nfc_iso14443a_wake_and_select(void) {
    uint8_t rx[32];
    size_t rx_bits = 0;

    uint8_t wupa = 0x52;
    if(!nfc_send_recv(&wupa, 7, rx, sizeof(rx), &rx_bits)) return false;

    uint8_t anticoll[2] = {0x93, 0x20};
    if(!nfc_send_recv(anticoll, 16, rx, sizeof(rx), &rx_bits)) return false;
    if(rx_bits < 32) return false; 

    uint8_t select_cmd[9] = {0x93, 0x70, rx[0], rx[1], rx[2], rx[3], rx[4], 0, 0};
    append_crc(select_cmd, 7);
    if(!nfc_send_recv(select_cmd, 72, rx, sizeof(rx), &rx_bits)) return false;

    return true;
}

// =========================================================
// SEQUENZE BACKDOOR BASE
// =========================================================

static bool try_gen1_backdoor(void) {
    uint8_t rx[32];
    size_t  rx_bits = 0;

    FURI_LOG_I(TAG, "TRY BACKDOOR GEN1 COLD");
    uint8_t c1 = 0x40;
    
    if(!nfc_send_recv(&c1, 7, rx, sizeof(rx), &rx_bits)) return false;
    if(rx_bits < 4 || (rx[0] & 0x0F) != 0x0A) return false;

    uint8_t c2 = 0x43;
    if(!nfc_send_recv(&c2, 8, rx, sizeof(rx), &rx_bits)) return false;
    if(rx_bits < 4 || (rx[0] & 0x0F) != 0x0A) return false;

    return true;
}

static bool try_gen1_wake_backdoor(void) {
    uint8_t rx[32];
    size_t  rx_bits = 0;

    FURI_LOG_I(TAG, "TRY BACKDOOR GEN1 WAKE");
    if(!nfc_iso14443a_wake_and_select()) return false;

    uint8_t c1 = 0x40;
    if(!nfc_send_recv(&c1, 7, rx, sizeof(rx), &rx_bits)) return false;
    if(rx_bits < 4 || (rx[0] & 0x0F) != 0x0A) return false;

    uint8_t c2 = 0x43;
    if(!nfc_send_recv(&c2, 8, rx, sizeof(rx), &rx_bits)) return false;
    if(rx_bits < 4 || (rx[0] & 0x0F) != 0x0A) return false;

    return true;
}

// FIX: Sequenza stile Proxmark3 per cloni che richiedono l'Halt logico per entrare in backdoor
static bool try_gen1_proxmark_backdoor(void) {
    uint8_t rx[32];
    size_t  rx_bits = 0;

    FURI_LOG_I(TAG, "TRY BACKDOOR GEN1 PROXMARK");
    if(!nfc_iso14443a_wake_and_select()) return false;
    
    nfc_send_halt_only();
    
    // Attendiamo 20ms ma NON spegniamo il campo (il chip resta alimentato ma in stato Halt)
    furi_delay_ms(20); 

    uint8_t c1 = 0x40;
    if(!nfc_send_recv(&c1, 7, rx, sizeof(rx), &rx_bits)) return false;
    if(rx_bits < 4 || (rx[0] & 0x0F) != 0x0A) return false;

    uint8_t c2 = 0x43;
    if(!nfc_send_recv(&c2, 8, rx, sizeof(rx), &rx_bits)) return false;
    if(rx_bits < 4 || (rx[0] & 0x0F) != 0x0A) return false;

    return true;
}

static bool try_gen2_backdoor(void) {
    uint8_t rx[32];
    size_t rx_bits = 0;

    FURI_LOG_I(TAG, "TRY BACKDOOR GEN2 (UFUID)");
    if(!nfc_iso14443a_wake_and_select()) return false;

    uint8_t auth_cmd[] = {0x60, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
    append_crc(auth_cmd, 8);
    if(!nfc_send_recv(auth_cmd, 80, rx, sizeof(rx), &rx_bits)) return false;
    if(rx_bits < 32) return false; 

    uint8_t c1 = 0x40;
    if(!nfc_send_recv(&c1, 7, rx, sizeof(rx), &rx_bits)) return false;
    if(rx_bits < 4 || (rx[0] & 0x0F) != 0x0A) return false;

    uint8_t c2 = 0x43;
    if(!nfc_send_recv(&c2, 8, rx, sizeof(rx), &rx_bits)) return false;
    if(rx_bits < 4 || (rx[0] & 0x0F) != 0x0A) return false;

    return true;
}

// =========================================================
// MOTORI ISOLATI (FUID e UFUID)
// =========================================================

static bool prepare_fuid_tag(void) {
    if(try_gen1_backdoor()) return true;

    force_hardware_reset();
    if(try_gen1_wake_backdoor()) return true;

    force_hardware_reset();
    if(try_gen1_proxmark_backdoor()) return true;

    return false;
}

static bool prepare_ufuid_tag(void) {
    if(try_gen1_backdoor()) return true;

    force_hardware_reset();
    if(try_gen2_backdoor()) return true;

    force_hardware_reset();
    if(try_gen1_proxmark_backdoor()) return true;
    
    return false;
}
// =========================================================
// MOTORE DI SCRITTURA E SIGILLATURA
// =========================================================

static bool nfc_write_block(uint8_t block_num, uint8_t* data) {
    uint8_t rx[32];
    size_t  rx_bits = 0;

    uint8_t cmd[4] = {0xA0, block_num, 0, 0};
    append_crc(cmd, 2);
    if(!nfc_send_recv(cmd, 32, rx, sizeof(rx), &rx_bits)) {
        FURI_LOG_E(TAG, "Write B%d 0xA0 Timeout", block_num);
        return false;
    }
    if(rx_bits < 4 || (rx[0] & 0x0F) != 0x0A) {
        FURI_LOG_E(TAG, "Write B%d 0xA0 NAK", block_num);
        return false;
    }

    uint8_t write_data[18];
    memcpy(write_data, data, 16);
    append_crc(write_data, 16);
    if(!nfc_send_recv(write_data, 144, rx, sizeof(rx), &rx_bits)) {
        FURI_LOG_E(TAG, "Write B%d DATA Timeout", block_num);
        return false;
    }
    if(rx_bits < 4 || (rx[0] & 0x0F) != 0x0A) {
        FURI_LOG_E(TAG, "Write B%d DATA NAK", block_num);
        return false;
    }
    return true;
}

static bool core_write_mifare_bin(const char* file_path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file       = storage_file_alloc(storage);
    
    uint8_t* dump = malloc(1024);
    if(!dump) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }
    
    size_t bytes_read = 0;
    if(storage_file_open(file, file_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        bytes_read = storage_file_read(file, dump, 1024);
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    if(bytes_read != 1024) {
        free(dump);
        return false;
    }

    bool success = true;
    for(uint8_t i = 1; i < 64; i++) {
        if(!nfc_write_block(i, &dump[i * 16])) {
            success = false;
            break;
        }
        furi_delay_ms(5); 
    }
    
    if(success) {
        if(!nfc_write_block(0, &dump[0])) {
            success = false;
        }
    }

    free(dump);
    return success;
}

static bool seal_ufuid(void) {
    uint8_t rx[32];
    size_t  rx_bits = 0;

    uint8_t c3[4] = {0xE1, 0x00, 0, 0};
    append_crc(c3, 2);
    if(!nfc_send_recv(c3, 32, rx, sizeof(rx), &rx_bits)) return false;
    if(rx_bits < 4 || (rx[0] & 0x0F) != 0x0A) return false;

    uint8_t seal[18] = {
        0x85, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08,
        0x00, 0x00};
    append_crc(seal, 16);
    if(!nfc_send_recv(seal, 144, rx, sizeof(rx), &rx_bits)) return false;

    return (rx_bits >= 4 && (rx[0] & 0x0F) == 0x0A);
}

// =========================================================
// ROUTER ESECUZIONE NFC
// =========================================================

static bool execute_nfc_action(uint8_t action_index, const char* file_path) {
    bool result = false;

    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notifications, &sequence_blink_start_cyan);

    furi_hal_nfc_acquire();
    furi_hal_nfc_low_power_mode_stop();
    furi_hal_nfc_set_mode(FuriHalNfcModePoller, FuriHalNfcTechIso14443a);
    furi_hal_nfc_poller_field_on();
    
    // FIX: Ricarica profonda all'avvio del ciclo
    furi_delay_ms(80);

    uint32_t start_time = furi_get_tick();

    while(furi_get_tick() - start_time < 5000) {
        bool tag_ready = false;
        
        if (action_index == 0) {
            tag_ready = prepare_fuid_tag();
        } else {
            tag_ready = prepare_ufuid_tag();
        }

        if(tag_ready) {
            if(action_index == 0 || action_index == 1) {
                result = core_write_mifare_bin(file_path);
            } else if(action_index == 2) {
                result = seal_ufuid();
            }
            break; 
        }
        furi_delay_ms(50);
    }

    furi_hal_nfc_low_power_mode_start();
    furi_hal_nfc_release();

    notification_message(notifications, &sequence_blink_stop);
    if(result) {
        notification_message(notifications, &sequence_success);
    } else {
        notification_message(notifications, &sequence_error);
    }
    furi_record_close(RECORD_NOTIFICATION);

    return result;
}

// =========================================================
// GUI E ENTRY POINT
// =========================================================

typedef enum { AppMenu, AppProcessing, AppSuccess, AppError } AppState;

typedef struct {
    AppState          state;
    uint8_t           menu_index;
    const char* menu_items[3];
    FuriMessageQueue* event_queue;
} AppContext;

static void draw_callback(Canvas* canvas, void* ctx) {
    AppContext* context = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);

    if(context->state == AppMenu) {
        canvas_draw_str_aligned(canvas, 64, 5, AlignCenter, AlignTop, "UFUID Sealer v8.0");
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
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "Scrittura in corso...");
    } else if(context->state == AppSuccess) {
        canvas_draw_str_aligned(canvas, 64, 25, AlignCenter, AlignCenter, "SUCCESSO!");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "Operazione completata");
        canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignCenter, "Premi BACK per tornare");
    } else if(context->state == AppError) {
        canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, "ERRORE");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 35, AlignCenter, AlignCenter, "Tag non letto o fallito");
        canvas_draw_str_aligned(canvas, 64, 45, AlignCenter, AlignCenter, "Leggi log su qFlipper");
        canvas_draw_str_aligned(canvas, 64, 55, AlignCenter, AlignCenter, "Premi BACK");
    }
}

static void input_callback(InputEvent* input_event, void* ctx) {
    AppContext* context = ctx;
    furi_message_queue_put(context->event_queue, input_event, FuriWaitForever);
}

int32_t ufuid_sealer_app(void* p) {
    UNUSED(p);

    AppContext* context    = malloc(sizeof(AppContext));
    context->state         = AppMenu;
    context->menu_index    = 0;
    context->menu_items[0] = "1. Scrivi tag FUID";
    context->menu_items[1] = "2. Scrivi tag UFUID";
    context->menu_items[2] = "3. Sigilla tag UFUID";
    context->event_queue   = furi_message_queue_alloc(8, sizeof(InputEvent));

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, context);
    view_port_input_callback_set(view_port, input_callback, context);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent event;
    bool       running = true;

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
                        bool        go_ahead  = true;
                        FuriString* file_path = furi_string_alloc();

                        if(context->menu_index == 0 || context->menu_index == 1) {
                            furi_string_set(file_path, EXT_PATH("nfc"));
                            DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
                            DialogsFileBrowserOptions browser_options;
                            dialog_file_browser_set_basic_options(&browser_options, ".bin", NULL);
                            browser_options.base_path = EXT_PATH("nfc");
                            go_ahead = dialog_file_browser_show(dialogs, file_path, file_path, &browser_options);
                            furi_record_close(RECORD_DIALOGS);
                        }

                        if(go_ahead) {
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

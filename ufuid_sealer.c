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
    furi_delay_ms(40);
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
// BACKDOOR GEN1 E GEN2 (UFUID)
// =========================================================

static bool try_gen1_backdoor(void) {
    uint8_t rx[32];
    size_t  rx_bits = 0;

    FURI_LOG_I(TAG, "TRY BACKDOOR GEN1");
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

    FURI_LOG_I(TAG, "TRY BACKDOOR GEN2");
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

// Motore robusto: Gen1 -> Gen2 -> fallback halt+reset+Gen1
static bool prepare_magic_tag(void) {
    if(try_gen1_backdoor()) return true;

    force_hardware_reset();
    if(try_gen2_backdoor()) return true;

    force_hardware_reset();
    if(nfc_iso14443a_wake_and_select()) {
        nfc_send_halt_only();
        force_hardware_reset();
        if(try_gen1_backdoor()) return true;
    }
    return false;
}

// =========================================================
// MOTORE DI SCRITTURA E SIGILLATURA UFUID
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
    // Blocchi 1-63 prima, blocco 0 per ultimo (regola cinese)
    for(uint8_t i = 1; i < 64; i++) {
        if(!nfc_write_block(i, &dump[i * 16])) {
            success = false;
            break;
        }
        furi_delay_ms(5);
    }
    if(success) {
        if(!nfc_write_block(0, &dump[0])) success = false;
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
// ESECUZIONE NFC - solo per UFUID
// action_index: 0=Scrivi UFUID  1=Sigilla UFUID
// =========================================================

static bool execute_nfc_action(uint8_t action_index, const char* file_path) {
    bool result = false;

    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notifications, &sequence_blink_start_cyan);

    furi_hal_nfc_acquire();
    furi_hal_nfc_low_power_mode_stop();
    furi_hal_nfc_set_mode(FuriHalNfcModePoller, FuriHalNfcTechIso14443a);
    furi_hal_nfc_poller_field_on();
    furi_delay_ms(40);

    uint32_t start_time = furi_get_tick();

    while(furi_get_tick() - start_time < 5000) {
        if(prepare_magic_tag()) {
            if(action_index == 0) {
                result = core_write_mifare_bin(file_path);
            } else if(action_index == 1) {
                result = seal_ufuid();
            }
            break;
        }
        furi_delay_ms(50);
    }

    furi_hal_nfc_low_power_mode_start();
    furi_hal_nfc_release();

    notification_message(notifications, &sequence_blink_stop);
    notification_message(notifications, result ? &sequence_success : &sequence_error);
    furi_record_close(RECORD_NOTIFICATION);

    return result;
}

// =========================================================
// CONVERTITORE BIN -> NFC (per FUID)
// Il FUID non accetta scrittura diretta: va convertito in
// .nfc e scritto tramite l'app NFC nativa del Flipper.
// =========================================================

// Chiavi Bambu Lab - sostituire con quelle reali del dump
static const uint8_t bambu_key_a[16][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 0
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 1
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 2
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 3
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 4
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 5
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 6
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 7
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 8
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 9
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 10
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 11
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 12
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 13
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 14
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 15
};

static const uint8_t bambu_key_b[16][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 0
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 1
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 2
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 3
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 4
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 5
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 6
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 7
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 8
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 9
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 10
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 11
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 12
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 13
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 14
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // settore 15
};

// Copia il path sostituendo .bin con .nfc, senza strcat/strcpy
static void build_nfc_path(const char* bin_path, char* out_path, size_t out_size) {
    size_t len = 0;
    while(len < out_size - 1 && bin_path[len] != '\0') {
        out_path[len] = bin_path[len];
        len++;
    }
    out_path[len] = '\0';

    if(len >= 4 &&
       out_path[len-4] == '.' &&
       out_path[len-3] == 'b' &&
       out_path[len-2] == 'i' &&
       out_path[len-1] == 'n') {
        out_path[len-3] = 'n';
        out_path[len-2] = 'f';
        out_path[len-1] = 'c';
    } else {
        if(len + 4 < out_size) {
            out_path[len]   = '.';
            out_path[len+1] = 'n';
            out_path[len+2] = 'f';
            out_path[len+3] = 'c';
            out_path[len+4] = '\0';
        }
    }
}

static bool convert_bin_to_nfc(const char* bin_path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file_in    = storage_file_alloc(storage);
    File* file_out   = storage_file_alloc(storage);
    bool success     = false;

    uint8_t* dump = malloc(1024);
    if(!dump) {
        storage_file_free(file_in);
        storage_file_free(file_out);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    if(!storage_file_open(file_in, bin_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E(TAG, "Impossibile aprire il file .bin");
        goto cleanup;
    }
    if(storage_file_read(file_in, dump, 1024) != 1024) {
        FURI_LOG_E(TAG, "File .bin non valido (size != 1024)");
        goto cleanup;
    }
    storage_file_close(file_in);

    char out_path[256];
    build_nfc_path(bin_path, out_path, sizeof(out_path));
    FURI_LOG_I(TAG, "Output: %s", out_path);

    if(!storage_file_open(file_out, out_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FURI_LOG_E(TAG, "Impossibile creare .nfc");
        goto cleanup;
    }

    // Header Flipper NFC v4 - Mifare Classic 1K
    char header[512];
    int header_len = snprintf(
        header, sizeof(header),
        "Filetype: Flipper NFC device\n"
        "Version: 4\n"
        "Device type: Mifare Classic\n"
        "UID: %02X %02X %02X %02X\n"
        "ATQA: 00 04\n"
        "SAK: 08\n"
        "Mifare Classic type: 1K\n"
        "Data format version: 2\n"
        "# Mifare Classic blocks\n",
        dump[0], dump[1], dump[2], dump[3]);

    if(storage_file_write(file_out, header, header_len) != (uint16_t)header_len) {
        FURI_LOG_E(TAG, "Errore scrittura header");
        goto cleanup;
    }

    // 64 blocchi: trailer ricostruiti con chiavi Bambu, dati raw dal dump
    for(int block = 0; block < 64; block++) {
        char block_line[128];
        uint8_t* b      = &dump[block * 16];
        int sector      = block / 4;
        bool is_trailer = ((block % 4) == 3);
        int line_len;

        if(is_trailer) {
            // [KeyA 6B][AccessBits dal dump 4B][KeyB 6B]
            line_len = snprintf(
                block_line, sizeof(block_line),
                "Block %d: %02X %02X %02X %02X %02X %02X "
                "%02X %02X %02X %02X "
                "%02X %02X %02X %02X %02X %02X\n",
                block,
                bambu_key_a[sector][0], bambu_key_a[sector][1],
                bambu_key_a[sector][2], bambu_key_a[sector][3],
                bambu_key_a[sector][4], bambu_key_a[sector][5],
                b[6], b[7], b[8], b[9],
                bambu_key_b[sector][0], bambu_key_b[sector][1],
                bambu_key_b[sector][2], bambu_key_b[sector][3],
                bambu_key_b[sector][4], bambu_key_b[sector][5]);
        } else {
            line_len = snprintf(
                block_line, sizeof(block_line),
                "Block %d: %02X %02X %02X %02X %02X %02X %02X %02X "
                "%02X %02X %02X %02X %02X %02X %02X %02X\n",
                block,
                b[0],  b[1],  b[2],  b[3],
                b[4],  b[5],  b[6],  b[7],
                b[8],  b[9],  b[10], b[11],
                b[12], b[13], b[14], b[15]);
        }

        if(storage_file_write(file_out, block_line, line_len) != (uint16_t)line_len) {
            FURI_LOG_E(TAG, "Errore scrittura blocco %d", block);
            goto cleanup;
        }
    }

    success = true;
    FURI_LOG_I(TAG, "BIN->NFC OK: %s", out_path);

cleanup:
    storage_file_close(file_in);
    storage_file_close(file_out);
    storage_file_free(file_in);
    storage_file_free(file_out);
    furi_record_close(RECORD_STORAGE);
    free(dump);
    return success;
}

// =========================================================
// GUI E ENTRY POINT
//
// MENU:
//   0 = Scrivi UFUID   (.bin -> tag via backdoor Gen1/Gen2)
//   1 = Sigilla UFUID  (comando E1 dopo scrittura)
//   2 = FUID: bin>nfc  (.bin -> .nfc per app NFC nativa)
//
// FUID NON compare come "scrivi tag" perche' il Flipper
// non puo' scrivere un FUID direttamente: serve prima
// convertire il .bin in .nfc e poi usare l'app NFC nativa.
// =========================================================

#define MENU_ITEMS_COUNT 3

typedef enum { AppMenu, AppProcessing, AppSuccess, AppError, AppConverted } AppState;

typedef struct {
    AppState          state;
    uint8_t           menu_index;
    const char*       menu_items[MENU_ITEMS_COUNT];
    FuriMessageQueue* event_queue;
} AppContext;

static void draw_callback(Canvas* canvas, void* ctx) {
    AppContext* context = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);

    if(context->state == AppMenu) {
        canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "UFUID & FUID Tool v8");
        canvas_set_font(canvas, FontSecondary);
        for(uint8_t i = 0; i < MENU_ITEMS_COUNT; i++) {
            if(i == context->menu_index) {
                canvas_draw_box(canvas, 4, 18 + (i * 14), 120, 12);
                canvas_set_color(canvas, ColorWhite);
            } else {
                canvas_set_color(canvas, ColorBlack);
            }
            canvas_draw_str(canvas, 8, 27 + (i * 14), context->menu_items[i]);
            canvas_set_color(canvas, ColorBlack);
        }
    } else if(context->state == AppProcessing) {
        canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignCenter, "Avvicina il TAG...");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, "Scrittura in corso...");
    } else if(context->state == AppSuccess) {
        canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignCenter, "SUCCESSO!");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, "Operazione completata");
        canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignCenter, "Premi BACK per tornare");
    } else if(context->state == AppConverted) {
        canvas_draw_str_aligned(canvas, 64, 12, AlignCenter, AlignCenter, "CONVERTITO!");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignCenter, "File .nfc salvato su SD");
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, "Ora usa app NFC nativa");
        canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignCenter, "per scrivere il FUID");
        canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignCenter, "Premi BACK");
    } else if(context->state == AppError) {
        canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignCenter, "ERRORE");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Operazione fallita");
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, "Leggi log su qFlipper");
        canvas_draw_str_aligned(canvas, 64, 56, AlignCenter, AlignCenter, "Premi BACK");
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
    context->menu_items[0] = "1. Scrivi UFUID (.bin)";
    context->menu_items[1] = "2. Sigilla UFUID";
    context->menu_items[2] = "3. FUID: .bin -> .nfc";
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
                        if(context->menu_index < MENU_ITEMS_COUNT - 1) context->menu_index++;
                    } else if(event.key == InputKeyOk) {
                        bool        go_ahead  = true;
                        FuriString* file_path = furi_string_alloc();

                        // Voce 0 (scrivi UFUID) e voce 2 (converti FUID) richiedono .bin
                        if(context->menu_index == 0 || context->menu_index == 2) {
                            furi_string_set(file_path, EXT_PATH("nfc"));
                            DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
                            DialogsFileBrowserOptions browser_options;
                            dialog_file_browser_set_basic_options(
                                &browser_options, ".bin", NULL);
                            browser_options.base_path = EXT_PATH("nfc");
                            go_ahead = dialog_file_browser_show(
                                dialogs, file_path, file_path, &browser_options);
                            furi_record_close(RECORD_DIALOGS);
                        }

                        if(go_ahead) {
                            furi_delay_ms(200);
                            furi_message_queue_reset(context->event_queue);

                            if(context->menu_index == 2) {
                                // FUID: conversione pura, nessun radio
                                bool success = convert_bin_to_nfc(
                                    furi_string_get_cstr(file_path));
                                context->state = success ? AppConverted : AppError;
                                view_port_update(view_port);
                            } else {
                                // UFUID: operazioni radio
                                // menu 0 -> action 0 (scrivi), menu 1 -> action 1 (sigilla)
                                context->state = AppProcessing;
                                view_port_update(view_port);

                                bool success = execute_nfc_action(
                                    context->menu_index,
                                    furi_string_get_cstr(file_path));

                                context->state = success ? AppSuccess : AppError;
                                view_port_update(view_port);
                            }
                        }
                        furi_string_free(file_path);
                    }
                } else if(context->state != AppMenu && context->state != AppProcessing) {
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

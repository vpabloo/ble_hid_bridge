#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <bt/bt_service/bt.h>
#include <extra_profiles/hid_profile.h>
#include <furi_hal_usb_cdc.h>
#include <lib/libusb_stm32/inc/hid_usage_keyboard.h>

#define TAG "BleHidBridge"
#define MOD_SHIFT (1 << 8)
#define MAX_LINES 6

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    FuriThread* cdc_thread;
    FuriStreamBuffer* rx_stream;
    Bt* bt;
    FuriHalBleProfileBase* ble_hid_profile;
    
    // Terminal Log
    FuriString* history[MAX_LINES];
    
    bool bt_connected;
    volatile bool cdc_running;
} BleHidBridgeApp;

typedef enum {
    EventTypeTick,
    EventTypeKey,
    EventTypeCdcLine,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} AppEvent;

// Helper to append text to the terminal history
static void log_append(BleHidBridgeApp* app, const char* text) {
    if(!app || !text) return;
    
    // Rotate history: 0 is oldest, MAX_LINES-1 is newest
    // We reuse the string object at index 0, moving it to the end
    FuriString* temp = app->history[0];
    for(int i = 0; i < MAX_LINES - 1; i++) {
        app->history[i] = app->history[i+1];
    }
    app->history[MAX_LINES - 1] = temp;
    
    // Set new text
    furi_string_set(app->history[MAX_LINES - 1], text);
    
    // Trigger redraw
    view_port_update(app->view_port);
}

// Helper for parsing
static char* next_token(char** line) {
    char* token = *line;
    if(!token) return NULL;
    char* end = strchr(token, ' ');
    if(end) {
        *end = 0;
        *line = end + 1;
    } else {
        *line = NULL;
    }
    return token;
}

static int next_int(char** line, int default_val) {
    char* token = next_token(line);
    if(!token) return default_val;
    return atoi(token);
}

static uint16_t ascii_to_hid(char c) {
    if(c >= 'a' && c <= 'z') return HID_KEYBOARD_A + (c - 'a');
    if(c >= 'A' && c <= 'Z') return (HID_KEYBOARD_A + (c - 'A')) | MOD_SHIFT;
    if(c >= '1' && c <= '9') return HID_KEYBOARD_1 + (c - '1');
    if(c == '0') return HID_KEYBOARD_0;
    if(c == ' ') return HID_KEYBOARD_SPACEBAR;
    return 0;
}

static void process_line(BleHidBridgeApp* app, char* line) {
    // Log the raw command before parsing (parsing modifies the string)
    log_append(app, line);

    char* save = line;
    char* type = next_token(&save);
    
    if(type && strcmp(type, "M") == 0) {
        char* cmd = next_token(&save);
        if(cmd && strcmp(cmd, "MOVE") == 0) {
            int dx = next_int(&save, 0);
            int dy = next_int(&save, 0);
            ble_profile_hid_mouse_move(app->ble_hid_profile, (int8_t)dx, (int8_t)dy);
        } else if(cmd && strcmp(cmd, "BTN") == 0) {
            int mask = next_int(&save, 0);
            ble_profile_hid_mouse_press(app->ble_hid_profile, (uint16_t)mask); 
            furi_delay_ms(20);
            ble_profile_hid_mouse_release(app->ble_hid_profile, (uint16_t)mask);
        } else if(cmd && strcmp(cmd, "SCROLL") == 0) {
            int v = next_int(&save, 0);
            ble_profile_hid_mouse_scroll(app->ble_hid_profile, (int8_t)v);
        }
    } else if(type && strcmp(type, "K") == 0) {
        char* cmd = next_token(&save);
        if(cmd && strcmp(cmd, "TYPE") == 0) {
            char* rest = save;
            while(rest && *rest == ' ') rest++;
            if(rest && *rest) {
                const char* text = rest;
                while(*text) {
                    uint16_t combo = ascii_to_hid(*text++);
                    if(combo) {
                        if(combo & MOD_SHIFT) {
                             ble_profile_hid_kb_press(app->ble_hid_profile, HID_KEYBOARD_L_SHIFT);
                        }
                        ble_profile_hid_kb_press(app->ble_hid_profile, combo & 0xFF);
                        furi_delay_ms(15);
                        ble_profile_hid_kb_release(app->ble_hid_profile, combo & 0xFF);
                        if(combo & MOD_SHIFT) {
                             ble_profile_hid_kb_release(app->ble_hid_profile, HID_KEYBOARD_L_SHIFT);
                        }
                        furi_delay_ms(15);
                    }
                }
            }
        } else if(cmd && strcmp(cmd, "KEY") == 0) {
            int code = next_int(&save, 0);
            ble_profile_hid_kb_press(app->ble_hid_profile, (uint16_t)code);
            furi_delay_ms(20);
            ble_profile_hid_kb_release(app->ble_hid_profile, (uint16_t)code);
        }
    }
}

static int32_t cdc_thread_task(void* context) {
    BleHidBridgeApp* app = context;
    char buf[128];
    size_t idx = 0;
    while(app->cdc_running) {
        uint8_t byte;
        if(furi_stream_buffer_receive(app->rx_stream, &byte, 1, 50) == 1) {
            if(byte == '\n' || byte == '\r') {
                if(idx > 0) {
                    buf[idx] = 0;
                    process_line(app, buf);
                }
                idx = 0;
            } else if(idx < sizeof(buf) - 1) {
                buf[idx++] = (char)byte;
            } else {
                idx = 0; // Overflow, reset
            }
        }
    }
    return 0;
}

static void cdc_rx_callback(void* context) {
    BleHidBridgeApp* app = context;
    uint8_t buf[64];
    int32_t len = furi_hal_cdc_receive(0, buf, sizeof(buf));
    if(len > 0) {
        furi_stream_buffer_send(app->rx_stream, buf, len, 0);
    }
}

static CdcCallbacks cdc_cb = {
    .rx_ep_callback = cdc_rx_callback,
    .state_callback = NULL,
    .tx_ep_callback = NULL,
    .ctrl_line_callback = NULL,
    .config_callback = NULL,
};

static void draw_callback(Canvas* canvas, void* context) {
    BleHidBridgeApp* app = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    
    // Draw history lines
    // Line height approx 9-10px to be safe? 
    // FontSecondary is usually ~7px height + 1-2px leading. 
    // Let's use 10px spacing.
    int y = 9; // Baseline for first line
    
    for(int i = 0; i < MAX_LINES; i++) {
        const char* str = furi_string_get_cstr(app->history[i]);
        if(str && *str) {
            canvas_draw_str(canvas, 2, y, str);
        }
        y += 10;
    }
}

static void input_callback(InputEvent* input, void* context) {
    BleHidBridgeApp* app = context;
    AppEvent event;
    event.type = EventTypeKey;
    event.input = *input;
    furi_message_queue_put(app->event_queue, &event, 0);
}

static void bt_status_callback(BtStatus status, void* context) {
    BleHidBridgeApp* app = context;
    bool connected = (status == BtStatusConnected);
    
    if(app->bt_connected != connected) {
        app->bt_connected = connected;
        if(connected) {
            log_append(app, "BT: Connected");
        } else {
            log_append(app, "BT: Disconnected");
        }
    }
}

int32_t hid_ble_bridge_app(void* p) {
    UNUSED(p);
    BleHidBridgeApp* app = malloc(sizeof(BleHidBridgeApp));
    memset(app, 0, sizeof(BleHidBridgeApp));

    // Init history strings
    for(int i = 0; i < MAX_LINES; i++) {
        app->history[i] = furi_string_alloc();
    }
    
    app->event_queue = furi_message_queue_alloc(8, sizeof(AppEvent));
    app->rx_stream = furi_stream_buffer_alloc(512, 1);
    
    // UI
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);
    
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    // Initial logs
    log_append(app, "USB HID Bridge Ready");
    log_append(app, "Waiting for BT...");

    // BT Init
    app->bt = furi_record_open(RECORD_BT);
    bt_disconnect(app->bt);
    furi_delay_ms(200); 
    
    bt_set_status_changed_callback(app->bt, bt_status_callback, app);
    app->ble_hid_profile = bt_profile_start(app->bt, ble_profile_hid, NULL);
    furi_hal_bt_start_advertising();

    // USB Init
    app->cdc_running = true;
    app->cdc_thread = furi_thread_alloc();
    furi_thread_set_name(app->cdc_thread, "HidBridgeCdc");
    furi_thread_set_stack_size(app->cdc_thread, 1024);
    furi_thread_set_callback(app->cdc_thread, cdc_thread_task);
    furi_thread_set_context(app->cdc_thread, app);
    furi_thread_start(app->cdc_thread);

    furi_hal_usb_unlock();
    furi_hal_usb_set_config(&usb_cdc_single, NULL);
    furi_hal_cdc_set_callbacks(0, &cdc_cb, app);

    // Main Loop
    AppEvent event;
    while(1) {
        if(furi_message_queue_get(app->event_queue, &event, FuriWaitForever) == FuriStatusOk) {
            if(event.type == EventTypeKey) {
                if(event.input.type == InputTypeShort && event.input.key == InputKeyBack) {
                    break;
                }
            }
        }
    }

    // Cleanup
    app->cdc_running = false;
    furi_thread_join(app->cdc_thread);
    furi_thread_free(app->cdc_thread);
    
    furi_hal_usb_set_config(NULL, NULL);
    furi_stream_buffer_free(app->rx_stream);
    
    bt_set_status_changed_callback(app->bt, NULL, NULL);
    bt_disconnect(app->bt);
    furi_delay_ms(200);
    bt_profile_restore_default(app->bt);
    furi_record_close(RECORD_BT); 

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    
    furi_message_queue_free(app->event_queue);
    
    // Free history
    for(int i = 0; i < MAX_LINES; i++) {
        furi_string_free(app->history[i]);
    }
    
    free(app);

    return 0;
}

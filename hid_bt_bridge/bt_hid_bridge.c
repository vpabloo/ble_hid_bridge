#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <bt/bt_service/bt.h>
#include <extra_profiles/hid_profile.h>
#include <furi_hal_usb_cdc.h>
#include <lib/libusb_stm32/inc/hid_usage_keyboard.h>
#include <stdlib.h>

#define TAG "BtHidBridge"
#define MOD_SHIFT (1 << 8)
#define MAX_LINES 6

typedef enum {
    HidModeMain,
    HidModeAppSwitcher,
    HidModeWindowSwitcher,
    HidModeExitConfirm,
} HidMode;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    FuriThread* cdc_thread;
    FuriStreamBuffer* rx_stream;
    Bt* bt;
    FuriHalBleProfileBase* bt_hid_profile;

    // Terminal Log
    FuriString* history[MAX_LINES];

    bool bt_connected;
    volatile bool cdc_running;

    // HID state machine
    HidMode mode;
    bool command_held;
    bool exit_confirmed;

    // Visual feedback for the last physical input/event.
    InputKey last_key;
    bool has_last_key;
    uint32_t visual_event_counter;
} BtHidBridgeApp;

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
static void log_append(BtHidBridgeApp* app, const char* text) {
    if(!app || !text) return;

    FuriString* temp = app->history[0];
    for(int i = 0; i < MAX_LINES - 1; i++) {
        app->history[i] = app->history[i + 1];
    }
    app->history[MAX_LINES - 1] = temp;

    furi_string_set(app->history[MAX_LINES - 1], text);
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

/*
 * Send a complete HID key tap.
 * The short delay makes the press/release visible to the host without
 * holding the key long enough to trigger repeats.
 */
static void send_key_tap(BtHidBridgeApp* app, uint16_t key) {
    ble_profile_hid_kb_press(app->bt_hid_profile, key);
    furi_delay_ms(20);
    ble_profile_hid_kb_release(app->bt_hid_profile, key);
}

static void command_press(BtHidBridgeApp* app) {
    if(!app->command_held) {
        ble_profile_hid_kb_press(app->bt_hid_profile, HID_KEYBOARD_L_GUI);
        app->command_held = true;
    }
}

static void command_release(BtHidBridgeApp* app) {
    if(app->command_held) {
        ble_profile_hid_kb_release(app->bt_hid_profile, HID_KEYBOARD_L_GUI);
        app->command_held = false;
    }
}

/*
 * Emergency cleanup for every state.
 * All actions in this application are taps except Command, so Command is
 * the only key that can intentionally remain pressed across events.
 */
static void release_all_keys(BtHidBridgeApp* app) {
    command_release(app);
}

/*
 * Enter macOS application switcher:
 *   1. Press and hold Command.
 *   2. Tap Tab once.
 *   3. Remain in HidModeAppSwitcher with Command held.
 */
static void enter_app_switcher(BtHidBridgeApp* app) {
    command_press(app);
    send_key_tap(app, HID_KEYBOARD_TAB);
    app->mode = HidModeAppSwitcher;
    log_append(app, "MODE 2: APP SWITCHER");
}

/*
 * Select the highlighted application:
 * Enter/Return is sent while Command is still held, then Command is released.
 */
static void select_application(BtHidBridgeApp* app) {
    send_key_tap(app, HID_KEYBOARD_RETURN);
    command_release(app);
    app->mode = HidModeMain;
    log_append(app, "APP SELECTED");
}

/*
 * Cancel application selection without selecting anything.
 */
static void cancel_app_switcher(BtHidBridgeApp* app) {
    command_release(app);
    app->mode = HidModeMain;
    log_append(app, "APP SWITCHER: CANCEL");
}

/*
 * Transition from application selector to window selector.
 * The Left physical button means HID Up, then Command is released.
 */
static void enter_window_switcher(BtHidBridgeApp* app) {
    send_key_tap(app, HID_KEYBOARD_UP_ARROW);
    command_release(app);
    app->mode = HidModeWindowSwitcher;
    log_append(app, "MODE 3: WINDOW SWITCHER");
}

/*
 * Re-enter the application selector from the window selector.
 * This intentionally reconstructs the Command+Tab state instead of simply
 * changing the enum, so the HID state and UI state remain synchronized.
 */
static void window_switcher_back_to_apps(BtHidBridgeApp* app) {
    enter_app_switcher(app);
}

/*
 * Select the highlighted window.
 * Enter/Return is sent with Command released in Mode 3.
 */
static void select_window(BtHidBridgeApp* app) {
    send_key_tap(app, HID_KEYBOARD_RETURN);
    app->mode = HidModeMain;
    log_append(app, "WINDOW SELECTED");
}

/*
 * Permanent physical D-pad remap used by the normal menu and window mode:
 *   Left  -> Up
 *   Up    -> Right
 *   Right -> Down
 *   Down  -> Left
 */
static void remapped_direction(BtHidBridgeApp* app, InputKey key) {
    uint16_t hid_key = 0;

    switch(key) {
    case InputKeyLeft:
        hid_key = HID_KEYBOARD_UP_ARROW;
        break;
    case InputKeyUp:
        hid_key = HID_KEYBOARD_RIGHT_ARROW;
        break;
    case InputKeyRight:
        hid_key = HID_KEYBOARD_DOWN_ARROW;
        break;
    case InputKeyDown:
        hid_key = HID_KEYBOARD_LEFT_ARROW;
        break;
    default:
        return;
    }

    send_key_tap(app, hid_key);
}

static void handle_main_menu(BtHidBridgeApp* app, const InputEvent* input) {
    if(input->type == InputTypeShort) {
        visual_event(app, input->key);
        switch(input->key) {
        case InputKeyOk:
            enter_app_switcher(app);
            break;

        case InputKeyBack:
            // Short Back does not exit. Exit requires a deliberate hold.
            log_append(app, "HOLD BACK TO EXIT");
            break;

        case InputKeyLeft:
        case InputKeyUp:
        case InputKeyRight:
        case InputKeyDown:
            remapped_direction(app, input->key);
            break;

        default:
            break;
        }
    } else if(input->type == InputTypeLong && input->key == InputKeyBack) {
        visual_event(app, input->key);
        app->exit_confirmed = false;
        app->mode = HidModeExitConfirm;
        log_append(app, "EXIT? OK=YES BACK=NO");
    }
}

static void handle_app_switcher(BtHidBridgeApp* app, const InputEvent* input) {
    if(input->type != InputTypeShort) return;
    visual_event(app, input->key);

    switch(input->key) {
    case InputKeyUp:
        // Command remains held.
        send_key_tap(app, HID_KEYBOARD_RIGHT_ARROW);
        log_append(app, "APP: RIGHT");
        break;

    case InputKeyDown:
        // Command remains held.
        send_key_tap(app, HID_KEYBOARD_LEFT_ARROW);
        log_append(app, "APP: LEFT");
        break;

    case InputKeyOk:
        select_application(app);
        break;

    case InputKeyLeft:
        // Up is sent first, then Command is released and Mode 3 starts.
        enter_window_switcher(app);
        break;

    case InputKeyBack:
        cancel_app_switcher(app);
        break;

    default:
        break;
    }
}

static void handle_window_switcher(BtHidBridgeApp* app, const InputEvent* input) {
    if(input->type != InputTypeShort) return;
    visual_event(app, input->key);

    switch(input->key) {
    case InputKeyLeft:
    case InputKeyUp:
    case InputKeyRight:
    case InputKeyDown:
        remapped_direction(app, input->key);
        break;

    case InputKeyOk:
        select_window(app);
        break;

    case InputKeyBack:
        // Re-enter Mode 2 with a fresh Command+Tab sequence.
        window_switcher_back_to_apps(app);
        break;

    default:
        break;
    }
}

static void handle_exit_confirmation(BtHidBridgeApp* app, const InputEvent* input) {
    if(input->type != InputTypeShort) return;
    visual_event(app, input->key);

    switch(input->key) {
    case InputKeyOk:
        app->exit_confirmed = true;
        break;

    case InputKeyBack:
        app->exit_confirmed = false;
        app->mode = HidModeMain;
        log_append(app, "EXIT CANCELLED");
        break;

    default:
        break;
    }
}

static void perform_move_to(BtHidBridgeApp* app, int x, int y) {
    for(int i = 0; i < 50; i++) {
        ble_profile_hid_mouse_move(app->bt_hid_profile, -127, -127);
        furi_delay_ms(10);
    }

    int cur_x = 0;
    int cur_y = 0;
    const int step = 30;

    while(cur_x < x || cur_y < y) {
        int dx = x - cur_x;
        int dy = y - cur_y;

        if(dx > step) dx = step;
        if(dy > step) dy = step;
        if(dx < 0) dx = 0;
        if(dy < 0) dy = 0;

        if(dx == 0 && dy == 0) break;

        ble_profile_hid_mouse_move(app->bt_hid_profile, (int8_t)dx, (int8_t)dy);
        cur_x += dx;
        cur_y += dy;
        furi_delay_ms(10);
    }
}

static void process_line(BtHidBridgeApp* app, char* line) {
    log_append(app, line);

    char* save = line;
    char* type = next_token(&save);

    if(type && strcmp(type, "M") == 0) {
        char* cmd = next_token(&save);
        if(cmd && strcmp(cmd, "MOVE") == 0) {
            int dx = next_int(&save, 0);
            int dy = next_int(&save, 0);
            ble_profile_hid_mouse_move(app->bt_hid_profile, (int8_t)dx, (int8_t)dy);
        } else if(cmd && strcmp(cmd, "BTN") == 0) {
            int mask = next_int(&save, 0);
            ble_profile_hid_mouse_press(app->bt_hid_profile, (uint16_t)mask);
            furi_delay_ms(10 + rand() % 21);
            ble_profile_hid_mouse_release(app->bt_hid_profile, (uint16_t)mask);
        } else if(cmd && strcmp(cmd, "SCROLL") == 0) {
            int v = next_int(&save, 0);
            ble_profile_hid_mouse_scroll(app->bt_hid_profile, (int8_t)v);
        } else if(cmd && strcmp(cmd, "MOVETO") == 0) {
            int x = next_int(&save, 0);
            int y = next_int(&save, 0);
            perform_move_to(app, x, y);
        } else if(cmd && strcmp(cmd, "MOVECENTER") == 0) {
            int w = next_int(&save, 0);
            int h = next_int(&save, 0);
            perform_move_to(app, w / 2, h / 2);
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
                            ble_profile_hid_kb_press(app->bt_hid_profile, HID_KEYBOARD_L_SHIFT);
                        }
                        ble_profile_hid_kb_press(app->bt_hid_profile, combo & 0xFF);
                        furi_delay_ms(15);
                        ble_profile_hid_kb_release(app->bt_hid_profile, combo & 0xFF);
                        if(combo & MOD_SHIFT) {
                            ble_profile_hid_kb_release(app->bt_hid_profile, HID_KEYBOARD_L_SHIFT);
                        }
                        furi_delay_ms(15);
                    }
                }
            }
        } else if(cmd && strcmp(cmd, "KEY") == 0) {
            int code = next_int(&save, 0);
            ble_profile_hid_kb_press(app->bt_hid_profile, (uint16_t)code);
            furi_delay_ms(20);
            ble_profile_hid_kb_release(app->bt_hid_profile, (uint16_t)code);
        }
    }
}

static int32_t cdc_thread_task(void* context) {
    BtHidBridgeApp* app = context;
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
                idx = 0;
            }
        }
    }

    return 0;
}

static void cdc_rx_callback(void* context) {
    BtHidBridgeApp* app = context;
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


static void visual_event(BtHidBridgeApp* app, InputKey key) {
    app->last_key = key;
    app->has_last_key = true;
    app->visual_event_counter++;
    view_port_update(app->view_port);
}

static void draw_arrow(Canvas* canvas, int cx, int cy, int dx, int dy) {
    const int len = 8;
    const int head = 4;
    canvas_draw_line(canvas, cx - dx * len, cy - dy * len, cx + dx * len, cy + dy * len);

    if(dx == 0 && dy < 0) {
        canvas_draw_line(canvas, cx, cy - len, cx - head, cy - len + head);
        canvas_draw_line(canvas, cx, cy - len, cx + head, cy - len + head);
    } else if(dx == 0 && dy > 0) {
        canvas_draw_line(canvas, cx, cy + len, cx - head, cy + len - head);
        canvas_draw_line(canvas, cx, cy + len, cx + head, cy + len - head);
    } else if(dx < 0 && dy == 0) {
        canvas_draw_line(canvas, cx - len, cy, cx - len + head, cy - head);
        canvas_draw_line(canvas, cx - len, cy, cx - len + head, cy + head);
    } else if(dx > 0 && dy == 0) {
        canvas_draw_line(canvas, cx + len, cy, cx + len - head, cy - head);
        canvas_draw_line(canvas, cx + len, cy, cx + len - head, cy + head);
    }
}

static void draw_dpad(Canvas* canvas, int cx, int cy, int highlight) {
    canvas_draw_circle(canvas, cx, cy, 17);

    const int positions[4][2] = {
        {cx, cy - 10},
        {cx + 10, cy},
        {cx, cy + 10},
        {cx - 10, cy},
    };

    for(int i = 0; i < 4; i++) {
        if(i == highlight) {
            canvas_draw_disc(canvas, positions[i][0], positions[i][1], 5);
        } else {
            canvas_draw_circle(canvas, positions[i][0], positions[i][1], 5);
        }
    }

    canvas_draw_line(canvas, cx - 3, cy, cx + 3, cy);
    canvas_draw_line(canvas, cx, cy - 3, cx, cy + 3);
}

static int input_to_highlight(InputKey key) {
    switch(key) {
    case InputKeyUp:
        return 0;
    case InputKeyRight:
        return 1;
    case InputKeyDown:
        return 2;
    case InputKeyLeft:
        return 3;
    default:
        return -1;
    }
}

static const char* input_name(InputKey key) {
    switch(key) {
    case InputKeyUp:
        return "UP";
    case InputKeyDown:
        return "DOWN";
    case InputKeyLeft:
        return "LEFT";
    case InputKeyRight:
        return "RIGHT";
    case InputKeyOk:
        return "OK";
    case InputKeyBack:
        return "BACK";
    default:
        return "?";
    }
}

static void draw_event_badge(Canvas* canvas, InputKey key, bool command_held) {
    canvas_draw_rframe(canvas, 82, 14, 42, 24, 3);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 87, 25, "EVENT");
    canvas_draw_str(canvas, 87, 34, input_name(key));

    if(command_held) {
        canvas_draw_rframe(canvas, 83, 40, 40, 10, 2);
        canvas_draw_str(canvas, 89, 48, "CMD");
    }
}

static void draw_callback(Canvas* canvas, void* context) {
    BtHidBridgeApp* app = context;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);

    if(app->mode == HidModeAppSwitcher) {
        canvas_draw_str(canvas, 2, 9, "APP SWITCHER");
        canvas_set_font(canvas, FontSecondary);

        // Visual Command modifier badge and app-navigation arrows.
        canvas_draw_rframe(canvas, 2, 14, 32, 15, 3);
        canvas_draw_str(canvas, 8, 25, "CMD");
        draw_arrow(canvas, 50, 22, 1, 0);
        draw_arrow(canvas, 50, 39, -1, 0);

        canvas_draw_str(canvas, 62, 25, "UP=RIGHT");
        canvas_draw_str(canvas, 62, 42, "DN=LEFT");

        canvas_draw_rframe(canvas, 2, 47, 58, 14, 3);
        canvas_draw_str(canvas, 8, 57, "L = WINDOW");

        canvas_draw_str(canvas, 66, 57, "OK=SEL");

        if(app->has_last_key) {
            draw_event_badge(canvas, app->last_key, app->command_held);
        }
    } else if(app->mode == HidModeWindowSwitcher) {
        canvas_draw_str(canvas, 2, 9, "WINDOW SWITCHER");
        canvas_set_font(canvas, FontSecondary);

        // Central D-pad graphic. Highlight shows the last physical input.
        draw_dpad(canvas, 31, 35, app->has_last_key ? input_to_highlight(app->last_key) : -1);

        draw_arrow(canvas, 66, 22, 0, -1);
        draw_arrow(canvas, 86, 22, 1, 0);
        draw_arrow(canvas, 66, 40, 0, 1);
        draw_arrow(canvas, 86, 40, -1, 0);

        canvas_draw_str(canvas, 58, 54, "L U R D");
        canvas_draw_str(canvas, 58, 63, "U R D L");

        if(app->has_last_key) {
            draw_event_badge(canvas, app->last_key, app->command_held);
        }
    } else if(app->mode == HidModeExitConfirm) {
        canvas_draw_str(canvas, 2, 10, "EXIT APPLICATION?");
        canvas_set_font(canvas, FontSecondary);

        canvas_draw_rframe(canvas, 4, 18, 36, 28, 4);
        canvas_draw_str(canvas, 12, 29, "OK");
        canvas_draw_str(canvas, 9, 40, "YES");

        canvas_draw_rframe(canvas, 47, 18, 42, 28, 4);
        canvas_draw_str(canvas, 56, 29, "BACK");
        canvas_draw_str(canvas, 58, 40, "NO");

        canvas_draw_circle(canvas, 111, 32, 13);
        canvas_draw_line(canvas, 104, 32, 118, 32);
        canvas_draw_line(canvas, 111, 25, 111, 39);
        canvas_draw_str(canvas, 89, 57, "RELEASE CMD");

        if(app->has_last_key) {
            draw_event_badge(canvas, app->last_key, app->command_held);
        }
    } else {
        canvas_draw_str(canvas, 2, 9, "BT HID BRIDGE");
        canvas_set_font(canvas, FontSecondary);

        // Main screen: D-pad + destination arrows.
        draw_dpad(canvas, 31, 35, app->has_last_key ? input_to_highlight(app->last_key) : -1);

        draw_arrow(canvas, 70, 20, 0, -1);
        draw_arrow(canvas, 88, 20, 1, 0);
        draw_arrow(canvas, 70, 38, 0, 1);
        draw_arrow(canvas, 88, 38, -1, 0);

        canvas_draw_str(canvas, 60, 51, "L U R D");
        canvas_draw_str(canvas, 60, 60, "U R D L");

        canvas_draw_rframe(canvas, 2, 50, 44, 12, 3);
        canvas_draw_str(canvas, 8, 59, "OK=APP");

        if(app->has_last_key) {
            draw_event_badge(canvas, app->last_key, app->command_held);
        }
    }
}

static void input_callback(InputEvent* input, void* context) {
    BtHidBridgeApp* app = context;
    AppEvent event;
    event.type = EventTypeKey;
    event.input = *input;
    furi_message_queue_put(app->event_queue, &event, 0);
}

static void bt_status_callback(BtStatus status, void* context) {
    BtHidBridgeApp* app = context;
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

int32_t hid_bt_bridge_app(void* p) {
    UNUSED(p);

    BtHidBridgeApp* app = malloc(sizeof(BtHidBridgeApp));
    memset(app, 0, sizeof(BtHidBridgeApp));

    app->mode = HidModeMain;

    for(int i = 0; i < MAX_LINES; i++) {
        app->history[i] = furi_string_alloc();
    }

    app->event_queue = furi_message_queue_alloc(8, sizeof(AppEvent));
    app->rx_stream = furi_stream_buffer_alloc(512, 1);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    log_append(app, "USB HID Bridge Ready");
    log_append(app, "Waiting for BT...");

    app->bt = furi_record_open(RECORD_BT);
    bt_disconnect(app->bt);
    furi_delay_ms(200);

    bt_set_status_changed_callback(app->bt, bt_status_callback, app);
    app->bt_hid_profile = bt_profile_start(app->bt, ble_profile_hid, NULL);
    furi_hal_bt_start_advertising();

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

    AppEvent event;
    while(1) {
        if(furi_message_queue_get(app->event_queue, &event, FuriWaitForever) == FuriStatusOk) {
            if(event.type != EventTypeKey) continue;

            if(app->mode == HidModeMain) {
                handle_main_menu(app, &event.input);
            } else if(app->mode == HidModeAppSwitcher) {
                handle_app_switcher(app, &event.input);
            } else if(app->mode == HidModeWindowSwitcher) {
                handle_window_switcher(app, &event.input);
            } else if(app->mode == HidModeExitConfirm) {
                handle_exit_confirmation(app, &event.input);
            }

            if(app->exit_confirmed) {
                break;
            }

            view_port_update(app->view_port);
        }
    }

    // Always release Command before leaving the application.
    release_all_keys(app);

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

    for(int i = 0; i < MAX_LINES; i++) {
        furi_string_free(app->history[i]);
    }

    free(app);

    return 0;
}

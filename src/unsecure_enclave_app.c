#include "unsecure_enclave_app.h"
#include <furi_hal.h>
#include <furi_hal_random.h>
#include <gui/gui.h>
#include <gui/elements.h>

#define UE_SLOT_PATH_FMT UE_DIR "/slot_%03u.bin"

typedef enum {
    UeSceneStart,
    UeSceneMenu,
    UeSceneSlotMenu,
    UeSceneView,
    UeSceneNum,
} UeSceneId;

typedef struct {
    uint8_t remaining;
    bool enabled;
} StartModel;

static void ue_menu_cb(void* ctx, uint32_t index) {
    UnsecureEnclaveApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

// START VIEW
static void ue_start_timer_cb(void* ctx) {
    UnsecureEnclaveApp* app = ctx;
    if(!app || !app->start_view) return;
    StartModel* model = view_get_model(app->start_view);
    if(model->enabled) {
        // nothing to do
        view_commit_model(app->start_view, false);
        return;
    }
    if(model->remaining > 0) {
        model->remaining--;
        if(model->remaining == 0) model->enabled = true;
    }
    // trigger redraw
    view_commit_model(app->start_view, true);
}

static void ue_start_view_draw(Canvas* canvas, void* ctx) {
    StartModel* model = ctx;
    canvas_clear(canvas);
    // Background warning text
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 38, 8, "WARNING");

    // Tagline lines
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 21, 16, "It is called 'UN'secure");
    canvas_draw_str(canvas, 23, 24, "enclave for a reason");

    // Divider
    canvas_draw_line(canvas, 0, 26, 127, 26);

    // Details
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 38, "ALL KEYS ARE STORED");
    canvas_draw_str(canvas, 8, 50, "IN PLAINTEXT IN /INT");

    // Bottom action hint
    char btn[32];
    if(model->enabled) {
        snprintf(btn, sizeof(btn), "I understand");
    } else {
        snprintf(btn, sizeof(btn), "(%u)", (unsigned)model->remaining);
    }
    elements_button_center(canvas, btn);
}

static bool ue_start_view_input(InputEvent* event, void* ctx) {
    UnsecureEnclaveApp* app = ctx;
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        StartModel* model = view_get_model(app->start_view);
        bool can_continue = model->enabled;
        view_commit_model(app->start_view, false);
        if(can_continue) {
            scene_manager_next_scene(app->scene_manager, UeSceneMenu);
            return true;
        }
    }
    return false;
}

static void ue_slot_menu_enter(void* ctx) {
    UnsecureEnclaveApp* app = ctx;
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Slot actions");
    submenu_add_item(app->submenu, "View", 1, ue_menu_cb, app);
    submenu_add_item(app->submenu, "Generate Random", 2, ue_menu_cb, app);
    submenu_add_item(app->submenu, "Delete", 3, ue_menu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, UeViewSubmenu);
}

static bool ue_nav_cb(void* ctx) {
    UnsecureEnclaveApp* app = ctx;
    return scene_manager_handle_back_event(app->scene_manager);
}

static bool ue_cust_cb(void* ctx, uint32_t event) {
    UnsecureEnclaveApp* app = ctx;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static void ue_build_slot_list(UnsecureEnclaveApp* app) {
    variable_item_list_reset(app->varlist);
    for(uint8_t s = 1; s <= UE_MAX_SLOTS; s++) {
        char path[64];
        snprintf(path, sizeof(path), UE_SLOT_PATH_FMT, s);
        bool filled = storage_file_exists(app->storage, path);
        char label[32];
        snprintf(label, sizeof(label), "Slot %03u: %s", s, filled ? "Filled" : "Empty");
        VariableItem* it = variable_item_list_add(app->varlist, label, 0, NULL, NULL);
        (void)it;
    }
}

static void ue_varlist_enter_cb(void* ctx, uint32_t index) {
    UnsecureEnclaveApp* app = ctx;
    app->selected_slot = (uint8_t)(index + 1); // index is 0-based
    scene_manager_next_scene(app->scene_manager, UeSceneSlotMenu);
}

static void ue_scene_menu_on_enter(void* ctx) {
    UnsecureEnclaveApp* app = ctx;
    ue_build_slot_list(app);
    variable_item_list_set_enter_callback(app->varlist, ue_varlist_enter_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, UeViewVarList);
}

static void ue_scene_start_on_enter(void* ctx) {
    UnsecureEnclaveApp* app = ctx;
    // Initialize model
    StartModel* model = view_get_model(app->start_view);
    model->remaining = 10;
    model->enabled = false;
    view_commit_model(app->start_view, true);
    // Ensure timer exists and start it
    if(!app->start_timer) {
        app->start_timer = furi_timer_alloc(ue_start_timer_cb, FuriTimerTypePeriodic, app);
    }
    furi_timer_start(app->start_timer, furi_ms_to_ticks(1000));
    view_dispatcher_switch_to_view(app->view_dispatcher, UeViewStart);
}

static bool ue_read_slot(UnsecureEnclaveApp* app, FuriString* out) {
    char path[64];
    snprintf(path, sizeof(path), UE_SLOT_PATH_FMT, app->selected_slot);
    if(!storage_file_exists(app->storage, path)) return false;
    File* f = storage_file_alloc(app->storage);
    if(!storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) { storage_file_free(f); return false; }
    uint8_t hdr[4];
    size_t rd = storage_file_read(f, hdr, sizeof(hdr));
    if(rd != sizeof(hdr)) { storage_file_close(f); storage_file_free(f); return false; }
    uint8_t ver = hdr[0];
    uint8_t type = hdr[1];
    uint8_t ksize = hdr[2];
    // Basic header validation to avoid crashes on corrupt files
    if(ver != 1 || (type != 1 /* Simple */) || !(ksize == 16 || ksize == 24 || ksize == 32)) {
        storage_file_close(f);
        storage_file_free(f);
        return false;
    }
    FuriString* line = furi_string_alloc();
    furi_string_printf(line, "Slot %03u\nType: %u\nSize: %u\nKey (hex):\n", app->selected_slot, type, ksize);
    furi_string_cat(out, furi_string_get_cstr(line));
    furi_string_free(line);

    uint8_t buf[32];
    rd = storage_file_read(f, buf, ksize);
    storage_file_close(f);
    storage_file_free(f);
    if(rd != ksize) return false;
    for(size_t i = 0; i < rd; i++) {
        char b[3];
        snprintf(b, sizeof(b), "%02X", buf[i]);
        furi_string_cat(out, b);
        if((i % 32) == 31) furi_string_push_back(out, '\n');
    }
    return true;
}

static void ue_scene_view_on_enter(void* ctx) {
    UnsecureEnclaveApp* app = ctx;
    text_box_reset(app->textbox);
    furi_string_reset(app->view_text);
    if(!ue_read_slot(app, app->view_text)) {
        furi_string_set(app->view_text, "Empty or invalid slot");
    }
    text_box_set_text(app->textbox, furi_string_get_cstr(app->view_text));
    view_dispatcher_switch_to_view(app->view_dispatcher, UeViewText);
}

static void ue_generate_random(UnsecureEnclaveApp* app) {
    char path[64];
    snprintf(path, sizeof(path), UE_SLOT_PATH_FMT, app->selected_slot);
    if(storage_file_exists(app->storage, path)) return; // don't overwrite
    // Ensure directory exists
    storage_simply_mkdir(app->storage, UE_DIR);
    File* f = storage_file_alloc(app->storage);
    if(!storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) { storage_file_free(f); return; }
    uint8_t hdr[4] = {1, 1 /* Simple */, 32, 0};
    storage_file_write(f, hdr, sizeof(hdr));
    uint8_t key[32];
    furi_hal_random_fill_buf(key, sizeof(key));
    storage_file_write(f, key, sizeof(key));
    storage_file_sync(f);
    storage_file_close(f);
    storage_file_free(f);
}

static void ue_delete_slot(UnsecureEnclaveApp* app) {
    char path[64];
    snprintf(path, sizeof(path), UE_SLOT_PATH_FMT, app->selected_slot);
    if(storage_file_exists(app->storage, path)) {
        storage_common_remove(app->storage, path);
    }
}

static bool ue_scene_slot_menu_on_event(void* ctx, SceneManagerEvent ev) {
    UnsecureEnclaveApp* app = ctx;
    if(ev.type == SceneManagerEventTypeBack) {
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    if(ev.type == SceneManagerEventTypeCustom) {
        if(ev.event == 1) { // View
            scene_manager_next_scene(app->scene_manager, UeSceneView);
            return true;
        } else if(ev.event == 2) { // Generate
            ue_generate_random(app);
            scene_manager_previous_scene(app->scene_manager);
            return true;
        } else if(ev.event == 3) { // Delete
            ue_delete_slot(app);
            scene_manager_previous_scene(app->scene_manager);
            return true;
        }
    }
    return false;
}

static bool ue_scene_menu_on_event(void* ctx, SceneManagerEvent ev) {
    UnsecureEnclaveApp* app = ctx;
    if(ev.type == SceneManagerEventTypeBack) {
        scene_manager_stop(app->scene_manager);
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }
    return false;
}

static bool ue_scene_start_on_event(void* ctx, SceneManagerEvent ev) {
    UnsecureEnclaveApp* app = ctx;
    if(ev.type == SceneManagerEventTypeBack) {
        scene_manager_stop(app->scene_manager);
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }
    return false;
}

static void ue_scene_menu_on_exit(void* ctx) {
    UnsecureEnclaveApp* app = ctx;
    variable_item_list_reset(app->varlist);
}

static void ue_scene_start_on_exit(void* ctx) {
    UnsecureEnclaveApp* app = ctx;
    if(app->start_timer) furi_timer_stop(app->start_timer);
}

static void ue_scene_slot_menu_on_enter(void* ctx) {
    ue_slot_menu_enter(ctx);
}

static void ue_scene_view_on_exit(void* ctx) {
    UnsecureEnclaveApp* app = ctx;
    text_box_reset(app->textbox);
}

static void ue_scene_slot_menu_on_exit(void* ctx) {
    UnsecureEnclaveApp* app = ctx;
    submenu_reset(app->submenu);
    // When coming back to menu, refresh the list to reflect changes
    ue_build_slot_list(app);
}

static bool ue_scene_view_on_event(void* ctx, SceneManagerEvent ev) {
    UnsecureEnclaveApp* app = ctx;
    if(ev.type == SceneManagerEventTypeBack) {
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    return false;
}

// Scene handlers arrays must match SceneManagerHandlers definition
static void (*const ue_on_enter[])(void*) = {
    ue_scene_start_on_enter,
    ue_scene_menu_on_enter,
    ue_scene_slot_menu_on_enter,
    ue_scene_view_on_enter,
};

static bool (*const ue_on_event[])(void*, SceneManagerEvent) = {
    ue_scene_start_on_event,
    ue_scene_menu_on_event,
    ue_scene_slot_menu_on_event,
    ue_scene_view_on_event,
};

static void (*const ue_on_exit[])(void*) = {
    ue_scene_start_on_exit,
    ue_scene_menu_on_exit,
    ue_scene_slot_menu_on_exit,
    ue_scene_view_on_exit,
};

static const SceneManagerHandlers ue_scene_handlers = {
    .on_enter_handlers = ue_on_enter,
    .on_event_handlers = ue_on_event,
    .on_exit_handlers = ue_on_exit,
    .scene_num = UeSceneNum,
};

UnsecureEnclaveApp* unsecure_enclave_app_alloc(void) {
    UnsecureEnclaveApp* app = malloc(sizeof(UnsecureEnclaveApp));

    app->storage = furi_record_open(RECORD_STORAGE);
    app->view_text = furi_string_alloc();

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&ue_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, ue_cust_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, ue_nav_cb);

    app->start_view = view_alloc();
    view_allocate_model(app->start_view, ViewModelTypeLocking, sizeof(StartModel));
    view_set_context(app->start_view, app);
    view_set_draw_callback(app->start_view, ue_start_view_draw);
    view_set_input_callback(app->start_view, ue_start_view_input);
    view_dispatcher_add_view(app->view_dispatcher, UeViewStart, app->start_view);

    app->varlist = variable_item_list_alloc();
    view_dispatcher_add_view(app->view_dispatcher, UeViewVarList, variable_item_list_get_view(app->varlist));

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, UeViewSubmenu, submenu_get_view(app->submenu));

    app->textbox = text_box_alloc();
    view_dispatcher_add_view(app->view_dispatcher, UeViewText, text_box_get_view(app->textbox));

    app->start_timer = NULL;

    return app;
}

void unsecure_enclave_app_free(UnsecureEnclaveApp* app) {
    if(!app) return;

    view_dispatcher_remove_view(app->view_dispatcher, UeViewVarList);
    variable_item_list_free(app->varlist);

    view_dispatcher_remove_view(app->view_dispatcher, UeViewSubmenu);
    submenu_free(app->submenu);

    view_dispatcher_remove_view(app->view_dispatcher, UeViewText);
    text_box_free(app->textbox);

    view_dispatcher_remove_view(app->view_dispatcher, UeViewStart);
    view_free(app->start_view);

    scene_manager_free(app->scene_manager);

    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_STORAGE);
    furi_string_free(app->view_text);

    if(app->start_timer) furi_timer_free(app->start_timer);

    free(app);
}

int32_t unsecure_enclave_app(void* p) {
    UNUSED(p);
    UnsecureEnclaveApp* app = unsecure_enclave_app_alloc();

    Gui* gui = furi_record_open(RECORD_GUI);
    view_dispatcher_attach_to_gui(app->view_dispatcher, gui, ViewDispatcherTypeFullscreen);

    scene_manager_next_scene(app->scene_manager, UeSceneStart);

    view_dispatcher_run(app->view_dispatcher);

    unsecure_enclave_app_free(app);
    furi_record_close(RECORD_GUI);

    return 0;
}

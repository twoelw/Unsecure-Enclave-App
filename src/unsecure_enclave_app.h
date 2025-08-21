#pragma once
#include <furi.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/text_box.h>
#include <storage/storage.h>

#define UE_MAX_SLOTS 100
#define UE_DIR INT_PATH("enclave")

typedef enum {
    UeViewSubmenu,
    UeViewVarList,
    UeViewText,
} UeViewId;

typedef struct {
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    Submenu* submenu;
    VariableItemList* varlist;
    TextBox* textbox;

    Storage* storage;
    FuriString* view_text; // persistent buffer for TextBox content

    // state
    uint8_t selected_slot;
} UnsecureEnclaveApp;

UnsecureEnclaveApp* unsecure_enclave_app_alloc(void);
void unsecure_enclave_app_free(UnsecureEnclaveApp* app);
int32_t unsecure_enclave_app(void* p);

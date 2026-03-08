#include "rolljam_scene.h"

// ============================================================
// Menu scene: select frequency, modulation, start attack
// ============================================================

static void menu_freq_changed(VariableItem* item) {
    RollJamApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    app->freq_index = index;
    app->frequency = freq_values[index];
    variable_item_set_current_value_text(item, freq_names[index]);
}

static void menu_mod_changed(VariableItem* item) {
    RollJamApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    app->mod_index = index;
    variable_item_set_current_value_text(item, mod_names[index]);
}

static void menu_enter_callback(void* context, uint32_t index) {
    RollJamApp* app = context;

    if(index == 2) {
        // "Start Attack" item
        view_dispatcher_send_custom_event(
            app->view_dispatcher, RollJamEventStartAttack);
    }
}

void rolljam_scene_menu_on_enter(void* context) {
    RollJamApp* app = context;

    variable_item_list_reset(app->var_item_list);

    // --- Frequency ---
    VariableItem* freq_item = variable_item_list_add(
        app->var_item_list,
        "Frequency",
        FreqIndex_COUNT,
        menu_freq_changed,
        app);
    variable_item_set_current_value_index(freq_item, app->freq_index);
    variable_item_set_current_value_text(freq_item, freq_names[app->freq_index]);

    // --- Modulation ---
    VariableItem* mod_item = variable_item_list_add(
        app->var_item_list,
        "Modulation",
        ModIndex_COUNT,
        menu_mod_changed,
        app);
    variable_item_set_current_value_index(mod_item, app->mod_index);
    variable_item_set_current_value_text(mod_item, mod_names[app->mod_index]);

    // --- Start button ---
    variable_item_list_add(
        app->var_item_list,
        ">> START ATTACK <<",
        0,
        NULL,
        app);

    variable_item_list_set_enter_callback(
        app->var_item_list, menu_enter_callback, app);

    view_dispatcher_switch_to_view(
        app->view_dispatcher, RollJamViewVarItemList);
}

bool rolljam_scene_menu_on_event(void* context, SceneManagerEvent event) {
    RollJamApp* app = context;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == RollJamEventStartAttack) {
            // Clear previous captures
            memset(&app->signal_first, 0, sizeof(RawSignal));
            memset(&app->signal_second, 0, sizeof(RawSignal));

            scene_manager_next_scene(
                app->scene_manager, RollJamSceneAttackPhase1);
            return true;
        }
    }
    return false;
}

void rolljam_scene_menu_on_exit(void* context) {
    RollJamApp* app = context;
    variable_item_list_reset(app->var_item_list);
}

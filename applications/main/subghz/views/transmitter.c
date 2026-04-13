#include "transmitter.h"

#include <assets_icons.h>
#include <input/input.h>
#include <gui/elements.h>

#include <lib/subghz/blocks/custom_btn.h>

struct SubGhzViewTransmitter {
    View* view;
    SubGhzViewTransmitterCallback callback;
    void* context;
};

typedef struct {
    FuriString* frequency_str;
    FuriString* preset_str;
    FuriString* key_str;
    bool show_button;
    SubGhzRadioDeviceType device_type;
    FuriString* temp_button_id;
    bool draw_temp_button;
} SubGhzViewTransmitterModel;

void subghz_view_transmitter_set_callback(
    SubGhzViewTransmitter* subghz_transmitter,
    SubGhzViewTransmitterCallback callback,
    void* context) {
    furi_assert(subghz_transmitter);

    subghz_transmitter->callback = callback;
    subghz_transmitter->context = context;
}

void subghz_view_transmitter_add_data_to_show(
    SubGhzViewTransmitter* subghz_transmitter,
    const char* key_str,
    const char* frequency_str,
    const char* preset_str,
    bool show_button) {
    furi_assert(subghz_transmitter);
    with_view_model(
        subghz_transmitter->view,
        SubGhzViewTransmitterModel * model,
        {
            furi_string_set(model->key_str, key_str);
            furi_string_set(model->frequency_str, frequency_str);
            furi_string_set(model->preset_str, preset_str);
            model->show_button = show_button;
        },
        true);
}

void subghz_view_transmitter_set_radio_device_type(
    SubGhzViewTransmitter* subghz_transmitter,
    SubGhzRadioDeviceType device_type) {
    furi_assert(subghz_transmitter);
    with_view_model(
        subghz_transmitter->view,
        SubGhzViewTransmitterModel * model,
        { model->device_type = device_type; },
        true);
}

void subghz_view_transmitter_draw(Canvas* canvas, SubGhzViewTransmitterModel* model) {
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    elements_multiline_text_aligned(
        canvas, 0, 0, AlignLeft, AlignTop, furi_string_get_cstr(model->key_str));
    canvas_draw_str(canvas, 78, 7, furi_string_get_cstr(model->frequency_str));
    canvas_draw_str(canvas, 113, 7, furi_string_get_cstr(model->preset_str));

    if(model->draw_temp_button) {
        canvas_set_font(canvas, FontBatteryPercent);
        canvas_draw_str(canvas, 117, 40, furi_string_get_cstr(model->temp_button_id));
        canvas_set_font(canvas, FontSecondary);
    }

    if(model->show_button) {
        canvas_draw_str(
            canvas,
            58,
            62,
            (model->device_type == SubGhzRadioDeviceTypeInternal) ? "R: Int" : "R: Ext");

        canvas_draw_icon(canvas, 92, 16, &I_ButtonUp_7x4);
        canvas_draw_icon(canvas, 92, 44, &I_ButtonDown_7x4);
        canvas_draw_icon(canvas, 71, 29, &I_ButtonLeft_4x7);
        canvas_draw_icon(canvas, 108, 29, &I_ButtonRight_4x7);
        canvas_draw_icon(canvas, 91, 28, &I_ButtonCenter_7x7);
    }
}

bool subghz_view_transmitter_input(InputEvent* event, void* context) {
    furi_assert(context);
    SubGhzViewTransmitter* subghz_transmitter = context;
    bool can_be_sent = false;

    if(event->key == InputKeyBack && event->type == InputTypeLong) {
        // Reset view model
        with_view_model(
            subghz_transmitter->view,
            SubGhzViewTransmitterModel * model,
            {
                furi_string_reset(model->frequency_str);
                furi_string_reset(model->preset_str);
                furi_string_reset(model->key_str);
                furi_string_reset(model->temp_button_id);
                model->show_button = false;
                model->draw_temp_button = false;
            },
            false);
        return false;
    } // Finish "Back" key processing

    with_view_model(
        subghz_transmitter->view,
        SubGhzViewTransmitterModel * model,
        {
            if(model->show_button) {
                can_be_sent = true;
            }
        },
        true);

    if(can_be_sent) {
        // Long press d-pad: set custom btn + long flag (no send here, send happens below)
        if(event->type == InputTypeLong) {
            if(event->key == InputKeyUp) {
                subghz_custom_btn_set(SUBGHZ_CUSTOM_BTN_UP);
                subghz_custom_btn_set_long(true);
            } else if(event->key == InputKeyDown) {
                subghz_custom_btn_set(SUBGHZ_CUSTOM_BTN_DOWN);
                subghz_custom_btn_set_long(true);
            } else if(event->key == InputKeyLeft) {
                subghz_custom_btn_set(SUBGHZ_CUSTOM_BTN_LEFT);
                subghz_custom_btn_set_long(true);
            } else if(event->key == InputKeyRight) {
                subghz_custom_btn_set(SUBGHZ_CUSTOM_BTN_RIGHT);
                subghz_custom_btn_set_long(true);
            }
        }

        // OK button handling
        if(event->key == InputKeyOk) {
            if(event->type == InputTypePress) {
                if(subghz_custom_btn_has_pages()) {
                    // Multi-page protocol: cycle pages, do NOT send
                    uint8_t max_pages = subghz_custom_btn_get_max_pages();
                    uint8_t next_page = (subghz_custom_btn_get_page() + 1) % max_pages;
                    subghz_custom_btn_set_page(next_page);
                    // Reset d-pad selection to OK so display shows original btn
                    subghz_custom_btn_set(SUBGHZ_CUSTOM_BTN_OK);
                    with_view_model(
                        subghz_transmitter->view,
                        SubGhzViewTransmitterModel * model,
                        {
                            furi_string_reset(model->temp_button_id);
                            furi_string_printf(model->temp_button_id, "P%u", next_page + 1);
                            model->draw_temp_button = true;
                        },
                        true);
                    // Refresh display with new page mapping
                    subghz_transmitter->callback(
                        SubGhzCustomEventViewTransmitterPageChange, subghz_transmitter->context);
                    return true;
                }
                // Normal protocol: send original button
                subghz_custom_btn_set(SUBGHZ_CUSTOM_BTN_OK);
                with_view_model(
                    subghz_transmitter->view,
                    SubGhzViewTransmitterModel * model,
                    {
                        furi_string_reset(model->temp_button_id);
                        model->draw_temp_button = false;
                    },
                    true);
                subghz_transmitter->callback(
                    SubGhzCustomEventViewTransmitterSendStart, subghz_transmitter->context);
                return true;
            } else if(event->type == InputTypeRelease) {
                // Only stop TX if we actually started it (not a page toggle)
                if(!subghz_custom_btn_has_pages()) {
                    subghz_transmitter->callback(
                        SubGhzCustomEventViewTransmitterSendStop, subghz_transmitter->context);
                }
                return true;
            }
        } // Finish "OK" key processing

        if(subghz_custom_btn_is_allowed()) {
            uint8_t temp_btn_id;
            if(event->key == InputKeyUp) {
                temp_btn_id = SUBGHZ_CUSTOM_BTN_UP;
            } else if(event->key == InputKeyDown) {
                temp_btn_id = SUBGHZ_CUSTOM_BTN_DOWN;
            } else if(event->key == InputKeyLeft) {
                temp_btn_id = SUBGHZ_CUSTOM_BTN_LEFT;
            } else if(event->key == InputKeyRight) {
                temp_btn_id = SUBGHZ_CUSTOM_BTN_RIGHT;
            } else {
                // Finish processing if the button is different
                return true;
            }

            if(event->type == InputTypePress) {
                with_view_model(
                    subghz_transmitter->view,
                    SubGhzViewTransmitterModel * model,
                    {
                        furi_string_reset(model->temp_button_id);
                        if(subghz_custom_btn_get_original() != 0) {
                            if(subghz_custom_btn_set(temp_btn_id)) {
                                furi_string_printf(
                                    model->temp_button_id,
                                    "%01X",
                                    subghz_custom_btn_get_original());
                                model->draw_temp_button = true;
                            }
                        }
                    },
                    true);
                subghz_transmitter->callback(
                    SubGhzCustomEventViewTransmitterSendStart, subghz_transmitter->context);
                return true;
            } else if(event->type == InputTypeRelease) {
                subghz_transmitter->callback(
                    SubGhzCustomEventViewTransmitterSendStop, subghz_transmitter->context);
                return true;
            }
        }
    }

    return true;
}

void subghz_view_transmitter_enter(void* context) {
    furi_assert(context);
}

void subghz_view_transmitter_exit(void* context) {
    furi_assert(context);
}

SubGhzViewTransmitter* subghz_view_transmitter_alloc(void) {
    SubGhzViewTransmitter* subghz_transmitter = malloc(sizeof(SubGhzViewTransmitter));

    // View allocation and configuration
    subghz_transmitter->view = view_alloc();
    view_allocate_model(
        subghz_transmitter->view, ViewModelTypeLocking, sizeof(SubGhzViewTransmitterModel));
    view_set_context(subghz_transmitter->view, subghz_transmitter);
    view_set_draw_callback(
        subghz_transmitter->view, (ViewDrawCallback)subghz_view_transmitter_draw);
    view_set_input_callback(subghz_transmitter->view, subghz_view_transmitter_input);
    view_set_enter_callback(subghz_transmitter->view, subghz_view_transmitter_enter);
    view_set_exit_callback(subghz_transmitter->view, subghz_view_transmitter_exit);

    with_view_model(
        subghz_transmitter->view,
        SubGhzViewTransmitterModel * model,
        {
            model->frequency_str = furi_string_alloc();
            model->preset_str = furi_string_alloc();
            model->key_str = furi_string_alloc();
            model->temp_button_id = furi_string_alloc();
        },
        true);
    return subghz_transmitter;
}

void subghz_view_transmitter_free(SubGhzViewTransmitter* subghz_transmitter) {
    furi_assert(subghz_transmitter);

    with_view_model(
        subghz_transmitter->view,
        SubGhzViewTransmitterModel * model,
        {
            furi_string_free(model->frequency_str);
            furi_string_free(model->preset_str);
            furi_string_free(model->key_str);
            furi_string_free(model->temp_button_id);
        },
        true);
    view_free(subghz_transmitter->view);
    free(subghz_transmitter);
}

View* subghz_view_transmitter_get_view(SubGhzViewTransmitter* subghz_transmitter) {
    furi_assert(subghz_transmitter);
    return subghz_transmitter->view;
}

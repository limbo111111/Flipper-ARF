#include "../subghz_i.h"
#include <lib/subghz/protocols/keeloq.h>
#include <dialogs/dialogs.h>

enum {
    KlBf2IndexLoadSig1,
    KlBf2IndexLoadSig2,
    KlBf2IndexStartBf,
};

static bool kl_bf2_extract_key(SubGhz* subghz, uint32_t* out_fix, uint32_t* out_hop) {
    FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
    flipper_format_rewind(fff);
    uint8_t key_data[8] = {0};
    if(!flipper_format_read_hex(fff, "Key", key_data, 8)) return false;
    *out_fix = ((uint32_t)key_data[0] << 24) | ((uint32_t)key_data[1] << 16) |
               ((uint32_t)key_data[2] << 8) | key_data[3];
    *out_hop = ((uint32_t)key_data[4] << 24) | ((uint32_t)key_data[5] << 16) |
               ((uint32_t)key_data[6] << 8) | key_data[7];
    return true;
}

static bool kl_bf2_is_keeloq(SubGhz* subghz) {
    FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
    flipper_format_rewind(fff);
    FuriString* proto = furi_string_alloc();
    bool ok = flipper_format_read_string(fff, "Protocol", proto) &&
              furi_string_equal_str(proto, "KeeLoq");
    furi_string_free(proto);
    return ok;
}

static void kl_bf2_submenu_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

static bool kl_bf2_load_signal(SubGhz* subghz, FuriString* out_path) {
    DialogsFileBrowserOptions browser_options;
    dialog_file_browser_set_basic_options(
        &browser_options, SUBGHZ_APP_FILENAME_EXTENSION, &I_sub1_10px);
    browser_options.base_path = SUBGHZ_APP_FOLDER;

    FuriString* selected = furi_string_alloc();
    furi_string_set(selected, SUBGHZ_APP_FOLDER);

    bool res = dialog_file_browser_show(subghz->dialogs, selected, selected, &browser_options);

    if(res) {
        res = subghz_key_load(subghz, furi_string_get_cstr(selected), true);
        if(res) {
            furi_string_set(out_path, selected);
        }
    }

    furi_string_free(selected);
    return res;
}

static void kl_bf2_rebuild_menu(SubGhz* subghz) {
    submenu_reset(subghz->submenu);

    char label1[64];
    char label2[64];

    if(subghz->keeloq_bf2.sig1_loaded) {
        FuriString* name = furi_string_alloc();
        path_extract_filename(subghz->keeloq_bf2.sig1_path, name, true);
        snprintf(label1, sizeof(label1), "Sig 1: %s", furi_string_get_cstr(name));
        furi_string_free(name);
    } else {
        snprintf(label1, sizeof(label1), "Load Signal 1");
    }

    if(subghz->keeloq_bf2.sig2_loaded) {
        FuriString* name = furi_string_alloc();
        path_extract_filename(subghz->keeloq_bf2.sig2_path, name, true);
        snprintf(label2, sizeof(label2), "Sig 2: %s", furi_string_get_cstr(name));
        furi_string_free(name);
    } else {
        snprintf(label2, sizeof(label2), "Load Signal 2");
    }

    submenu_add_item(
        subghz->submenu, label1, KlBf2IndexLoadSig1,
        kl_bf2_submenu_callback, subghz);
    submenu_add_item(
        subghz->submenu, label2, KlBf2IndexLoadSig2,
        kl_bf2_submenu_callback, subghz);

    if(subghz->keeloq_bf2.sig1_loaded && subghz->keeloq_bf2.sig2_loaded) {
        submenu_add_item(
            subghz->submenu, "Start BF", KlBf2IndexStartBf,
            kl_bf2_submenu_callback, subghz);
    }

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

void subghz_scene_keeloq_bf2_on_enter(void* context) {
    SubGhz* subghz = context;

    subghz->keeloq_bf2.sig1_loaded = false;
    subghz->keeloq_bf2.sig2_loaded = false;

    kl_bf2_rebuild_menu(subghz);
}

bool subghz_scene_keeloq_bf2_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == KlBf2IndexLoadSig1) {
            FuriString* path = furi_string_alloc();
            if(kl_bf2_load_signal(subghz, path)) {
                if(!kl_bf2_is_keeloq(subghz)) {
                    dialog_message_show_storage_error(
                        subghz->dialogs, "Not a KeeLoq\nprotocol file");
                    furi_string_free(path);
                    kl_bf2_rebuild_menu(subghz);
                    return true;
                }

                uint32_t fix, hop;
                if(!kl_bf2_extract_key(subghz, &fix, &hop)) {
                    dialog_message_show_storage_error(
                        subghz->dialogs, "Cannot read Key\nfrom file");
                    furi_string_free(path);
                    kl_bf2_rebuild_menu(subghz);
                    return true;
                }

                subghz->keeloq_bf2.fix = fix;
                subghz->keeloq_bf2.hop1 = hop;
                subghz->keeloq_bf2.serial = fix & 0x0FFFFFFF;
                subghz->keeloq_bf2.sig1_loaded = true;
                furi_string_set(subghz->keeloq_bf2.sig1_path, path);

                subghz->keeloq_bf2.sig2_loaded = false;
            }
            furi_string_free(path);
            kl_bf2_rebuild_menu(subghz);
            return true;

        } else if(event.event == KlBf2IndexLoadSig2) {
            if(!subghz->keeloq_bf2.sig1_loaded) {
                dialog_message_show_storage_error(
                    subghz->dialogs, "Load Signal 1 first");
                kl_bf2_rebuild_menu(subghz);
                return true;
            }

            FuriString* path = furi_string_alloc();
            if(kl_bf2_load_signal(subghz, path)) {
                if(!kl_bf2_is_keeloq(subghz)) {
                    dialog_message_show_storage_error(
                        subghz->dialogs, "Not a KeeLoq\nprotocol file");
                    furi_string_free(path);
                    kl_bf2_rebuild_menu(subghz);
                    return true;
                }

                uint32_t fix2, hop2;
                if(!kl_bf2_extract_key(subghz, &fix2, &hop2)) {
                    dialog_message_show_storage_error(
                        subghz->dialogs, "Cannot read Key\nfrom file");
                    furi_string_free(path);
                    kl_bf2_rebuild_menu(subghz);
                    return true;
                }

                uint32_t serial2 = fix2 & 0x0FFFFFFF;
                if(serial2 != subghz->keeloq_bf2.serial) {
                    dialog_message_show_storage_error(
                        subghz->dialogs, "Serial mismatch!\nMust be same remote");
                    furi_string_free(path);
                    kl_bf2_rebuild_menu(subghz);
                    return true;
                }

                if(hop2 == subghz->keeloq_bf2.hop1) {
                    dialog_message_show_storage_error(
                        subghz->dialogs, "Same hop code!\nUse a different\ncapture");
                    furi_string_free(path);
                    kl_bf2_rebuild_menu(subghz);
                    return true;
                }

                subghz->keeloq_bf2.hop2 = hop2;
                subghz->keeloq_bf2.sig2_loaded = true;
                furi_string_set(subghz->keeloq_bf2.sig2_path, path);
            }
            furi_string_free(path);
            kl_bf2_rebuild_menu(subghz);
            return true;

        } else if(event.event == KlBf2IndexStartBf) {
            if(!subghz->keeloq_bf2.sig1_loaded || !subghz->keeloq_bf2.sig2_loaded) {
                return true;
            }

            if(!subghz_key_load(
                   subghz,
                   furi_string_get_cstr(subghz->keeloq_bf2.sig1_path),
                   true)) {
                dialog_message_show_storage_error(
                    subghz->dialogs, "Cannot reload\nSignal 1");
                kl_bf2_rebuild_menu(subghz);
                return true;
            }

            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneKeeloqDecrypt);
            return true;
        }
    }
    return false;
}

void subghz_scene_keeloq_bf2_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
}

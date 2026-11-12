// 0xb000: settings profile load/save (cJSON), saved-string helper
//
// NOTE: there is deliberately no "load the global settings file" helper here. That job belongs to
// load_settings() in engine/protected/init.cpp, which also restores the profile key binds. This file
// originally carried a second copy (load_module_states_from_text) that no code ever called and whose
// module filter was the inverse of the live profiles.cpp load_module_states(); keeping it around only
// invited a future caller to load the wrong half of the modules. Do not reintroduce it.
#include "slinky/engine.h"
struct cJSON;
FN(cJSON *, cjson_parse, (const char *text));                     // cJSON_Parse
FN(cJSON *, cjson_get_object_item, (const cJSON *object, const char *name));  // cJSON_GetObjectItemCaseSensitive
FN(char *, cjson_get_string_value, (const cJSON *object));                     // cJSON_GetStringValue
FN(void, cjson_delete, (cJSON *object));                             // cJSON_Delete
FN(void, cjson_free_string, (void *p));                              // cJSON_free
struct Mod { char padding_00[0x18]; unsigned char flags; };
struct Mods { Mod **data; unsigned long long count; };
extern "C" const char str_profile_binds[], str_name[], str_bind[], str_slinky_in_profiles[], str_settings[];

FN(int, settings_path, (char **path));
FN(int, settings_path_append, (char **path, const char *suffix));
FN(int, settings_path_ensure, (char **path));
FN(void, free_path, (char **path));
FN(int, write_text_file, (char **path, const char *data, bool text));
FN(char *, serialize_settings, (Mods *mods, void *ctx));
extern "C" int save_settings(void *module_manager, void *profile_list) {
    char *path;
    int error_code = settings_path(&path);
    if (error_code) return error_code;
    error_code = settings_path_append(&path, str_slinky_in_profiles);
    if (!error_code) error_code = settings_path_ensure(&path);
    if (!error_code) error_code = settings_path_append(&path, str_settings);
    if (!error_code) {
        char *json = serialize_settings((Mods *)module_manager, profile_list);
        if (!json) {
            error_code = 0x1388;
        } else {
            error_code = write_text_file(&path, json, true);
            cjson_free_string(json);
        }
    }
    free_path(&path);
    return error_code;
}

extern "C" char *settings_buffer;
FN(void *, libc_malloc, (unsigned long long n));   // malloc
FN(void *, libc_memcpy, (void *dest, const void *src, unsigned long long n));   // memcpy
FN(unsigned long long, libc_strlen, (const char *str));
FN(void, libc_free, (void *ptr));
extern "C" void set_saved_string(const char *s) {
    if (settings_buffer) libc_free(settings_buffer);
    if (!s) {
        settings_buffer = 0;
        return;
    }
    unsigned long long n = libc_strlen(s);
    char *p = (char *)libc_malloc(n + 1);
    settings_buffer = p;
    if (!p) return;
    libc_memcpy(p, s, n);
    p[n] = 0;
}

FN(cJSON *, cjson_create_object, ());                                                  // cJSON_CreateObject
FN(cJSON *, cjson_add_array_to_object, (cJSON *object, const char *name));                        // cJSON_AddArrayToObject
FN(cJSON *, cjson_add_string_to_object, (cJSON *object, const char *name, const char *s));         // cJSON_AddStringToObject
FN(cJSON *, cjson_add_number_to_object, (cJSON *object, const char *name, double v));              // cJSON_AddNumberToObject
FN(int, cjson_add_item_to_array, (cJSON *arr, cJSON *item));                               // cJSON_AddItemToArray
FN(char *, cjson_print_unformatted, (const cJSON *object));                                     // cJSON_PrintUnformatted
FN(unsigned char, save_module_settings, (cJSON *root, Mod *module));
struct BindEnt { const char *name; char padding_08[0x30]; unsigned bind; int padding_3c; };
FN(BindEnt *, profile_array, (void *ctx, unsigned long long *n));
// serialize all savable modules plus the key binds
extern "C" char *serialize_settings(Mods *mods, void *ctx) {
    cJSON *root = cjson_create_object();
    char *out = 0;
    if (!root) goto done;
    for (unsigned long long i = 0; i < mods->count; i++) {
        Mod *module = mods->data[i];
        if (!(module->flags & 0x10)) continue;
        if (!save_module_settings(root, module)) goto done;
    }
    {
        cJSON *binds = cjson_add_array_to_object(root, str_profile_binds);
        if (!binds) { out = 0; goto done; }
        unsigned long long n;
        BindEnt *e = profile_array(ctx, &n);
        for (unsigned long long i = 0; i < n; i++, e++) {
            if (e->bind == 0) continue;
            cJSON *json = cjson_create_object();
            if (!json) goto done;
            if (!cjson_add_string_to_object(json, str_name, e->name)) goto done;
            if (!cjson_add_number_to_object(json, str_bind, (double)e->bind)) goto done;
            if (!cjson_add_item_to_array(binds, json)) goto done;
        }
        out = cjson_print_unformatted(root);
    }
done:
    cjson_delete(root);
    return out;
}

FN(int, read_text_file, (char **path, char **data));
FN(unsigned char, load_module_states, (const char *data, Mods *mods));
FN(void, apply_module_state, (Mods *mods, Mod *module, int v));
FN(void, queue_module_toggle, (Mods *module_manager, Mod *module, unsigned char enabled));
extern "C" const char str_profiles[];
extern "C" unsigned char settings_dirty_flag;
struct Pair16 { long long a, b; };
extern "C" const Pair16 settings_saved_event;
struct ModX { char padding_00[0x18]; unsigned char flags; char padding_19[7]; unsigned char padding_20; char padding_21[0x1df]; void (*register_settings)(Pair16 *); };
// load profile `name` (NULL = reset every savable module to its defaults)
extern "C" int load_profile_name(const char *name, Mods *mods) {
    char *data;
    char *path;
    Pair16 p;
    if (!name) {
        for (unsigned long long i = 0; i < mods->count; i++) {
            ModX *module = (ModX *)mods->data[i];
            if (module->flags & 0x10) continue;
            apply_module_state(mods, (Mod *)module, 0);
            module->padding_20 = 0;
            if (!(module->flags & 2)) queue_module_toggle(mods, (Mod *)module, 0);
            if (module->register_settings) {
                p = settings_saved_event;
                module->register_settings(&p);
            }
        }
        if (settings_buffer) libc_free(settings_buffer);
        settings_buffer = 0;
        settings_dirty_flag = 0;
        return 0;
    }
    int error_code = settings_path(&path);
    if (error_code) return error_code;
    if ((error_code = settings_path_append(&path, str_slinky_in_profiles)) != 0) goto clean;
    if ((error_code = settings_path_ensure(&path)) != 0) goto clean;
    if ((error_code = settings_path_append(&path, str_profiles)) != 0) goto clean;
    if ((error_code = settings_path_ensure(&path)) != 0) goto clean;
    if ((error_code = settings_path_append(&path, name)) != 0) goto clean;
    if ((error_code = read_text_file(&path, &data)) != 0) goto clean;
    {
    unsigned char ok = load_module_states(data, mods);
    error_code = 0x1389;
    if (ok) {
        set_saved_string(name);
        settings_dirty_flag = 0;
        error_code = 0;
    }
    }
    libc_free(data);
clean:
    free_path(&path);
    return error_code;
}

// ---- run the settings visitor (descriptor +0x200) of every module in "remember" mode ----
// Used at start-up to copy the static setting values into each setting's saved slot, which is the
// configuration the Default profile restores and the fallback for fields missing from a profile.
struct ModList { struct ModCb { char padding_00[0x200]; void (*register_settings)(void *ev); } **data; unsigned long long count; };
struct Ev16 { long long a, b; };
extern "C" const Ev16 settings_pending_event;
extern "C" void save_module_states(ModList *l) {
    for (unsigned long long i = 0; i < l->count; i++) {
        auto register_settings = l->data[i]->register_settings;
        if (register_settings) {
            Ev16 ev = settings_pending_event;
            register_settings(&ev);
        }
    }
}

/*
OBS_Random_Media_Source
Copyright (C) 2026 ClockOrange

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs-properties.h>
#include <obs-websocket-api.h>  // vendor request API
#include <util/platform.h>
#include <graphics/vec2.h>
#include <atomic>
#include <mutex>
#include <random>
#include <vector>
#include <string>
#include <algorithm>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("random_media_source", "en-US")

// -------------------------------------------------------
// Global vendor handle (registered in obs_module_post_load)
// -------------------------------------------------------
static obs_websocket_vendor_t *g_vendor = nullptr;

// -------------------------------------------------------
// Plugin settings struct
// -------------------------------------------------------
struct random_media_data {
    obs_source_t *source = nullptr;

    std::string folder;
    bool  do_random_transform = false;
    bool  hide_on_end         = false;

    float min_scale      = 50.0f;
    float max_scale      = 150.0f;
    bool  preserve_aspect = true;

    float min_rot        = -180.0f;
    float max_rot        =  180.0f;
    bool  disable_rot    = false;

    int   min_x = 0, min_y = 0;
    int   max_x = 0, max_y = 0;

    // How many videos to spawn per trigger
    int   spawn_count     = 1;
    // Hard cap: won't spawn if this many are already playing
    int   max_active      = 5;

    std::vector<std::string> file_list;

    // Track active scene items so we can enforce max_active
    std::mutex              active_mutex;
    std::vector<obs_sceneitem_t *> active_items;
};

// -------------------------------------------------------
// Global pointer so the vendor callback can reach the data.
// In a multi-source world you'd want a list; for this use-case
// (one trigger source per stream) a single global is fine.
// -------------------------------------------------------
static random_media_data *g_data = nullptr;

// -------------------------------------------------------
// Supported media extensions
// -------------------------------------------------------
static const std::vector<std::string> MEDIA_EXTS = {
    ".mp4", ".mkv", ".avi", ".mov", ".webm", ".flv",
    ".jpg", ".jpeg", ".png", ".gif"
};

static bool has_media_ext(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return std::find(MEDIA_EXTS.begin(), MEDIA_EXTS.end(), ext) != MEDIA_EXTS.end();
}

// -------------------------------------------------------
// Rebuild file list from folder
// -------------------------------------------------------
static void update_file_list(random_media_data *data) {
    data->file_list.clear();
    if (data->folder.empty()) return;

    os_dir_t *dir = os_opendir(data->folder.c_str());
    if (!dir) {
        blog(LOG_WARNING, "[RandomMedia] Cannot open folder: %s", data->folder.c_str());
        return;
    }
    struct os_dirent *ent;
    while ((ent = os_readdir(dir))) {
        if (ent->directory) continue;
        std::string f = data->folder + "/" + ent->d_name;
        if (has_media_ext(f))
            data->file_list.push_back(f);
    }
    os_closedir(dir);
    blog(LOG_INFO, "[RandomMedia] %zu files found in '%s'",
         data->file_list.size(), data->folder.c_str());
}

// -------------------------------------------------------
// hide_ctx — used to remove a scene item when media ends
// -------------------------------------------------------
struct hide_ctx {
    random_media_data *data;
    obs_sceneitem_t   *item;
    obs_source_t      *media_source;
};

static void on_media_ended(void *param, calldata_t * /*cd*/) {
    hide_ctx *ctx = static_cast<hide_ctx *>(param);

    // Remove from active list
    {
        std::lock_guard<std::mutex> lock(ctx->data->active_mutex);
        auto &v = ctx->data->active_items;
        v.erase(std::remove(v.begin(), v.end(), ctx->item), v.end());
    }

    obs_sceneitem_remove(ctx->item);      // removes + releases item
    obs_source_release(ctx->media_source); // release our extra ref
    delete ctx;

    blog(LOG_INFO, "[RandomMedia] Media ended — item removed");
}

// -------------------------------------------------------
// Apply random transform to a scene item
// -------------------------------------------------------
static void apply_random_transform(random_media_data *data,
                                   obs_sceneitem_t   *item,
                                   std::mt19937      &gen)
{
    struct obs_video_info ovi = {};
    obs_get_video_info(&ovi);
    float cw = static_cast<float>(ovi.base_width);
    float ch = static_cast<float>(ovi.base_height);

    float x0 = (data->min_x > 0) ? static_cast<float>(data->min_x) : 0.0f;
    float y0 = (data->min_y > 0) ? static_cast<float>(data->min_y) : 0.0f;
    float x1 = (data->max_x > 0) ? static_cast<float>(data->max_x) : cw;
    float y1 = (data->max_y > 0) ? static_cast<float>(data->max_y) : ch;
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);

    float smin = data->min_scale / 100.0f;
    float smax = data->max_scale / 100.0f;
    if (smin > smax) std::swap(smin, smax);

    std::uniform_real_distribution<float> dx(x0, x1);
    std::uniform_real_distribution<float> dy(y0, y1);
    std::uniform_real_distribution<float> ds(smin, smax);

    vec2 pos = {dx(gen), dy(gen)};
    obs_sceneitem_set_pos(item, &pos);

    float sx = ds(gen);
    float sy = data->preserve_aspect ? sx : ds(gen);
    vec2 scale = {sx, sy};
    obs_sceneitem_set_scale(item, &scale);

    if (!data->disable_rot) {
        float rmin = data->min_rot, rmax = data->max_rot;
        if (rmin > rmax) std::swap(rmin, rmax);
        std::uniform_real_distribution<float> dr(rmin, rmax);
        obs_sceneitem_set_rot(item, dr(gen));
    }
}

// -------------------------------------------------------
// Spawn one media item onto the scene
// -------------------------------------------------------
static std::atomic<int> s_uid{0};

static void spawn_one(random_media_data *data,
                      obs_scene_t       *scene,
                      const std::string &file,
                      std::mt19937      &gen)
{
    std::string name = "RandomMedia_" + std::to_string(++s_uid);

    obs_data_t *s = obs_data_create();
    obs_data_set_string(s, "local_file",         file.c_str());
    obs_data_set_bool  (s, "is_local_file",       true);
    obs_data_set_bool  (s, "restart_on_activate", true);
    obs_data_set_bool  (s, "close_when_inactive", true);

    obs_source_t *media = obs_source_create("ffmpeg_source", name.c_str(), s, nullptr);
    obs_data_release(s);

    if (!media) {
        blog(LOG_ERROR, "[RandomMedia] Failed to create source for: %s", file.c_str());
        return;
    }

    obs_sceneitem_t *item = obs_scene_add(scene, media);
    if (!item) {
        blog(LOG_ERROR, "[RandomMedia] obs_scene_add failed for: %s", file.c_str());
        obs_source_release(media);
        return;
    }

    // Track active item
    {
        std::lock_guard<std::mutex> lock(data->active_mutex);
        data->active_items.push_back(item);
    }

    if (data->do_random_transform)
        apply_random_transform(data, item, gen);

    if (data->hide_on_end) {
        obs_source_get_ref(media); // extra ref for the callback
        hide_ctx *ctx = new hide_ctx{data, item, media};
        signal_handler_connect(obs_source_get_signal_handler(media),
                               "media_ended", on_media_ended, ctx);
    }

    obs_source_release(media); // scene holds its own ref now

    blog(LOG_INFO, "[RandomMedia] Spawned: %s", file.c_str());
}

// -------------------------------------------------------
// Main spawn logic — called by both activate and vendor request
// -------------------------------------------------------
static void do_spawn(random_media_data *data) {
    if (data->file_list.empty()) {
        blog(LOG_WARNING, "[RandomMedia] No media files — skipping");
        return;
    }

    // Check active cap
    {
        std::lock_guard<std::mutex> lock(data->active_mutex);
        int active = static_cast<int>(data->active_items.size());
        if (active >= data->max_active) {
            blog(LOG_INFO, "[RandomMedia] Active cap reached (%d/%d) — skipping",
                 active, data->max_active);
            return;
        }
    }

    obs_source_t *scene_src = obs_frontend_get_current_scene();
    if (!scene_src) {
        blog(LOG_WARNING, "[RandomMedia] No current scene");
        return;
    }

    obs_scene_t *scene = obs_scene_from_source(scene_src);
    // obs_scene_from_source does NOT bump refcount — don't release scene.

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> pick(0, data->file_list.size() - 1);

    int count = std::max(1, data->spawn_count);
    for (int i = 0; i < count; ++i)
        spawn_one(data, scene, data->file_list[pick(gen)], gen);

    obs_source_release(scene_src);
}

// -------------------------------------------------------
// obs-websocket vendor request callback
// Called when Streamer.bot sends:
//   CallVendorRequest("random_media_source", "spawn", {})
//   CallVendorRequest("random_media_source", "reload_files", {})
// -------------------------------------------------------
static void vendor_spawn_cb(obs_data_t * /*req*/, obs_data_t *res, void * /*priv*/) {
    if (!g_data) {
        obs_data_set_string(res, "status", "error");
        obs_data_set_string(res, "message", "plugin not initialized");
        return;
    }
    do_spawn(g_data);
    obs_data_set_string(res, "status", "ok");

    int active;
    {
        std::lock_guard<std::mutex> lock(g_data->active_mutex);
        active = static_cast<int>(g_data->active_items.size());
    }
    obs_data_set_int(res, "active_count", active);
    blog(LOG_INFO, "[RandomMedia] Vendor 'spawn' called — active: %d", active);
}

static void vendor_reload_cb(obs_data_t * /*req*/, obs_data_t *res, void * /*priv*/) {
    if (!g_data) {
        obs_data_set_string(res, "status", "error");
        return;
    }
    update_file_list(g_data);
    obs_data_set_string(res, "status", "ok");
    obs_data_set_int   (res, "file_count", static_cast<int>(g_data->file_list.size()));
    blog(LOG_INFO, "[RandomMedia] Vendor 'reload_files' called — %zu files",
         g_data->file_list.size());
}

// -------------------------------------------------------
// OBS source callbacks
// -------------------------------------------------------
static const char *source_get_name(void *) {
    return "Random Media Source";
}

static void source_update(void *d, obs_data_t *settings);

static void *source_create(obs_data_t *settings, obs_source_t *source) {
    random_media_data *data = new random_media_data();
    data->source = source;
    g_data = data; // register as global target for vendor requests
    source_update(data, settings);
    return data;
}

static void source_destroy(void *d) {
    random_media_data *data = static_cast<random_media_data *>(d);
    if (g_data == data) g_data = nullptr;
    delete data;
}

static void source_update(void *d, obs_data_t *settings) {
    random_media_data *data = static_cast<random_media_data *>(d);

    std::string new_folder = obs_data_get_string(settings, "folder");
    bool folder_changed = (new_folder != data->folder);

    data->folder            = new_folder;
    data->do_random_transform = obs_data_get_bool(settings, "random_transform");
    data->hide_on_end       = obs_data_get_bool(settings, "hide_on_end");
    data->min_scale         = static_cast<float>(obs_data_get_double(settings, "min_scale"));
    data->max_scale         = static_cast<float>(obs_data_get_double(settings, "max_scale"));
    data->preserve_aspect   = obs_data_get_bool(settings, "preserve_aspect");
    data->min_rot           = static_cast<float>(obs_data_get_double(settings, "min_rot"));
    data->max_rot           = static_cast<float>(obs_data_get_double(settings, "max_rot"));
    data->disable_rot       = obs_data_get_bool(settings, "disable_rot");
    data->min_x             = static_cast<int>(obs_data_get_int(settings, "min_x"));
    data->min_y             = static_cast<int>(obs_data_get_int(settings, "min_y"));
    data->max_x             = static_cast<int>(obs_data_get_int(settings, "max_x"));
    data->max_y             = static_cast<int>(obs_data_get_int(settings, "max_y"));
    data->spawn_count       = static_cast<int>(obs_data_get_int(settings, "spawn_count"));
    data->max_active        = static_cast<int>(obs_data_get_int(settings, "max_active"));

    if (folder_changed)
        update_file_list(data);
}

static obs_properties_t *source_properties(void *) {
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_path(props, "folder", "Media Folder",
                            OBS_PATH_DIRECTORY, nullptr, nullptr);

    obs_properties_add_group(props, "grp_transform",
                             "Random Transform", OBS_GROUP_NORMAL,
                             [](obs_properties_t *gp, void*) -> obs_properties_t* {
                                 // inner properties added below
                                 (void)gp;
                                 return nullptr;
                             }(nullptr, nullptr));

    // Transform
    obs_properties_add_bool  (props, "random_transform", "Enable Random Transform");
    obs_properties_add_float (props, "min_scale",   "Min Scale (%)",     10.0, 1000.0, 1.0);
    obs_properties_add_float (props, "max_scale",   "Max Scale (%)",     10.0, 1000.0, 1.0);
    obs_properties_add_bool  (props, "preserve_aspect", "Preserve Aspect Ratio");
    obs_properties_add_float (props, "min_rot",     "Min Rotation (°)", -360.0, 360.0, 1.0);
    obs_properties_add_float (props, "max_rot",     "Max Rotation (°)", -360.0, 360.0, 1.0);
    obs_properties_add_bool  (props, "disable_rot", "Disable Rotation");
    obs_properties_add_int   (props, "min_x",  "Min X (px)",  0, 7680, 1);
    obs_properties_add_int   (props, "min_y",  "Min Y (px)",  0, 4320, 1);
    obs_properties_add_int   (props, "max_x",  "Max X (px)",  0, 7680, 1);
    obs_properties_add_int   (props, "max_y",  "Max Y (px)",  0, 4320, 1);

    // Playback / limits
    obs_properties_add_bool  (props, "hide_on_end",   "Remove Item After Playback Ends");
    obs_properties_add_int   (props, "spawn_count",   "Videos per Trigger",      1, 10,  1);
    obs_properties_add_int   (props, "max_active",    "Max Simultaneous Videos", 1, 20,  1);

    return props;
}

static void source_defaults(obs_data_t *settings) {
    obs_data_set_default_double(settings, "min_scale",  50.0);
    obs_data_set_default_double(settings, "max_scale", 150.0);
    obs_data_set_default_bool  (settings, "preserve_aspect", true);
    obs_data_set_default_double(settings, "min_rot",  -180.0);
    obs_data_set_default_double(settings, "max_rot",   180.0);
    obs_data_set_default_bool  (settings, "hide_on_end", true);
    obs_data_set_default_int   (settings, "spawn_count", 1);
    obs_data_set_default_int   (settings, "max_active",  5);
}

// activate is kept as a fallback / manual test trigger
static void source_activate(void *d) {
    blog(LOG_INFO, "[RandomMedia] Activated (manual/fallback trigger)");
    do_spawn(static_cast<random_media_data *>(d));
}

static uint32_t source_get_width (void *) { return 1; }
static uint32_t source_get_height(void *) { return 1; }

// -------------------------------------------------------
// Source info struct
// -------------------------------------------------------
static struct obs_source_info random_media_info = {};

// -------------------------------------------------------
// Module load / post_load
// -------------------------------------------------------
bool obs_module_load(void) {
    random_media_info.id             = "random_media_source";
    random_media_info.type           = OBS_SOURCE_TYPE_INPUT;
    random_media_info.output_flags   = OBS_SOURCE_DO_NOT_DUPLICATE;
    random_media_info.get_name       = source_get_name;
    random_media_info.create         = source_create;
    random_media_info.destroy        = source_destroy;
    random_media_info.get_width      = source_get_width;
    random_media_info.get_height     = source_get_height;
    random_media_info.get_properties = source_properties;
    random_media_info.get_defaults   = source_defaults;
    random_media_info.update         = source_update;
    random_media_info.activate       = source_activate;

    obs_register_source(&random_media_info);
    blog(LOG_INFO, "[RandomMedia] Plugin loaded");
    return true;
}

// obs_module_post_load is called AFTER all plugins including obs-websocket are loaded.
// This is the correct place to register vendor requests.
void obs_module_post_load(void) {
    g_vendor = obs_websocket_register_vendor("random_media_source");
    if (!g_vendor) {
        blog(LOG_WARNING, "[RandomMedia] obs-websocket not available — vendor API disabled");
        return;
    }

    if (!obs_websocket_vendor_register_request(g_vendor, "spawn",
                                               vendor_spawn_cb, nullptr))
        blog(LOG_ERROR, "[RandomMedia] Failed to register 'spawn' request");

    if (!obs_websocket_vendor_register_request(g_vendor, "reload_files",
                                               vendor_reload_cb, nullptr))
        blog(LOG_ERROR, "[RandomMedia] Failed to register 'reload_files' request");

    blog(LOG_INFO, "[RandomMedia] obs-websocket vendor registered: 'random_media_source'");
    blog(LOG_INFO, "[RandomMedia] Available requests: 'spawn', 'reload_files'");
}

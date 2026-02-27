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
#include <util/platform.h>
#include <util/dstr.h>
#include <graphics/vec2.h>
#include <atomic>
#include <random>
#include <vector>
#include <string>
#include <algorithm>
#include <ctime>

OBS_DECLARE_MODULE()

// -------------------------------------------------------
// Data struct
// -------------------------------------------------------
struct random_media_data {
    obs_source_t *source = nullptr;
    std::string   folder;

    bool  do_random_transform = false;
    bool  hide_on_end         = false;

    float min_scale    = 50.0f;
    float max_scale    = 200.0f;
    float min_rot      = -180.0f;
    float max_rot      =  180.0f;
    bool  disable_rot  = false;
    bool  preserve_aspect = true;

    int   min_x = 0, min_y = 0;
    int   max_x = 0, max_y = 0;

    bool  allow_multiple = true;
    int   spawn_count    = 1;   // how many to spawn when allow_multiple == true

    std::vector<std::string> file_list;
};

// -------------------------------------------------------
// Supported extensions
// -------------------------------------------------------
static const std::vector<std::string> media_extensions = {
    ".mp4", ".mkv", ".avi", ".mov", ".jpg", ".jpeg", ".png", ".gif", ".webm", ".flv"
};

static bool has_media_extension(const std::string &filename) {
    size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = filename.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return std::find(media_extensions.begin(), media_extensions.end(), ext)
           != media_extensions.end();
}

// -------------------------------------------------------
// Build file list from folder
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
        std::string file = data->folder + "/" + ent->d_name;
        if (has_media_extension(file))
            data->file_list.push_back(file);
    }
    os_closedir(dir);

    blog(LOG_INFO, "[RandomMedia] Found %zu media files in '%s'",
         data->file_list.size(), data->folder.c_str());
}

// -------------------------------------------------------
// Callback: hide scene item when media playback ends
// NOTE: OBS signal "media_ended" passes the source, not the item.
//       We store the item pointer and remove it here.
//       obs_sceneitem_remove already releases the item internally,
//       so we must NOT call obs_sceneitem_release afterwards.
// -------------------------------------------------------
struct hide_ctx {
    obs_sceneitem_t *item;
    obs_source_t    *media_source;
};

static void on_media_ended(void *param, calldata_t * /*cd*/) {
    hide_ctx *ctx = static_cast<hide_ctx *>(param);
    if (ctx->item)
        obs_sceneitem_remove(ctx->item);   // removes & releases the item
    obs_source_release(ctx->media_source); // release the extra ref we kept
    delete ctx;
}

// -------------------------------------------------------
// Apply random transform to a scene item
// -------------------------------------------------------
static void apply_random_transform(random_media_data *data,
                                   obs_sceneitem_t   *item,
                                   std::mt19937      &gen)
{
    struct obs_video_info ovi;
    obs_get_video_info(&ovi);
    float cw = static_cast<float>(ovi.base_width);
    float ch = static_cast<float>(ovi.base_height);

    float pos_min_x = (data->min_x > 0) ? static_cast<float>(data->min_x) : 0.0f;
    float pos_min_y = (data->min_y > 0) ? static_cast<float>(data->min_y) : 0.0f;
    float pos_max_x = (data->max_x > 0) ? static_cast<float>(data->max_x) : cw;
    float pos_max_y = (data->max_y > 0) ? static_cast<float>(data->max_y) : ch;

    // Guard against inverted ranges
    if (pos_min_x > pos_max_x) std::swap(pos_min_x, pos_max_x);
    if (pos_min_y > pos_max_y) std::swap(pos_min_y, pos_max_y);
    float smin = data->min_scale / 100.0f;
    float smax = data->max_scale / 100.0f;
    if (smin > smax) std::swap(smin, smax);

    std::uniform_real_distribution<float> dist_x(pos_min_x, pos_max_x);
    std::uniform_real_distribution<float> dist_y(pos_min_y, pos_max_y);
    std::uniform_real_distribution<float> dist_s(smin, smax);

    vec2 pos = {dist_x(gen), dist_y(gen)};
    obs_sceneitem_set_pos(item, &pos);

    float sx = dist_s(gen);
    float sy = data->preserve_aspect ? sx : dist_s(gen);
    vec2 scale = {sx, sy};
    obs_sceneitem_set_scale(item, &scale);

    if (!data->disable_rot) {
        float rmin = data->min_rot, rmax = data->max_rot;
        if (rmin > rmax) std::swap(rmin, rmax);
        std::uniform_real_distribution<float> dist_r(rmin, rmax);
        obs_sceneitem_set_rot(item, dist_r(gen));
    }
}

// -------------------------------------------------------
// Spawn one random media item onto the current scene
// Returns the created obs_source_t* (caller must release)
// -------------------------------------------------------
static obs_sceneitem_t *spawn_one(random_media_data *data,
                                  obs_scene_t       *scene,
                                  const std::string &file,
                                  std::mt19937      &gen)
{
    // Use a unique name so multiple items don't collide
    static std::atomic<int> uid{0};
    std::string name = std::string("RandomMedia_") + std::to_string(++uid);

    obs_data_t *s = obs_data_create();
    obs_data_set_string(s, "local_file",          file.c_str());
    obs_data_set_bool  (s, "is_local_file",        true);
    obs_data_set_bool  (s, "restart_on_activate",  true);
    obs_data_set_bool  (s, "close_when_inactive",  true);

    obs_source_t *media = obs_source_create("ffmpeg_source", name.c_str(), s, nullptr);
    obs_data_release(s); // release AFTER create

    if (!media) {
        blog(LOG_ERROR, "[RandomMedia] Failed to create ffmpeg_source for: %s", file.c_str());
        return nullptr;
    }

    obs_sceneitem_t *item = obs_scene_add(scene, media);
    // obs_scene_add adds its own reference; we still own 'media' here.

    if (!item) {
        blog(LOG_ERROR, "[RandomMedia] obs_scene_add failed for: %s", file.c_str());
        obs_source_release(media);
        return nullptr;
    }

    if (data->do_random_transform)
        apply_random_transform(data, item, gen);

    if (data->hide_on_end) {
        // Keep an extra ref on the source so on_media_ended can release it
        obs_source_get_ref(media);
        hide_ctx *ctx = new hide_ctx{item, media};
        signal_handler_t *sh = obs_source_get_signal_handler(media);
        signal_handler_connect(sh, "media_ended", on_media_ended, ctx);
    }

    obs_source_release(media); // scene now holds the only ref we care about
    return item;
}

// -------------------------------------------------------
// Main spawn logic
// -------------------------------------------------------
static void spawn_random_media(random_media_data *data) {
    if (data->file_list.empty()) {
        blog(LOG_WARNING, "[RandomMedia] No media files found — skipping spawn");
        return;
    }

    obs_source_t *scene_src = obs_frontend_get_current_scene();
    if (!scene_src) {
        blog(LOG_WARNING, "[RandomMedia] No current scene");
        return;
    }

    obs_scene_t *scene = obs_scene_from_source(scene_src);
    // obs_scene_from_source does NOT add a ref — do not release 'scene' separately.

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dis(0, data->file_list.size() - 1);

    int count = data->allow_multiple ? std::max(1, data->spawn_count) : 1;

    for (int i = 0; i < count; ++i) {
        std::string file = data->file_list[dis(gen)];
        blog(LOG_INFO, "[RandomMedia] Spawning (%d/%d): %s", i + 1, count, file.c_str());
        spawn_one(data, scene, file, gen);
    }

    // Only release the scene *source* reference returned by obs_frontend_get_current_scene
    obs_source_release(scene_src);
}

// -------------------------------------------------------
// OBS source callbacks
// -------------------------------------------------------
static const char *get_name(void *) {
    return "Random Media Source";
}

static void source_update(void *d, obs_data_t *settings);

static void *source_create(obs_data_t *settings, obs_source_t *source) {
    random_media_data *data = new random_media_data();
    data->source = source;
    source_update(data, settings);
    return data;
}

static void source_destroy(void *d) {
    delete static_cast<random_media_data *>(d);
}

static void source_update(void *d, obs_data_t *settings) {
    random_media_data *data = static_cast<random_media_data *>(d);

    data->folder           = obs_data_get_string(settings, "folder");
    data->do_random_transform = obs_data_get_bool(settings, "random_transform");
    data->hide_on_end      = obs_data_get_bool(settings, "hide_on_end");
    data->min_scale        = static_cast<float>(obs_data_get_double(settings, "min_scale"));
    data->max_scale        = static_cast<float>(obs_data_get_double(settings, "max_scale"));
    data->min_rot          = static_cast<float>(obs_data_get_double(settings, "min_rot"));
    data->max_rot          = static_cast<float>(obs_data_get_double(settings, "max_rot"));
    data->disable_rot      = obs_data_get_bool(settings, "disable_rot");
    data->preserve_aspect  = obs_data_get_bool(settings, "preserve_aspect");
    data->min_x            = static_cast<int>(obs_data_get_int(settings, "min_x"));
    data->min_y            = static_cast<int>(obs_data_get_int(settings, "min_y"));
    data->max_x            = static_cast<int>(obs_data_get_int(settings, "max_x"));
    data->max_y            = static_cast<int>(obs_data_get_int(settings, "max_y"));
    data->allow_multiple   = obs_data_get_bool(settings, "allow_multiple");
    data->spawn_count      = static_cast<int>(obs_data_get_int(settings, "spawn_count"));

    update_file_list(data);
}

static obs_properties_t *source_properties(void *) {
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_path(props, "folder", "Media Folder",
                            OBS_PATH_DIRECTORY, nullptr, nullptr);

    // --- Transform ---
    obs_properties_add_bool(props, "random_transform", "Apply Random Transform on Show");
    obs_properties_add_float(props, "min_scale",  "Min Scale (%)",      10.0, 1000.0, 1.0);
    obs_properties_add_float(props, "max_scale",  "Max Scale (%)",      10.0, 1000.0, 1.0);
    obs_properties_add_bool(props, "preserve_aspect", "Preserve Aspect Ratio");
    obs_properties_add_float(props, "min_rot", "Min Rotation (°)", -360.0, 360.0, 1.0);
    obs_properties_add_float(props, "max_rot", "Max Rotation (°)", -360.0, 360.0, 1.0);
    obs_properties_add_bool(props, "disable_rot", "Disable Rotation");
    obs_properties_add_int(props, "min_x", "Min X (px)", 0, 7680, 1);
    obs_properties_add_int(props, "min_y", "Min Y (px)", 0, 4320, 1);
    obs_properties_add_int(props, "max_x", "Max X (px)", 0, 7680, 1);
    obs_properties_add_int(props, "max_y", "Max Y (px)", 0, 4320, 1);

    // --- Playback ---
    obs_properties_add_bool(props, "hide_on_end", "Remove after Playback Ends");
    obs_properties_add_bool(props, "allow_multiple", "Allow Multiple Simultaneous Videos");
    obs_properties_add_int(props, "spawn_count", "How Many to Spawn", 1, 20, 1);

    return props;
}

static void source_defaults(obs_data_t *settings) {
    obs_data_set_default_double(settings, "min_scale",  50.0);
    obs_data_set_default_double(settings, "max_scale", 150.0);
    obs_data_set_default_double(settings, "min_rot",  -180.0);
    obs_data_set_default_double(settings, "max_rot",   180.0);
    obs_data_set_default_bool  (settings, "preserve_aspect", true);
    obs_data_set_default_bool  (settings, "allow_multiple",  false);
    obs_data_set_default_int   (settings, "spawn_count", 1);
}

static void source_activate(void *d) {
    random_media_data *data = static_cast<random_media_data *>(d);
    blog(LOG_INFO, "[RandomMedia] Activated — spawning media");
    spawn_random_media(data);
}

// This source is purely a "trigger" — it has no video output of its own.
// Width/height = 1 so OBS shows it in the source list without errors.
static uint32_t source_get_width(void *)  { return 1; }
static uint32_t source_get_height(void *) { return 1; }

// -------------------------------------------------------
// Source info registration
// -------------------------------------------------------
static struct obs_source_info random_media_info = {};

bool obs_module_load(void) {
    random_media_info.id             = "random_media_source";
    random_media_info.type           = OBS_SOURCE_TYPE_INPUT;
    // No output flags — this source produces no video itself;
    // it just spawns child ffmpeg_source items onto the scene.
    random_media_info.output_flags   = OBS_SOURCE_DO_NOT_DUPLICATE;
    random_media_info.get_name       = get_name;
    random_media_info.create         = source_create;
    random_media_info.destroy        = source_destroy;
    random_media_info.get_width      = source_get_width;
    random_media_info.get_height     = source_get_height;
    random_media_info.get_properties = source_properties;
    random_media_info.get_defaults   = source_defaults;
    random_media_info.update         = source_update;
    random_media_info.activate       = source_activate;

    obs_register_source(&random_media_info);
    blog(LOG_INFO, "[RandomMedia] Plugin loaded successfully");
    return true;
}

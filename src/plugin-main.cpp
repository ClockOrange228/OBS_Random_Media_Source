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
#include <random>
#include <vector>
#include <string>
#include <algorithm>
#include <ctime>

// Структура данных источника
struct random_media_data {
    obs_source_t *source = nullptr;
    obs_source_t *internal = nullptr;
    std::string folder;
    bool do_random_transform = false;
    bool hide_on_end = false;
    float min_scale = 50.0f;
    float max_scale = 200.0f;
    float min_rot = -180.0f;
    float max_rot = 180.0f;
    bool disable_rot = false;
    bool preserve_aspect = true;
    int min_x = 0;
    int min_y = 0;
    int max_x = 0;
    int max_y = 0;
    bool allow_multiple = true;
    std::vector<std::string> file_list;
};

// Поддерживаемые расширения
static const std::vector<std::string> media_extensions = {
    ".mp4", ".mkv", ".avi", ".mov", ".jpg", ".png", ".gif"
};

bool has_media_extension(const std::string &filename) {
    size_t dot = filename.find_last_of(".");
    if (dot == std::string::npos) return false;
    std::string ext = filename.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return std::find(media_extensions.begin(), media_extensions.end(), ext) != media_extensions.end();
}

void update_file_list(random_media_data *data) {
    data->file_list.clear();
    os_dir_t *dir = os_opendir(data->folder.c_str());
    if (dir) {
        struct os_dirent *ent;
        while ((ent = os_readdir(dir))) {
            if (ent->directory) continue;
            std::string file = data->folder + "/" + ent->d_name;
            if (has_media_extension(file)) {
                data->file_list.push_back(file);
            }
        }
        os_closedir(dir);
    }
    blog(LOG_INFO, "Found %zu media files in '%s'", data->file_list.size(), data->folder.c_str());
}

static void on_media_ended(void *param, calldata_t *cd) {
    (void)cd;
    obs_sceneitem_t *item = static_cast<obs_sceneitem_t *>(param);
    obs_sceneitem_remove(item);
    obs_sceneitem_release(item);
    blog(LOG_INFO, "Media ended - removed");
}

void spawn_random_media(random_media_data *data) {
    if (data->file_list.empty()) {
        blog(LOG_WARNING, "No media files - skipping");
        return;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, static_cast<int>(data->file_list.size() - 1));
    std::string file = data->file_list[dis(gen)];

    blog(LOG_INFO, "Spawning: %s", file.c_str());

    obs_data_t *s = obs_data_create();
    obs_data_set_string(s, "local_file", file.c_str());
    obs_data_set_bool(s, "is_local_file", true);
    obs_data_set_bool(s, "restart_on_activate", true);
    obs_data_set_bool(s, "clear_on_media_end", data->hide_on_end);

    obs_source_t *media = obs_source_create("ffmpeg_source", "Random Media", s, nullptr);
    obs_data_release(s);

    if (media) {
        obs_source_update(media, nullptr);  // Принудительно обновляем
        obs_source_restart(media);  // Перезапускаем воспроизведение
        os_sleep_ms(500);  // Даём ffmpeg время на загрузку
    }

    obs_source_t *scene_src = obs_frontend_get_current_scene();
    if (!scene_src) {
        obs_source_release(media);
        return;
    }

    obs_scene_t *scene = obs_scene_from_source(scene_src);
    if (!scene) {
        obs_source_release(media);
        obs_source_release(scene_src);
        return;
    }

    obs_sceneitem_t *item = obs_scene_add(scene, media);
    obs_source_release(media);
    obs_scene_release(scene);
    obs_source_release(scene_src);

    if (data->hide_on_end && item) {
        signal_handler_t *signals = obs_source_get_signal_handler(media);
        signal_handler_connect(signals, "media_ended", on_media_ended, item);
    }

    if (data->do_random_transform && item) {
        struct obs_video_info ovi;
        obs_get_video_info(&ovi);
        float cw = static_cast<float>(ovi.base_width);
        float ch = static_cast<float>(ovi.base_height);

        float pos_min_x = data->min_x > 0 ? static_cast<float>(data->min_x) : 0.0f;
        float pos_min_y = data->min_y > 0 ? static_cast<float>(data->min_y) : 0.0f;
        float pos_max_x = data->max_x > 0 ? static_cast<float>(data->max_x) : cw;
        float pos_max_y = data->max_y > 0 ? static_cast<float>(data->max_y) : ch;

        std::uniform_real_distribution<float> pos_x(pos_min_x, pos_max_x);
        std::uniform_real_distribution<float> pos_y(pos_min_y, pos_max_y);
        vec2 pos = {pos_x(gen), pos_y(gen)};
        obs_sceneitem_set_pos(item, &pos);

        std::uniform_real_distribution<float> scale_dist(data->min_scale / 100.0f, data->max_scale / 100.0f);
        float sx = scale_dist(gen);
        float sy = data->preserve_aspect ? sx : scale_dist(gen);
        vec2 scale = {sx, sy};
        obs_sceneitem_set_scale(item, &scale);

        if (!data->disable_rot) {
            std::uniform_real_distribution<float> rot_dist(data->min_rot, data->max_rot);
            obs_sceneitem_set_rot(item, rot_dist(gen));
        }
    }
}

const char *get_name(void *) {
    return "Random Media Source";
}

void update(void *d, obs_data_t *settings);

void *create(obs_data_t *settings, obs_source_t *source) {
    random_media_data *data = new random_media_data();
    data->source = source;
    update(data, settings);
    return data;
}

void destroy(void *d) {
    random_media_data *data = static_cast<random_media_data *>(d);
    delete data;
}

void update(void *d, obs_data_t *settings) {
    random_media_data *data = static_cast<random_media_data *>(d);
    data->folder = obs_data_get_string(settings, "folder");
    data->do_random_transform = obs_data_get_bool(settings, "random_transform");
    data->hide_on_end = obs_data_get_bool(settings, "hide_on_end");
    data->min_scale = static_cast<float>(obs_data_get_double(settings, "min_scale"));
    data->max_scale = static_cast<float>(obs_data_get_double(settings, "max_scale"));
    data->min_rot = static_cast<float>(obs_data_get_double(settings, "min_rot"));
    data->max_rot = static_cast<float>(obs_data_get_double(settings, "max_rot"));
    data->disable_rot = obs_data_get_bool(settings, "disable_rot");
    data->preserve_aspect = obs_data_get_bool(settings, "preserve_aspect");
    data->min_x = static_cast<int>(obs_data_get_int(settings, "min_x"));
    data->min_y = static_cast<int>(obs_data_get_int(settings, "min_y"));
    data->max_x = static_cast<int>(obs_data_get_int(settings, "max_x"));
    data->max_y = static_cast<int>(obs_data_get_int(settings, "max_y"));
    data->allow_multiple = obs_data_get_bool(settings, "allow_multiple");

    update_file_list(data);
}

obs_properties_t *properties(void *) {
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_path(props, "folder", "Folder", OBS_PATH_DIRECTORY, nullptr, nullptr);
    obs_properties_add_bool(props, "random_transform", "Apply Random Transform on Show");
    obs_properties_add_float(props, "min_scale", "Min Scale (%)", 10.0, 500.0, 1.0);
    obs_properties_add_float(props, "max_scale", "Max Scale (%)", 10.0, 500.0, 1.0);
    obs_properties_add_bool(props, "preserve_aspect", "Preserve Aspect Ratio");
    obs_properties_add_float(props, "min_rot", "Min Rotation (°)", -360.0, 360.0, 1.0);
    obs_properties_add_float(props, "max_rot", "Max Rotation (°)", -360.0, 360.0, 1.0);
    obs_properties_add_bool(props, "disable_rot", "Disable Rotation");
    obs_properties_add_int(props, "min_x", "Min X (px)", 0, 3840, 1);
    obs_properties_add_int(props, "min_y", "Min Y (px)", 0, 2160, 1);
    obs_properties_add_int(props, "max_x", "Max X (px)", 0, 3840, 1);
    obs_properties_add_int(props, "max_y", "Max Y (px)", 0, 2160, 1);
    obs_properties_add_bool(props, "hide_on_end", "Hide after Playback");
    obs_properties_add_bool(props, "allow_multiple", "Allow Multiple Videos");

    return props;
}

void activate(void *d) {
    random_media_data *data = static_cast<random_media_data *>(d);
    blog(LOG_INFO, "Activate called");

    spawn_random_media(data);
}

void video_render(void *d, gs_effect_t *effect) {
    (void)d; (void)effect;
}

uint32_t get_width(void *d) {
    (void)d;
    return 0;
}

uint32_t get_height(void *d) {
    (void)d;
    return 0;
}

static const struct obs_source_info random_media_info = {
    .id             = "random_media_source",
    .type           = OBS_SOURCE_TYPE_INPUT,
    .output_flags   = OBS_SOURCE_ASYNC_VIDEO,
    .get_name       = get_name,
    .create         = create,
    .destroy        = destroy,
    .get_width      = get_width,
    .get_height     = get_height,
    .get_properties = properties,
    .update         = update,
    .activate       = activate,
    .video_render   = video_render,
};

OBS_DECLARE_MODULE()

bool obs_module_load(void) {
    srand(static_cast<unsigned>(time(nullptr)));
    obs_register_source(&random_media_info);
    blog(LOG_INFO, "Random Media Source loaded");
    return true;
}

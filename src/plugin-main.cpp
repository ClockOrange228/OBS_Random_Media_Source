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
#include <util/platform.h>
#include <util/dstr.h>
#include <graphics/vec2.h>
#include <random>
#include <vector>
#include <string>
#include <algorithm>
#include <ctime>

// Структура данных для источника (менеджер)
struct random_media_data {
    obs_source_t *source;  // Сам источник
    std::string folder;
    bool do_random_transform;
    bool hide_on_end;
    float min_scale;
    float max_scale;
    float min_rot;
    float max_rot;
    bool disable_rot;
    bool preserve_aspect;
    int min_x;
    int min_y;
    int max_x;
    int max_y;
    bool allow_multiple;
    std::vector<std::string> file_list;  // Список файлов из папки
};

// Список поддерживаемых расширений
static const std::vector<std::string> media_extensions = {".mp4", ".mkv", ".avi", ".mov", ".jpg", ".png", ".gif"};

bool has_media_extension(const std::string &filename) {
    std::string ext = filename.substr(filename.find_last_of("."));
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
}

static void on_media_ended(void *param, calldata_t *cd) {
    obs_sceneitem_t *item = static_cast<obs_sceneitem_t *>(param);
    obs_sceneitem_remove(item);
    obs_sceneitem_release(item);
}

void spawn_random_media(random_media_data *data) {
    if (data->file_list.empty()) return;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, static_cast<int>(data->file_list.size() - 1));
    std::string random_file = data->file_list[dis(gen)];

    obs_data_t *internal_settings = obs_data_create();
    obs_data_set_string(internal_settings, "local_file", random_file.c_str());
    obs_data_set_bool(internal_settings, "is_local_file", true);
    obs_data_set_bool(internal_settings, "restart_on_activate", true);

    obs_source_t *internal = obs_source_create("ffmpeg_source", "Random Media Internal", internal_settings, nullptr);
    obs_data_release(internal_settings);

    // Получаем текущую сцену
    obs_source_t *scene_src = obs_frontend_get_current_scene();
    if (!scene_src) return;
    obs_scene_t *scene = obs_scene_from_source(scene_src);

    obs_sceneitem_t *item = obs_scene_add(scene, internal);
    obs_source_release(internal);
    obs_scene_release(scene);
    obs_source_release(scene_src);

    if (data->hide_on_end) {
        signal_handler_t *signals = obs_source_get_signal_handler(internal);
        signal_handler_connect_ref(signals, "media_ended", on_media_ended, item);
    }

    if (data->do_random_transform) {
        struct obs_video_info ovi;
        obs_get_video_info(&ovi);
        uint32_t cw = ovi.base_width;
        uint32_t ch = ovi.base_height;

        // Область позиционирования
        float pos_min_x = data->min_x > 0 ? data->min_x : 0;
        float pos_min_y = data->min_y > 0 ? data->min_y : 0;
        float pos_max_x = data->max_x > 0 ? data->max_x : cw;
        float pos_max_y = data->max_y > 0 ? data->max_y : ch;

        std::uniform_real_distribution<float> dist_pos_x(pos_min_x, pos_max_x);
        std::uniform_real_distribution<float> dist_pos_y(pos_min_y, pos_max_y);
        struct vec2 pos = {dist_pos_x(gen), dist_pos_y(gen)};
        obs_sceneitem_set_pos(item, &pos);

        // Масштаб в процентах (0.5 = 50%, 2.0 = 200%)
        float scale_min = data->min_scale / 100.0f;
        float scale_max = data->max_scale / 100.0f;
        std::uniform_real_distribution<float> dist_scale(scale_min, scale_max);
        float s_x = dist_scale(gen);
        float s_y = data->preserve_aspect ? s_x : dist_scale(gen);
        struct vec2 scale = {s_x, s_y};
        obs_sceneitem_set_scale(item, &scale);

        if (!data->disable_rot) {
            std::uniform_real_distribution<float> dist_rot(data->min_rot, data->max_rot);
            obs_sceneitem_set_rot(item, dist_rot(gen));
        }
    }
}

const char *get_name(void *) {
    return "Random Media Source";
}

void *create(obs_data_t *settings, obs_source_t *source) {
    random_media_data *data = new random_media_data();
    data->source = source;
    data->do_random_transform = false;
    data->hide_on_end = false;
    data->min_scale = 50.0f;
    data->max_scale = 200.0f;
    data->min_rot = -180.0f;
    data->max_rot = 180.0f;
    data->disable_rot = false;
    data->preserve_aspect = true;
    data->min_x = 0;
    data->min_y = 0;
    data->max_x = 0;
    data->max_y = 0;
    data->allow_multiple = true;
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
    data->min_x = obs_data_get_int(settings, "min_x");
    data->min_y = obs_data_get_int(settings, "min_y");
    data->max_x = obs_data_get_int(settings, "max_x");
    data->max_y = obs_data_get_int(settings, "max_y");
    data->allow_multiple = obs_data_get_bool(settings, "allow_multiple");
    update_file_list(data);
}

obs_properties_t *properties(void *) {
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_path(props, "folder", "Folder", OBS_PATH_DIRECTORY, nullptr, nullptr);
    obs_properties_add_bool(props, "random_transform", "Apply Random Transform on Show");
    obs_properties_add_bool(props, "hide_on_end", "Hide when playback ends");
    obs_properties_add_float(props, "min_scale", "Min Scale (%)", 10.0, 500.0, 1.0);
    obs_properties_add_float(props, "max_scale", "Max Scale (%)", 10.0, 500.0, 1.0);
    obs_properties_add_float(props, "min_rot", "Min Rotation (degrees)", -360.0, 360.0, 1.0);
    obs_properties_add_float(props, "max_rot", "Max Rotation (degrees)", -360.0, 360.0, 1.0);
    obs_properties_add_bool(props, "disable_rot", "Disable Rotation");
    obs_properties_add_bool(props, "preserve_aspect", "Preserve Aspect Ratio");
    obs_properties_add_int(props, "min_x", "Min X Position (px)", 0, 3840, 1);
    obs_properties_add_int(props, "min_y", "Min Y Position (px)", 0, 2160, 1);
    obs_properties_add_int(props, "max_x", "Max X Position (px)", 0, 3840, 1);
    obs_properties_add_int(props, "max_y", "Max Y Position (px)", 0, 2160, 1);
    obs_properties_add_bool(props, "allow_multiple", "Allow Multiple Instances Simultaneously");
    return props;
}

void activate(void *d) {
    random_media_data *data = static_cast<random_media_data *>(d);
    spawn_random_media(data);
}

void video_render(void *d, gs_effect_t *effect) {
    // Не нужно, т.к. спавним отдельные источники
}

uint32_t get_width(void *d) {
    return 0;  // Manager не имеет размера
}

uint32_t get_height(void *d) {
    return 0;
}

// Структура info
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
    srand(static_cast<unsigned int>(time(nullptr)));
    obs_register_source(&random_media_info);
    return true;
}

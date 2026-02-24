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

// Структура данных для источника
struct random_media_data {
    obs_source_t *source;  // Сам источник
    obs_source_t *internal;  // Внутренний ffmpeg_source
    std::string folder;
    bool do_random_transform;
    bool hide_on_end;
    signal_handler_t *media_signals;  // для хранения хэндлера сигналов
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
    random_media_data *data = static_cast<random_media_data *>(param);
    obs_source_set_hidden(data->source, true);
}

void pick_random_file(random_media_data *data) {
    if (data->file_list.empty()) return;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, static_cast<int>(data->file_list.size() - 1));  // ← фикс C4267
    std::string random_file = data->file_list[dis(gen)];

    obs_data_t *internal_settings = obs_data_create();
    obs_data_set_string(internal_settings, "local_file", random_file.c_str());
    obs_data_set_bool(internal_settings, "is_local_file", true);

    if (!data->internal) {
        data->internal = obs_source_create("ffmpeg_source", "Random Media Internal", internal_settings, nullptr);
    } else {
        obs_source_update(data->internal, internal_settings);
    }

    obs_data_release(internal_settings);

    if (data->hide_on_end && data->internal && !data->media_signals) {
        data->media_signals = obs_source_get_signal_handler(data->internal);
        signal_handler_connect(data->media_signals, "media_ended", on_media_ended, data);
    }
}

const char *get_name(void *) {
    return "Random Media Source";
}

// Обновление объявляем ПЕРЕД create!
void update(void *d, obs_data_t *settings);

void *create(obs_data_t *settings, obs_source_t *source) {
    random_media_data *data = new random_media_data();
    data->source = source;
    data->internal = nullptr;
    data->do_random_transform = false;
    data->hide_on_end = false;
    data->media_signals = nullptr;
    update(data, settings);
    return data;
}

void destroy(void *d) {
    random_media_data *data = static_cast<random_media_data *>(d);
    if (data->internal) {
        if (data->media_signals) {
            signal_handler_disconnect(data->media_signals, "media_ended", on_media_ended, data);
        }
        obs_source_release(data->internal);
    }
    delete data;
}

void update(void *d, obs_data_t *settings) {
    random_media_data *data = static_cast<random_media_data *>(d);
    data->folder = obs_data_get_string(settings, "folder");
    data->do_random_transform = obs_data_get_bool(settings, "random_transform");
    bool new_hide_on_end = obs_data_get_bool(settings, "hide_on_end");

    if (new_hide_on_end != data->hide_on_end) {
        data->hide_on_end = new_hide_on_end;

        if (data->internal) {
            if (data->hide_on_end) {
                if (!data->media_signals) {
                    data->media_signals = obs_source_get_signal_handler(data->internal);
                    signal_handler_connect(data->media_signals, "media_ended", on_media_ended, data);
                }
            } else {
                if (data->media_signals) {
                    signal_handler_disconnect(data->media_signals, "media_ended", on_media_ended, data);
                    data->media_signals = nullptr;
                }
            }
        }
    }

    update_file_list(data);
    pick_random_file(data);
}

obs_properties_t *properties(void *) {
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_path(props, "folder", obs_module_text("Folder"), OBS_PATH_DIRECTORY, nullptr, nullptr);
    obs_properties_add_bool(props, "random_transform", obs_module_text("Apply Random Transform on Show"));
    obs_properties_add_bool(props, "hide_on_end", obs_module_text("Hide when playback ends"));
    return props;
}

void activate(void *d) {
    random_media_data *data = static_cast<random_media_data *>(d);
    pick_random_file(data);

    if (!data->do_random_transform) return;

    obs_source_t *scene_src = obs_frontend_get_current_scene();
    if (!scene_src) return;
    obs_scene_t *scene = obs_scene_from_source(scene_src);
    if (!scene) {
        obs_source_release(scene_src);
        return;
    }

    obs_scene_enum_items(
        scene,
        [](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
            random_media_data *data = static_cast<random_media_data *>(param);
            if (obs_sceneitem_get_source(item) == data->source) {
                struct obs_video_info ovi;
                obs_get_video_info(&ovi);
                uint32_t cw = ovi.base_width;
                uint32_t ch = ovi.base_height;

                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_real_distribution<float> dist_pos_x(0.0f, static_cast<float>(cw));
                std::uniform_real_distribution<float> dist_pos_y(0.0f, static_cast<float>(ch));
                struct vec2 pos = {dist_pos_x(gen), dist_pos_y(gen)};
                obs_sceneitem_set_pos(item, &pos);

                std::uniform_real_distribution<float> dist_scale(0.5f, 2.0f);
                float s = dist_scale(gen);
                struct vec2 scale = {s, s};
                obs_sceneitem_set_scale(item, &scale);

                std::uniform_real_distribution<float> dist_rot(-180.0f, 180.0f);
                obs_sceneitem_set_rot(item, dist_rot(gen));
            }
            return true;
        },
        data);

    obs_scene_release(scene);
    obs_source_release(scene_src);
}

void video_render(void *d, gs_effect_t *effect) {
    random_media_data *data = static_cast<random_media_data *>(d);
    if (data->internal) obs_source_video_render(data->internal);
}

uint32_t get_width(void *d) {
    random_media_data *data = static_cast<random_media_data *>(d);
    return data->internal ? obs_source_get_width(data->internal) : 0;
}

uint32_t get_height(void *d) {
    random_media_data *data = static_cast<random_media_data *>(d);
    return data->internal ? obs_source_get_height(data->internal) : 0;
}

// Структура info — члены В СТРОГОМ ПОРЯДКЕ из obs-source.h!
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

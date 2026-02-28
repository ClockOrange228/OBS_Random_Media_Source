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
#include <callback/calldata.h>
#include <callback/proc.h>
#include <graphics/vec2.h>
#include <atomic>
#include <mutex>
#include <random>
#include <vector>
#include <string>
#include <algorithm>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("OBS_Random_Media_Source", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Spawns random media on scene with random transform. "
	       "Trigger via WebSocket vendor request 'spawn'.";
}

// ============================================================
//  Minimal inline vendor API (no obs-websocket-api.h needed)
// ============================================================
typedef void *ws_vendor_ptr;
typedef void (*ws_request_cb)(obs_data_t *req, obs_data_t *res,
			      void *priv);

struct ws_cb_holder {
	ws_request_cb callback;
	void *priv_data;
};

static proc_handler_t *get_ws_ph(void)
{
	proc_handler_t *gph = obs_get_proc_handler();
	if (!gph)
		return nullptr;
	calldata_t cd = {};
	bool ok =
		proc_handler_call(gph, "obs_websocket_api_get_ph", &cd);
	proc_handler_t *ret =
		ok ? (proc_handler_t *)calldata_ptr(&cd, "ph") : nullptr;
	calldata_free(&cd);
	return ret;
}

static ws_vendor_ptr vendor_register(const char *name)
{
	proc_handler_t *ws_ph = get_ws_ph();
	if (!ws_ph) {
		blog(LOG_INFO,
		     "[RandomMedia] obs-websocket not available"
		     " — vendor API disabled");
		return nullptr;
	}
	calldata_t cd = {};
	calldata_set_string(&cd, "vendor_name", name);
	proc_handler_call(ws_ph, "obs_websocket_create_vendor", &cd);
	ws_vendor_ptr vendor = calldata_ptr(&cd, "vendor");
	calldata_free(&cd);
	return vendor;
}

static bool vendor_add_request(ws_vendor_ptr vendor, const char *type,
				ws_request_cb cb, void *priv)
{
	if (!vendor)
		return false;
	proc_handler_t *ws_ph = get_ws_ph();
	if (!ws_ph)
		return false;
	auto *holder = new ws_cb_holder{cb, priv};
	calldata_t cd = {};
	calldata_set_ptr(&cd, "vendor", vendor);
	calldata_set_string(&cd, "request_type", type);
	calldata_set_ptr(&cd, "request_callback", holder);
	bool ok = proc_handler_call(
		ws_ph, "obs_websocket_vendor_register_request", &cd);
	calldata_free(&cd);
	return ok;
}

// ============================================================
//  Plugin data
// ============================================================
struct random_media_data {
	obs_source_t *source = nullptr;
	std::string folder;
	bool do_random_transform = true;
	bool hide_on_end = true;

	// Transform — relative to canvas (0.0 – 1.0)
	// Position randomised so item stays fully ON canvas
	float min_scale = 20.0f;  // % of canvas width
	float max_scale = 40.0f;
	bool preserve_aspect = true;
	float min_rot = -30.0f;
	float max_rot = 30.0f;
	bool disable_rot = false;

	// Volume
	float volume_db = 0.0f; // dB, applied to spawned source

	int spawn_count = 1;
	int max_active = 5;

	std::vector<std::string> file_list;
	std::mutex active_mutex;
	std::vector<obs_sceneitem_t *> active_items;
};

static random_media_data *g_data = nullptr;

// ============================================================
//  File list
// ============================================================
static const char *MEDIA_EXTS[] = {
	".mp4", ".mkv", ".avi", ".mov", ".webm",
	".flv", ".jpg", ".jpeg", ".png", ".gif", nullptr};

static bool has_media_ext(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot)
		return false;
	char ext[16] = {};
	size_t i = 0;
	for (const char *p = dot; *p && i < 15; ++p, ++i)
		ext[i] = (char)tolower((unsigned char)*p);
	for (int k = 0; MEDIA_EXTS[k]; ++k)
		if (strcmp(ext, MEDIA_EXTS[k]) == 0)
			return true;
	return false;
}

static void update_file_list(random_media_data *data)
{
	data->file_list.clear();
	if (data->folder.empty())
		return;
	os_dir_t *dir = os_opendir(data->folder.c_str());
	if (!dir) {
		blog(LOG_WARNING,
		     "[RandomMedia] Cannot open folder: %s",
		     data->folder.c_str());
		return;
	}
	struct os_dirent *ent;
	while ((ent = os_readdir(dir))) {
		if (ent->directory)
			continue;
		if (!has_media_ext(ent->d_name))
			continue;
		std::string f = data->folder + "/" + ent->d_name;
		data->file_list.push_back(f);
	}
	os_closedir(dir);
	blog(LOG_INFO, "[RandomMedia] Found %zu files in '%s'",
	     data->file_list.size(), data->folder.c_str());
}

// ============================================================
//  Media-ended callback
// ============================================================
struct hide_ctx {
	random_media_data *data;
	obs_sceneitem_t *item;
	obs_source_t *media_source;
};

static void on_media_ended(void *param, calldata_t * /*cd*/)
{
	hide_ctx *ctx = static_cast<hide_ctx *>(param);
	{
		std::lock_guard<std::mutex> lk(ctx->data->active_mutex);
		auto &v = ctx->data->active_items;
		v.erase(std::remove(v.begin(), v.end(), ctx->item),
			v.end());
	}
	obs_sceneitem_remove(ctx->item);
	obs_source_release(ctx->media_source);
	delete ctx;
	blog(LOG_INFO, "[RandomMedia] Media ended — item removed");
}

// ============================================================
//  Spawn one item
// ============================================================
static std::atomic<int> s_uid{0};

static void spawn_one(random_media_data *data, obs_scene_t *scene,
		      const std::string &file, std::mt19937 &gen)
{
	std::string name = "RMS_" + std::to_string(++s_uid);

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "local_file", file.c_str());
	obs_data_set_bool(s, "is_local_file", true);
	obs_data_set_bool(s, "restart_on_activate", true);
	obs_data_set_bool(s, "close_when_inactive", false);
	obs_data_set_bool(s, "clear_on_media_end", true);

	obs_source_t *media = obs_source_create("ffmpeg_source",
						name.c_str(), s,
						nullptr);
	obs_data_release(s);

	if (!media) {
		blog(LOG_ERROR,
		     "[RandomMedia] Failed to create source: %s",
		     file.c_str());
		return;
	}

	// Set volume: convert dB to linear (0 dB = 1.0)
	float vol_linear = obs_db_to_mul(data->volume_db);
	obs_source_set_volume(media, vol_linear);

	// Monitor + Output so streamer can hear it
	obs_source_set_monitoring_type(
		media, OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);

	obs_sceneitem_t *item = obs_scene_add(scene, media);
	if (!item) {
		blog(LOG_ERROR,
		     "[RandomMedia] obs_scene_add failed: %s",
		     file.c_str());
		obs_source_release(media);
		return;
	}
	obs_sceneitem_set_visible(item, true);

	// ---- Random transform ----
	if (data->do_random_transform) {
		struct obs_video_info ovi = {};
		obs_get_video_info(&ovi);
		float cw = (float)ovi.base_width;
		float ch = (float)ovi.base_height;

		float smin = data->min_scale / 100.0f;
		float smax = data->max_scale / 100.0f;
		if (smin > smax)
			std::swap(smin, smax);

		std::uniform_real_distribution<float> ds(smin, smax);
		float sx = ds(gen) * cw;

		// Source natural size
		float src_w =
			(float)obs_source_get_width(media);
		float src_h =
			(float)obs_source_get_height(media);
		if (src_w < 1.0f)
			src_w = cw * 0.3f;
		if (src_h < 1.0f)
			src_h = ch * 0.3f;

		float scale_x = sx / src_w;
		float scale_y = data->preserve_aspect
					? scale_x
					: (ds(gen) * ch / src_h);

		float item_w = src_w * scale_x;
		float item_h = src_h * scale_y;

		// Keep fully inside canvas
		float max_x = std::max(0.0f, cw - item_w);
		float max_y = std::max(0.0f, ch - item_h);

		std::uniform_real_distribution<float> dx(0.0f,
							 max_x);
		std::uniform_real_distribution<float> dy(0.0f,
							 max_y);

		vec2 pos = {dx(gen), dy(gen)};
		obs_sceneitem_set_pos(item, &pos);

		vec2 scale = {scale_x, scale_y};
		obs_sceneitem_set_scale(item, &scale);

		if (!data->disable_rot) {
			float rmin = data->min_rot;
			float rmax = data->max_rot;
			if (rmin > rmax)
				std::swap(rmin, rmax);
			std::uniform_real_distribution<float> dr(
				rmin, rmax);
			obs_sceneitem_set_rot(item, dr(gen));
		}
	}

	{
		std::lock_guard<std::mutex> lk(data->active_mutex);
		data->active_items.push_back(item);
	}

	if (data->hide_on_end) {
		obs_source_get_ref(media);
		auto *ctx = new hide_ctx{data, item, media};
		signal_handler_connect(
			obs_source_get_signal_handler(media),
			"media_ended", on_media_ended, ctx);
	}

	obs_source_release(media);
	blog(LOG_INFO, "[RandomMedia] Spawned '%s' -> %s",
	     name.c_str(), file.c_str());
}

// ============================================================
//  Main spawn
// ============================================================
static void do_spawn(random_media_data *data)
{
	if (data->file_list.empty()) {
		blog(LOG_WARNING,
		     "[RandomMedia] No files in '%s' — skipping",
		     data->folder.c_str());
		return;
	}
	{
		std::lock_guard<std::mutex> lk(data->active_mutex);
		int active =
			(int)data->active_items.size();
		if (active >= data->max_active) {
			blog(LOG_INFO,
			     "[RandomMedia] Cap %d/%d — skip",
			     active, data->max_active);
			return;
		}
	}

	obs_source_t *scene_src =
		obs_frontend_get_current_scene();
	if (!scene_src) {
		blog(LOG_WARNING,
		     "[RandomMedia] No current scene");
		return;
	}
	obs_scene_t *scene =
		obs_scene_from_source(scene_src);

	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<size_t> pick(
		0, data->file_list.size() - 1);

	int count = std::max(1, data->spawn_count);
	for (int i = 0; i < count; ++i)
		spawn_one(data, scene,
			  data->file_list[pick(gen)], gen);

	obs_source_release(scene_src);
}

// ============================================================
//  Vendor callbacks
// ============================================================
static void vendor_spawn_cb(obs_data_t * /*req*/,
			    obs_data_t *res, void * /*priv*/)
{
	if (!g_data) {
		obs_data_set_string(res, "status", "error");
		obs_data_set_string(res, "message",
				    "plugin not initialized");
		return;
	}
	do_spawn(g_data);
	obs_data_set_string(res, "status", "ok");
	std::lock_guard<std::mutex> lk(g_data->active_mutex);
	obs_data_set_int(res, "active_count",
			 (long long)g_data->active_items.size());
}

static void vendor_reload_cb(obs_data_t * /*req*/,
			     obs_data_t *res, void * /*priv*/)
{
	if (!g_data) {
		obs_data_set_string(res, "status", "error");
		return;
	}
	update_file_list(g_data);
	obs_data_set_string(res, "status", "ok");
	obs_data_set_int(res, "file_count",
			 (long long)g_data->file_list.size());
}

// ============================================================
//  Properties buttons
// ============================================================
static bool btn_test_spawn(obs_properties_t *,
			   obs_property_t *, void *priv)
{
	auto *data = static_cast<random_media_data *>(priv);
	blog(LOG_INFO, "[RandomMedia] Test Spawn clicked");
	do_spawn(data);
	return true;
}

static bool btn_reload_files(obs_properties_t *props,
			     obs_property_t *, void *priv)
{
	auto *data = static_cast<random_media_data *>(priv);
	update_file_list(data);
	obs_property_t *info =
		obs_properties_get(props, "file_count_info");
	if (info) {
		char buf[64];
		snprintf(buf, sizeof(buf), "Files found: %zu",
			 data->file_list.size());
		obs_property_set_description(info, buf);
	}
	return true;
}

// ============================================================
//  Source callbacks
// ============================================================
static const char *source_get_name(void *)
{
	return "Random Media Source";
}

static void source_update(void *d, obs_data_t *settings);

static void *source_create(obs_data_t *settings,
			   obs_source_t *source)
{
	auto *data = new random_media_data();
	data->source = source;
	g_data = data;
	source_update(data, settings);
	blog(LOG_INFO, "[Random Media Source] loaded");
	return data;
}

static void source_destroy(void *d)
{
	auto *data = static_cast<random_media_data *>(d);
	if (g_data == data)
		g_data = nullptr;
	delete data;
}

static void source_update(void *d, obs_data_t *settings)
{
	auto *data = static_cast<random_media_data *>(d);
	std::string new_folder =
		obs_data_get_string(settings, "folder");
	bool changed = (new_folder != data->folder);

	data->folder = new_folder;
	data->do_random_transform =
		obs_data_get_bool(settings, "random_transform");
	data->hide_on_end =
		obs_data_get_bool(settings, "hide_on_end");
	data->min_scale = (float)obs_data_get_double(
		settings, "min_scale");
	data->max_scale = (float)obs_data_get_double(
		settings, "max_scale");
	data->preserve_aspect =
		obs_data_get_bool(settings, "preserve_aspect");
	data->min_rot = (float)obs_data_get_double(
		settings, "min_rot");
	data->max_rot = (float)obs_data_get_double(
		settings, "max_rot");
	data->disable_rot =
		obs_data_get_bool(settings, "disable_rot");
	data->volume_db = (float)obs_data_get_double(
		settings, "volume_db");
	data->spawn_count =
		(int)obs_data_get_int(settings, "spawn_count");
	data->max_active =
		(int)obs_data_get_int(settings, "max_active");

	if (changed)
		update_file_list(data);
}

static obs_properties_t *source_properties(void *priv)
{
	auto *data = static_cast<random_media_data *>(priv);
	obs_properties_t *props = obs_properties_create();

	// --- Folder ---
	obs_properties_add_path(props, "folder",
				"Media Folder",
				OBS_PATH_DIRECTORY, nullptr,
				nullptr);

	char info_buf[64] = "Files found: 0";
	if (data)
		snprintf(info_buf, sizeof(info_buf),
			 "Files found: %zu",
			 data->file_list.size());
	obs_properties_add_text(props, "file_count_info",
				info_buf, OBS_TEXT_INFO);
	obs_properties_add_button2(props, "btn_reload",
				   "Reload File List",
				   btn_reload_files, data);

	// --- Audio ---
	obs_properties_add_float_slider(
		props, "volume_db", "Volume (dB)", -60.0, 0.0,
		0.5);

	// --- Transform ---
	obs_properties_add_bool(props, "random_transform",
				"Apply Random Transform");
	obs_properties_add_float_slider(
		props, "min_scale",
		"Min Size (% of canvas width)", 5.0, 100.0, 1.0);
	obs_properties_add_float_slider(
		props, "max_scale",
		"Max Size (% of canvas width)", 5.0, 100.0, 1.0);
	obs_properties_add_bool(props, "preserve_aspect",
				"Preserve Aspect Ratio");
	obs_properties_add_bool(props, "disable_rot",
				"Disable Rotation");
	obs_properties_add_float_slider(
		props, "min_rot", "Min Rotation (deg)", -360.0,
		360.0, 1.0);
	obs_properties_add_float_slider(
		props, "max_rot", "Max Rotation (deg)", -360.0,
		360.0, 1.0);

	// --- Playback ---
	obs_properties_add_bool(props, "hide_on_end",
				"Remove after playback ends");
	obs_properties_add_int(props, "spawn_count",
			       "Videos per Trigger", 1, 10, 1);
	obs_properties_add_int(props, "max_active",
			       "Max Simultaneous Videos", 1, 20,
			       1);

	// --- Test ---
	obs_properties_add_button2(props, "btn_spawn",
				   "▶  Test Spawn Now",
				   btn_test_spawn, data);

	return props;
}

static void source_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "min_scale",
				    20.0);
	obs_data_set_default_double(settings, "max_scale",
				    40.0);
	obs_data_set_default_bool(settings, "preserve_aspect",
				  true);
	obs_data_set_default_double(settings, "min_rot",
				    -30.0);
	obs_data_set_default_double(settings, "max_rot",
				    30.0);
	obs_data_set_default_bool(settings, "hide_on_end",
				  true);
	obs_data_set_default_bool(settings, "random_transform",
				  true);
	obs_data_set_default_int(settings, "spawn_count", 1);
	obs_data_set_default_int(settings, "max_active", 5);
	obs_data_set_default_double(settings, "volume_db",
				    -6.0);
}

static uint32_t source_get_width(void *)
{
	return 1;
}
static uint32_t source_get_height(void *)
{
	return 1;
}

// ============================================================
//  Module registration
// ============================================================
static struct obs_source_info random_media_info = {};

bool obs_module_load(void)
{
	random_media_info.id = "random_media_source";
	random_media_info.type = OBS_SOURCE_TYPE_INPUT;
	random_media_info.output_flags =
		OBS_SOURCE_DO_NOT_DUPLICATE;
	random_media_info.get_name = source_get_name;
	random_media_info.create = source_create;
	random_media_info.destroy = source_destroy;
	random_media_info.get_width = source_get_width;
	random_media_info.get_height = source_get_height;
	random_media_info.get_properties = source_properties;
	random_media_info.get_defaults = source_defaults;
	random_media_info.update = source_update;

	obs_register_source(&random_media_info);
	blog(LOG_INFO,
	     "[RandomMedia] Plugin loaded"
	     " — id: random_media_source");
	return true;
}

void obs_module_post_load(void)
{
	ws_vendor_ptr vendor =
		vendor_register("random_media_source");
	if (!vendor) {
		blog(LOG_INFO,
		     "[RandomMedia] Vendor API unavailable"
		     " — use Test Spawn button");
		return;
	}
	vendor_add_request(vendor, "spawn", vendor_spawn_cb,
			   nullptr);
	vendor_add_request(vendor, "reload_files",
			   vendor_reload_cb, nullptr);
	blog(LOG_INFO,
	     "[RandomMedia] WebSocket vendor ready"
	     " — requests: 'spawn', 'reload_files'");
}

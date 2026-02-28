// Minimal obs-websocket vendor API header
// Based on obsproject/obs-websocket lib/obs-websocket-api.h
// Only the parts needed for vendor request registration.
#pragma once

#include <obs-module.h>
#include <callback/proc.h>
#include <callback/calldata.h>

typedef void *obs_websocket_vendor;
typedef void (*obs_websocket_request_callback_function)(
	obs_data_t *request_data,
	obs_data_t *response_data,
	void *priv_data);

struct obs_websocket_request_callback {
	obs_websocket_request_callback_function callback;
	void *priv_data;
};

static proc_handler_t *_obs_ws_ph = nullptr;

static inline proc_handler_t *obs_websocket_get_ph(void)
{
	proc_handler_t *global_ph = obs_get_proc_handler();
	if (!global_ph)
		return nullptr;
	calldata_t cd = {0, 0, 0, 0};
	if (!proc_handler_call(global_ph,
			       "obs_websocket_api_get_ph",
			       &cd)) {
		calldata_free(&cd);
		return nullptr;
	}
	proc_handler_t *ret =
		(proc_handler_t *)calldata_ptr(&cd, "ph");
	calldata_free(&cd);
	return ret;
}

static inline bool obs_websocket_ensure_ph(void)
{
	if (!_obs_ws_ph)
		_obs_ws_ph = obs_websocket_get_ph();
	return _obs_ws_ph != nullptr;
}

static inline bool obs_websocket_vendor_run_simple_proc(
	obs_websocket_vendor vendor,
	const char *proc_name,
	calldata_t *cd)
{
	(void)vendor;
	if (!obs_websocket_ensure_ph())
		return false;
	proc_handler_call(_obs_ws_ph, proc_name, cd);
	return calldata_bool(cd, "success");
}

// Register a new vendor. Call from obs_module_post_load().
static inline obs_websocket_vendor
obs_websocket_register_vendor(const char *vendor_name)
{
	if (!obs_websocket_ensure_ph())
		return nullptr;
	calldata_t cd = {0, 0, 0, 0};
	calldata_set_string(&cd, "vendor_name", vendor_name);
	proc_handler_call(_obs_ws_ph,
			  "obs_websocket_create_vendor", &cd);
	obs_websocket_vendor vendor =
		(obs_websocket_vendor)calldata_ptr(&cd, "vendor");
	calldata_free(&cd);
	return vendor;
}

// Register a request handler for a vendor.
static inline bool obs_websocket_vendor_register_request(
	obs_websocket_vendor vendor,
	const char *request_type,
	obs_websocket_request_callback_function callback,
	void *priv_data)
{
	if (!vendor || !obs_websocket_ensure_ph())
		return false;
	struct obs_websocket_request_callback cb = {
		callback, priv_data};
	calldata_t cd = {0, 0, 0, 0};
	calldata_set_string(&cd, "type", request_type);
	calldata_set_ptr(&cd, "callback", &cb);
	return obs_websocket_vendor_run_simple_proc(
		vendor, "vendor_request_register", &cd);
}

// obs-websocket-api.h
// Minimal reproduction of obsproject/obs-websocket lib/obs-websocket-api.h
// Based on official API patterns. For use in obs_module_post_load() ONLY.
#pragma once

#include <obs-module.h>
#include <callback/proc.h>
#include <callback/calldata.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *obs_websocket_vendor;

typedef void (*obs_websocket_request_callback_function)(
	obs_data_t *request_data, obs_data_t *response_data,
	void *priv_data);

struct obs_websocket_request_callback {
	obs_websocket_request_callback_function callback;
	void *priv_data;
};

static proc_handler_t *_obs_ws_ph = nullptr;

static inline bool obs_websocket_ensure_ph(void)
{
	if (_obs_ws_ph)
		return true;
	proc_handler_t *gph = obs_get_proc_handler();
	if (!gph)
		return false;
	calldata_t cd = {0, 0, 0, 0};
	bool ok = proc_handler_call(
		gph, "obs_websocket_api_get_ph", &cd);
	if (ok)
		_obs_ws_ph = (proc_handler_t *)calldata_ptr(
			&cd, "ph");
	calldata_free(&cd);
	return _obs_ws_ph != nullptr;
}

static inline obs_websocket_vendor
obs_websocket_register_vendor(const char *vendor_name)
{
	if (!obs_websocket_ensure_ph())
		return nullptr;
	calldata_t cd = {0, 0, 0, 0};
	calldata_set_string(&cd, "vendor_name", vendor_name);
	proc_handler_call(_obs_ws_ph,
			  "obs_websocket_create_vendor", &cd);
	obs_websocket_vendor v =
		(obs_websocket_vendor)calldata_ptr(&cd, "vendor");
	calldata_free(&cd);
	return v;
}

static inline bool obs_websocket_vendor_register_request(
	obs_websocket_vendor vendor,
	const char *request_type,
	obs_websocket_request_callback_function callback,
	void *priv_data)
{
	if (!vendor || !obs_websocket_ensure_ph())
		return false;

	struct obs_websocket_request_callback *cb =
		(struct obs_websocket_request_callback *)bmalloc(
			sizeof(*cb));
	cb->callback = callback;
	cb->priv_data = priv_data;

	calldata_t cd = {0, 0, 0, 0};
	calldata_set_ptr(&cd, "vendor", vendor);
	calldata_set_string(&cd, "type", request_type);
	calldata_set_ptr(&cd, "callback", cb);
	bool ok = proc_handler_call(
		_obs_ws_ph, "vendor_request_register", &cd);
	bool success = ok && calldata_bool(&cd, "success");
	calldata_free(&cd);
	if (!success)
		bfree(cb);
	return success;
}

#ifdef __cplusplus
}
#endif

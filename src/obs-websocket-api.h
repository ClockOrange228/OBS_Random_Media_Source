// obs-websocket-api.h  
// Minimal vendor API. vendor IS a proc_handler_t*.
// vendor_request_register is called on VENDOR's own proc handler.
#pragma once
#include <obs-module.h>
#include <callback/proc.h>
#include <callback/calldata.h>
#include <util/bmem.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef proc_handler_t *obs_websocket_vendor;
typedef void (*obs_websocket_request_callback_function)(
	obs_data_t *, obs_data_t *, void *);
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
	struct calldata cd;
	calldata_init(&cd);
	bool ok = proc_handler_call(
		gph, "obs_websocket_api_get_ph", &cd);
	if (ok)
		_obs_ws_ph = (proc_handler_t *)
			calldata_ptr(&cd, "ph");
	calldata_free(&cd);
	return _obs_ws_ph != nullptr;
}

static inline obs_websocket_vendor
obs_websocket_register_vendor(const char *vendor_name)
{
	if (!obs_websocket_ensure_ph())
		return nullptr;
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "vendor_name", vendor_name);
	proc_handler_call(
		_obs_ws_ph, "obs_websocket_create_vendor", &cd);
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
	if (!vendor)
		return false;
	struct obs_websocket_request_callback *cb =
		(struct obs_websocket_request_callback *)
			bmalloc(sizeof(*cb));
	cb->callback = callback;
	cb->priv_data = priv_data;
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "request_type", request_type);
	calldata_set_ptr(&cd, "request_callback", cb);
	// vendor IS a proc_handler_t — call on vendor's own ph
	proc_handler_call((proc_handler_t *)vendor,
			  "vendor_request_register", &cd);
	bool ok = calldata_bool(&cd, "success");
	calldata_free(&cd);
	if (!ok)
		bfree(cb);
	return ok;
}

#ifdef __cplusplus
}
#endif

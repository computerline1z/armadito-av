/***

Copyright (C) 2015, 2016 Teclib'

This file is part of Armadito core.

Armadito core is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Armadito core is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Armadito core.  If not, see <http://www.gnu.org/licenses/>.

***/

#include <libarmadito.h>

#include "config/libarmadito-config.h"

#include "jsonhandler.h"
#include "jsonhandlerp.h"
#include "debug.h"
#include "state.h"
#include "scan.h"
#include "conf.h"
#include "quarantine.h"
#include "update.h"
#include "os/string.h"

#include <assert.h>
#include <json.h>
#include <stdlib.h>
#include <string.h>

static void json_request_destroy(struct json_request *req)
{
	if (req->request != NULL)
		free((void *)req->request);

	if (req->params != NULL)
		assert(json_object_put(req->params));
}

static void json_response_destroy(struct json_response *resp)
{
	if (resp->response != NULL)
		free((void *)resp->response);

	if (resp->info != NULL)
		assert(json_object_put(resp->info));

	if (resp->error_message != NULL)
		free((void *)resp->error_message);
}

static struct request_dispatch_entry {
	const char *request;
	response_cb_t response;
	process_cb_t process;
} request_dispatch_table[] = {
	{ "state", state_response_cb, NULL},
	{ "scan", scan_response_cb, scan_process_cb},
	{ "quarantine", quarantine_response_cb, NULL},
	{ "conf_set", conf_response_cb, NULL},
	{ "updatedb", update_response_cb, NULL},
	{ "scan_cancel", scan_cancel_response_cb, NULL},
	{ NULL, NULL, NULL},
};

struct a6o_json_handler {
	struct json_tokener *tokener;
	struct armadito *armadito;
	process_cb_t process;
	void *request_data;
	//void *user_data; TODO... for change on-access state.
};

struct a6o_json_handler *a6o_json_handler_new(struct armadito *armadito)
{
	struct a6o_json_handler *j = malloc(sizeof(struct a6o_json_handler));

	if (armadito == NULL) {
		return NULL;
	}

	j->tokener = json_tokener_new();
	assert(j->tokener != NULL);

	j->armadito = armadito;

	j->process = NULL;
	j->request_data = NULL;	

	return j;
}

void a6o_json_handler_free(struct a6o_json_handler *j)
{
	json_tokener_free(j->tokener);
	free(j);
}

static const char *status_2_error(enum a6o_json_status status)
{
	switch(status) {
	case JSON_OK: return "ok";
	case JSON_PARSE_ERROR: return "invalid JSON string";
	case JSON_MALFORMED_REQUEST: return "malformed request in JSON object";
	case JSON_INVALID_REQUEST: return "invalid request";
	case JSON_REQUEST_FAILED: return "execution of request failed";
	}

	return "??? invalid status";
}

static enum a6o_json_status parse_request(struct a6o_json_handler *j, const char *req, int req_len, struct json_object **p_j_request)
{
	json_tokener_reset(j->tokener);

	*p_j_request = json_tokener_parse_ex(j->tokener, req, req_len);

	if (*p_j_request == NULL) {
		enum json_tokener_error jerr = json_tokener_get_error(j->tokener);

		a6o_log(ARMADITO_LOG_MODULE, ARMADITO_LOG_LEVEL_WARNING, "error in JSON parsing: %s", json_tokener_error_desc(jerr));

		return JSON_PARSE_ERROR;
	}

	return JSON_OK;
}

static enum a6o_json_status extract_request(struct json_object *j_request, struct json_request *av_request)
{
	struct json_object *j_av_request, *j_id, *j_params;

	/* check if request is truly a JSON object, not a primitive value ? */
	if (!json_object_is_type(j_request, json_type_object))
		return JSON_INVALID_REQUEST;

	/* check if object contains key "av_request" with a string value */
	if (!json_object_object_get_ex(j_request, "av_request", &j_av_request)
		|| !json_object_is_type(j_av_request, json_type_string))
		return JSON_INVALID_REQUEST;

	/* check if object contains key "id" with a int value */
	if (!json_object_object_get_ex(j_request, "id", &j_id)
		|| !json_object_is_type(j_id, json_type_int))
		return JSON_INVALID_REQUEST;

	/* check if object contains key "params" with an object value */
	if (!json_object_object_get_ex(j_request, "params", &j_params)
		|| !json_object_is_type(j_params, json_type_object))
		return JSON_INVALID_REQUEST;

	av_request->request = os_strdup(json_object_get_string(j_av_request));
	av_request->id = json_object_get_int(j_id);
	av_request->params = json_object_get(j_params);

	/* note that this will automatically free the objects contained in j_request, i.e. j_id and j_av_request */
	/* but NOT j_params which has been extracted with json_object_get() */
	assert(json_object_put(j_request));

	return JSON_OK;
}

enum a6o_json_status call_request_handler(struct a6o_json_handler *j, struct json_request *av_request, struct json_response *av_response)
{
	enum a6o_json_status status = JSON_INVALID_REQUEST;
	struct request_dispatch_entry *p;

	for (p = request_dispatch_table; p->request != NULL && strcmp(p->request, av_request->request); p++)
		;

	if (p->request != NULL) {
		response_cb_t cb = p->response;

		status = (*cb)(j->armadito, av_request, av_response, &j->request_data);

		j->process = p->process;

#ifndef _WIN32
		if (av_response->info != NULL)
			jobj_debug(av_response->info, "info");
#endif
	}

	return status;
}

static enum a6o_json_status fill_response(struct json_response *av_response, char **p_resp, int *p_resp_len)
{
	struct json_object *j_response = json_object_new_object();

	if (av_response->response != NULL)
		json_object_object_add(j_response, "av_response", json_object_new_string(av_response->response));

	json_object_object_add(j_response, "id", json_object_new_int(av_response->id));

	json_object_object_add(j_response, "status", json_object_new_int(av_response->status));

	if (av_response->status == JSON_OK) {
		if (av_response->info != NULL)
			json_object_object_add(j_response, "info", json_object_get(av_response->info));
	} else {
		if (av_response->error_message == NULL)
			json_object_object_add(j_response, "error-message", json_object_new_string(status_2_error(av_response->status)));
		else
			json_object_object_add(j_response, "error-message", json_object_new_string(av_response->error_message));
	}

#ifndef _WIN32
	jobj_debug(j_response, "AV response");
#endif

	*p_resp = (char *)os_strdup(json_object_to_json_string(j_response));
	*p_resp_len = strlen(*p_resp);

	/* note that this will automatically free the objects contained in j_response */
	/* but NOT its "info" member which has been referenced with json_object_get() */
	assert(json_object_put(j_response));

	return JSON_OK;
}

enum a6o_json_status a6o_json_handler_get_response(struct a6o_json_handler *j, const char *req, int req_len, char **p_resp, int *p_resp_len)
{
	struct json_object *j_request, *j_response;
	struct json_request av_request;
	struct json_response av_response;

	av_response.response = NULL;
	av_response.id = -1;
	av_response.info = NULL;
	av_response.error_message = NULL;

	// Check parameters
	if (j == NULL || req == NULL || req_len <= 0) {
		return JSON_UNEXPECTED_ERR;
	}

	av_response.status = parse_request(j, req, req_len, &j_request);

	if (av_response.status)
		goto get_out;

#ifndef _WIN32
	jobj_debug(j_request, "AV request");
#endif

	av_response.status = extract_request(j_request, &av_request);

	if (av_response.status)
		goto get_out;

	av_response.response = os_strdup(av_request.request);
	av_response.id = av_request.id;

	av_response.status = call_request_handler(j, &av_request, &av_response);

get_out:
	fill_response(&av_response, p_resp, p_resp_len);

	if (&av_request != NULL)
		json_request_destroy(&av_request);

	if (&av_response != NULL)
		json_response_destroy(&av_response);

	return av_response.status;
}

void a6o_json_handler_process(struct a6o_json_handler *j)
{
	if (j->process != NULL)
		(*j->process)(j->armadito, j->request_data);
}

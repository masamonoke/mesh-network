#include "format.h"

#include "settings.h"
#include "custom_logger.h"
#include "control_utils.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

void format_sprint_result(enum request_result res, char buf[], size_t len) {
	switch (res) {
		case REQUEST_OK:
			snprintf(buf, len, "[OK]: %d", res);
			break;
		case REQUEST_ERR:
			snprintf(buf, len, "[ERR]: %d", res);
			break;
		case REQUEST_UNKNOWN:
			snprintf(buf, len, "[UNKNOWN]: %d", res);
			break;
		default:
			custom_log_error("Unknown result response");
			break;
	}
}

enum request_sender format_define_sender(const uint8_t* buf) {
	const uint8_t* p;
	enum request_sender sender;
	enum_ir tmp;

	p = buf;
	p += sizeof_enum(enum request); // skip cmd
	memcpy(&tmp, p, sizeof_enum(sender));
	sender = (enum request_sender) tmp;

	return sender;
}

bool format_is_message_correct(size_t buf_len, msg_len_type msg_len) {

	if (buf_len > sizeof(msg_len)) {
		if (buf_len != msg_len) {
			custom_log_error("Incorrect message format: buffer length (%d) != delcared message length (%d)", buf_len, msg_len);
			return false;
		}
	} else {
		custom_log_error("Incorrect message format: buffer length (%d) < size of uint32_t", buf_len);
		return false;
	}

	return true;
}

static uint8_t* create_base(uint8_t* message, msg_len_type msg_len, enum request cmd, enum request_sender sender); // NOLINT

static uint8_t* skip_base(const uint8_t* message);

static void create_send(uint8_t* p, const void* payload, uint8_t* message, msg_len_type* msg_len, enum request_sender sender);

static void parse_send(const uint8_t* buf, send_t* payload);

static void create_broadcast(uint8_t* p, const void* payload, uint8_t* message, msg_len_type* msg_len, enum request_sender sender, enum request cmd);

static void parse_broadcast(const uint8_t* buf, broadcast_t* payload);

void format_create(enum request req, const void* payload, uint8_t* buf, msg_len_type* len, enum request_sender sender) {
	uint8_t payload_len;
	uint8_t* p;

	p = NULL;
	switch (req) {
		case REQUEST_PING:
		case REQUEST_KILL_NODE:
		case REQUEST_REVIVE_NODE:
		case REQUEST_RESET:
			if (payload) {
				uint8_t* ret_payload;

				ret_payload = (uint8_t*) payload;
				payload_len = sizeof(*ret_payload);

				*len = payload_len + sizeof_enum(request) + sizeof(*len) + sizeof_enum(sender);
				p = create_base(buf, *len, req, sender);

				memcpy(p, ret_payload, sizeof(*ret_payload));
			} else {
				*len = sizeof_enum(sender) + sizeof_enum(request) + sizeof(*len);
				p = create_base(buf, *len, req, sender);
			}
			break;
		case REQUEST_SEND:
				create_send(p, payload, buf, len, sender);
			break;
		case REQUEST_BROADCAST:
		case REQUEST_UNICAST:
			if (payload) {
				create_broadcast(p, payload, buf, len, sender, req);
			}
			break;
		case REQUEST_UPDATE:
			if (payload) {
				int32_t pid;
				node_update_t* update_payload;
				update_payload = (node_update_t*) payload;
				pid = getpid();

				*len = sizeof(node_update_t) + MSG_BASE_LEN;

				p = create_base(buf, *len, req, sender);

				// payload
				memcpy(p, &update_payload->port, sizeof(update_payload->port));
				p += sizeof(update_payload->port);
				memcpy(p, &update_payload->addr, sizeof(update_payload->addr));
				p += sizeof(update_payload->addr);
				memcpy(p, &pid, sizeof(pid));
				p += sizeof(pid);
			} else {
				custom_log_error("Null payload passed");
			}
			break;
		case REQUEST_NOTIFY:
			{
				notify_t* notify_payload;

				notify_payload = (notify_t*) payload;

				*len = MSG_BASE_LEN + sizeof(notify_t);

				p = create_base(buf, *len, req, sender);

				//payload
				memcpy(p, &notify_payload->type, sizeof(notify_payload->type));
				p += sizeof(notify_payload->type);
				memcpy(p, &notify_payload->app_msg_id, sizeof(notify_payload->app_msg_id));
			}
			break;
		case REQUEST_ROUTE_DIRECT:
		case REQUEST_ROUTE_INVERSE:
			{
				route_payload_t* route_payload;

				route_payload = (route_payload_t*) payload;

				payload_len = sizeof(route_payload->local_sender_addr) + sizeof(route_payload->sender_addr) + sizeof(route_payload->receiver_addr) +
				sizeof(route_payload->time_to_live) + sizeof(route_payload->id) + format_app_message_len(&route_payload->app_payload);
				*len = payload_len + sizeof_enum(request) + sizeof(*len) + sizeof_enum(sender);

				p = create_base(buf, *len, req, sender);

				memcpy(p, &route_payload->sender_addr, sizeof(route_payload->sender_addr));
				p += sizeof(route_payload->sender_addr);
				memcpy(p, &route_payload->receiver_addr, sizeof(route_payload->receiver_addr));
				p += sizeof(route_payload->receiver_addr);
				memcpy(p, &route_payload->local_sender_addr, sizeof(route_payload->local_sender_addr));
				p += sizeof(route_payload->local_sender_addr);
				memcpy(p, &route_payload->time_to_live, sizeof(route_payload->time_to_live));
				p += sizeof(route_payload->time_to_live);
				memcpy(p, &route_payload->id, sizeof(route_payload->id));
				p += sizeof(route_payload->id);

				format_app_create_message(&route_payload->app_payload, p);
			}
			break;
		case REQUEST_UNICAST_CONTEST:
		case REQUEST_UNICAST_FIRST:
			{
				unicast_contest_t* unicast;

				unicast = (unicast_contest_t*) payload;

				*len = sizeof(unicast_contest_t) + MSG_BASE_LEN;

				p = create_base(buf, *len, req, sender);
				memcpy(p, &unicast->req, sizeof(unicast->req));
				p += sizeof(unicast->req);
				memcpy(p, &unicast->node_addr, sizeof(unicast->node_addr));
				p += sizeof(unicast->node_addr);

				format_app_create_message(&unicast->app_payload, p);
			}
			break;
		default:
			not_implemented();
			break;
	}
}

static void parse_addr_payload(const uint8_t* buf, void* ret_payload);

static void parse_route_payload(const uint8_t* buf, route_payload_t* payload);

static void parse_node_update_payload(const uint8_t* buf, node_update_t* payload);

static void parse_notify_payload(const uint8_t* buf, notify_t* payload);

static void parse_unicast_contest_payload(const uint8_t* buf, unicast_contest_t* payload);

void format_parse(enum request* req, void** payload, const void* buf) {
	const uint8_t* p;
	enum request cmd;
	enum_ir tmp;

	*req = REQUEST_UNDEFINED;
	p = buf;

	memcpy(&tmp, p, sizeof_enum(cmd));
	cmd = (enum request) tmp;

	switch (cmd) {
		case REQUEST_SEND:
			*req = cmd;
			*payload = malloc(sizeof(send_t));
			parse_send(buf, (send_t*) *payload);
			break;
		case REQUEST_PING:
		case REQUEST_REVIVE_NODE:
		case REQUEST_KILL_NODE:
			*req = cmd;
			*payload = malloc(sizeof(uint8_t));
			parse_addr_payload(buf, *payload);
			break;
		case REQUEST_RESET:
			*req = cmd;
			break;
		case REQUEST_UNDEFINED:
			custom_log_error("Unknown client-server request");
			break;
		case REQUEST_BROADCAST:
		case REQUEST_UNICAST:
			*req = cmd;
			*payload = malloc(sizeof(broadcast_t));
			parse_broadcast(buf, (broadcast_t*) *payload);
			break;
		case REQUEST_ROUTE_DIRECT:
		case REQUEST_ROUTE_INVERSE:
			*req = cmd;
			*payload = malloc(sizeof(route_payload_t));
			parse_route_payload(buf, (route_payload_t*) *payload);
			break;
		case REQUEST_UPDATE:
			*req = cmd;
			*payload = malloc(sizeof(node_update_t));
			parse_node_update_payload(buf, *payload);
			break;
		case REQUEST_NOTIFY:
			*req = cmd;
			*payload = malloc(sizeof(notify_t));
			parse_notify_payload(buf, *payload);
			break;
		case REQUEST_UNICAST_CONTEST:
		case REQUEST_UNICAST_FIRST:
			*req = cmd;
			*payload = malloc(sizeof(unicast_contest_t));
			parse_unicast_contest_payload(buf, *payload);
			break;
		default:
			not_implemented();
			break;
	}
}

static void parse_addr_payload(const uint8_t* buf, void* ret_payload) {
	const uint8_t* p;
	uint8_t* payload;

	payload = (uint8_t*) ret_payload;

	p = skip_base(buf);

	memcpy(payload, p, sizeof(*payload));
	p += sizeof(payload);
}

static void parse_route_payload(const uint8_t* buf, route_payload_t* payload) {
	const uint8_t* p;

	p = skip_base(buf);

	// parse payload
	memcpy(&payload->sender_addr, p, sizeof(payload->sender_addr));
	p += sizeof(payload->sender_addr);
	memcpy(&payload->receiver_addr, p, sizeof(payload->receiver_addr));
	p += sizeof(payload->receiver_addr);
	memcpy(&payload->local_sender_addr, p, sizeof(payload->local_sender_addr));
	p += sizeof(payload->local_sender_addr);
	memcpy(&payload->time_to_live, p, sizeof(payload->time_to_live));
	p += sizeof(payload->time_to_live);
	memcpy(&payload->id, p, sizeof(payload->id));
	p += sizeof(payload->id);

	format_app_parse_message(&payload->app_payload, p);
}

static void parse_node_update_payload(const uint8_t* buf, node_update_t* payload) {
	const uint8_t* p;

	p = skip_base(buf);

	// parse payload
	memcpy(&payload->port, p, sizeof(payload->port));
	p += sizeof(payload->port);
	memcpy(&payload->addr, p, sizeof(payload->addr));
	p += sizeof(payload->addr);
	memcpy(&payload->pid, p, sizeof(payload->pid));
	p += sizeof(payload->pid);
}

static void parse_unicast_contest_payload(const uint8_t* buf, unicast_contest_t* payload) {
	const uint8_t* p;

	p = skip_base(buf);

	// parse payload
	memcpy(&payload->req, p, sizeof(payload->req));
	p += sizeof(payload->req);
	memcpy(&payload->node_addr, p, sizeof(payload->node_addr));
	p += sizeof(payload->node_addr);
	format_app_parse_message(&payload->app_payload, p);
}

static void parse_notify_payload(const uint8_t* buf, notify_t* payload) {
	const uint8_t* p;

	p = skip_base(buf);

	// parse payload
	memcpy(&payload->type, p, sizeof(payload->type));
	p += sizeof(payload->type);
	memcpy(&payload->app_msg_id, p, sizeof(payload->app_msg_id));
	p += sizeof(payload->app_msg_id);
}

static uint8_t* create_base(uint8_t* message, msg_len_type msg_len, enum request cmd, enum request_sender sender) { // NOLINT
	uint8_t* p;

	p = message;
	memcpy(p, &msg_len, sizeof(msg_len));
	p += sizeof(msg_len);
	memcpy(p, &cmd, sizeof_enum(cmd));
	p += sizeof_enum(cmd);
	memcpy(p, &sender, sizeof_enum(sender));
	p += sizeof_enum(sender);

	return p;
}

static uint8_t* skip_base(const uint8_t* message) {
	uint8_t* p;

	p = (uint8_t*) message;

	p += sizeof_enum(enum request); // skip cmd
	p += sizeof_enum(enum request_sender); // skip sender

	return p;
}

static void create_broadcast(uint8_t* p, const void* payload, uint8_t* message, msg_len_type* msg_len, enum request_sender sender, enum request cmd) {
	broadcast_t* ret_payload;
	uint8_t payload_len;

	ret_payload = (broadcast_t*) payload;

	payload_len = sizeof(ret_payload->addr_from) + sizeof(ret_payload->time_to_live) + format_app_message_len(&ret_payload->app_payload);
	*msg_len = payload_len + sizeof_enum(cmd) + sizeof(*msg_len) + sizeof_enum(sender);

	p = create_base(message, *msg_len, cmd, sender);

	memcpy(p, &ret_payload->addr_from, sizeof(ret_payload->addr_from));
	p += sizeof(ret_payload->addr_from);
	memcpy(p, &ret_payload->time_to_live, sizeof(ret_payload->time_to_live));
	p += sizeof(ret_payload->time_to_live);

	format_app_create_message(&ret_payload->app_payload, p);
}

static void parse_broadcast(const uint8_t* buf, broadcast_t* payload) {
	const uint8_t* p;

	p = skip_base(buf);

	memcpy(&payload->addr_from, p, sizeof(payload->addr_from));
	p += sizeof(payload->addr_from);
	memcpy(&payload->time_to_live, p, sizeof(payload->time_to_live));
	p += sizeof(payload->time_to_live);

	format_app_parse_message(&payload->app_payload, p);
}

static void create_send(uint8_t* p, const void* payload, uint8_t* message, msg_len_type* msg_len, enum request_sender sender) {
	send_t* ret_payload;
	uint8_t payload_len;

	ret_payload = (send_t*) payload;

	payload_len = sizeof(ret_payload->addr_to) + sizeof(ret_payload->addr_from) + format_app_message_len(&ret_payload->app_payload);
	*msg_len = payload_len + sizeof_enum(REQUEST_SEND) + sizeof(*msg_len) + sizeof_enum(sender);

	p = create_base(message, *msg_len, REQUEST_SEND, sender);

	memcpy(p, &ret_payload->addr_from, sizeof(ret_payload->addr_from));
	p += sizeof(ret_payload->addr_from);
	memcpy(p, &ret_payload->addr_to, sizeof(ret_payload->addr_to));
	p += sizeof(ret_payload->addr_to);

	format_app_create_message(&ret_payload->app_payload, p);
	p += format_app_message_len(&ret_payload->app_payload);
}

static void parse_send(const uint8_t* buf, send_t* payload) {
	const uint8_t* p;

	p = skip_base(buf);

	memcpy(&payload->addr_from, p, sizeof(payload->addr_from));
	p += sizeof(payload->addr_from);
	memcpy(&payload->addr_to, p, sizeof(payload->addr_to));
	p += sizeof(payload->addr_to);

	format_app_parse_message(&payload->app_payload, p);
	p += format_app_message_len(&payload->app_payload);
}

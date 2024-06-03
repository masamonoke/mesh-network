#include "node_handler.h"

#include <memory.h>

#include "node_essentials.h"
#include "io.h"
#include "node_app.h"

#include <assert.h>

#define MAX_MESSAGE_DATA 100

struct message_data {
	uint16_t id;
	// if message was delivered by some route direct packet
	// and route inverse is already sent
	bool stop_inverse;
	// if broadcast message with this id was handled then ignore it
	bool was_message;
	bool unicast_first;
};

static uint8_t message_num = 0;
static bool init = false;
struct message_data messages[MAX_MESSAGE_DATA];

static bool is_id_set(uint16_t id);

static void init_messages_data(void);

static bool get_inverse_by_id(uint16_t id, bool* stop_inverse);

static bool get_was_message_by_id(uint16_t id, bool* was_message);

static void set_inverse_by_id(uint16_t id, bool stop_inverse);

static void set_was_message_by_id(uint16_t id, bool was_message);

static bool get_unicast_status_by_id(uint16_t id, bool* unicast_first);

static void set_unicast_status_by_id(uint16_t id, bool unicast_first);

bool handle_ping(int32_t conn_fd) {
	enum request_result req_res;

	req_res = REQUEST_OK;
	if (!io_write_all(conn_fd, (uint8_t*) &req_res, sizeof_enum(req_res))) {
		node_log_error("Failed to response to ping");
		return false;
	}

	return true;
}

bool handle_server_send(enum request cmd_type, uint8_t addr, const void* payload, const routing_table_t* routing, app_t apps[APPS_COUNT]) { // NOLINT
	send_t* ret_payload;
	uint8_t b[MAX_MSG_LEN];
	msg_len_type buf_len;
	uint8_t next_addr;
	bool res;
	notify_t notify;

	res = true;
	ret_payload = (send_t*) payload;
	notify.app_msg_id = ret_payload->app_payload.id;

	node_app_setup_delivery(&ret_payload->app_payload);

	if (ret_payload->addr_to == addr) {
		node_log_warn("Message for node itself");

		if (node_app_handle_request(apps, &ret_payload->app_payload, addr)) {
			notify.type = NOTIFY_GOT_MESSAGE;
			if (!node_essentials_notify_server(&notify)) {
				node_log_error("Failed to notify server");
			}
		} else {
			node_log_error("Failed to handle app request");
			notify.type = NOTIFY_FAIL;
			if (!node_essentials_notify_server(&notify)) {
				node_log_error("Failed to notify fail");
			}
		}

		return res;
	}

	node_log_debug("Finding route to %d", ret_payload->addr_to);
	next_addr = routing_next_addr(routing, ret_payload->addr_to);
	if (next_addr == UINT8_MAX) {
		node_log_warn("Failed to find route");

		route_payload_t route_payload = {
			.local_sender_addr = addr,
			.sender_addr = ret_payload->addr_from,
			.receiver_addr = ret_payload->addr_to,
			.time_to_live = TTL,
			.app_payload = ret_payload->app_payload
		};

		node_essentials_broadcast_route(&route_payload, false);

		return false;
	}

	format_create(cmd_type, ret_payload, b, &buf_len, REQUEST_SENDER_NODE);

	if (node_essentials_get_conn_and_send(node_port(next_addr), b, buf_len)) {
		node_log_info("Sent message (length %d) from %d:%d to %d:%d",
			ret_payload->app_payload.message_len, ret_payload->addr_from, ret_payload->app_payload.addr_from, ret_payload->addr_to, ret_payload->app_payload.addr_to);
	}

	return res;
}

void handle_broadcast(broadcast_t* broadcast_payload) {
	node_app_setup_delivery(&broadcast_payload->app_payload);
	node_essentials_broadcast(broadcast_payload);
}

void handle_server_unicast(broadcast_t* broadcast_payload, uint8_t cur_node_addr) {
	node_app_setup_delivery(&broadcast_payload->app_payload);
	unicast_contest_t unicast = {
		.node_addr = cur_node_addr,
		.app_payload = broadcast_payload->app_payload,
		.req = REQUEST_UNICAST_CONTEST
	};

	node_essentials_send_unicast_contest(&unicast);
}

__attribute__((warn_unused_result))
static bool node_handle_app_request(app_t apps[APPS_COUNT], send_t* send_payload, uint8_t addr);

__attribute__((warn_unused_result))
static bool send_next(const routing_table_t* routing, send_t* ret_payload);

bool handle_node_send(uint8_t addr, const void* payload, const routing_table_t* routing, app_t apps[APPS_COUNT]) {
	uint8_t addr_to;
	bool res;

	node_log_warn("Send node %d", addr);

	res = true;
	addr_to = ((send_t*) payload)->addr_to;
	if (addr_to == addr) {
		if (!node_handle_app_request(apps, (send_t*) payload, addr)) {
			node_log_error("Failed to handle app request");
			res = false;
		}
	} else {
		if (!send_next(routing, (send_t*) payload)) {
			node_log_error("Failed to send app request next");
			res = false;
		}
	}

	return res;
}

bool route_direct_handle_delivered(routing_table_t* routing, route_payload_t* route_payload, uint8_t server_addr, app_t apps[APPS_COUNT]);

bool handle_node_route_direct(routing_table_t* routing, uint8_t server_addr, void* payload, app_t apps[APPS_COUNT]) {
	route_payload_t* route_payload;
	bool was_message;
	int8_t new_metric;

	route_payload = (route_payload_t*) payload;

	if (route_payload->time_to_live <= 0) {
		return true;
	}

	if (get_was_message_by_id(route_payload->app_payload.id, &was_message)) {
		if (was_message) {
			return 0;
		} else {
			set_was_message_by_id(route_payload->app_payload.id, true);
		}
	} else {
		set_was_message_by_id(route_payload->app_payload.id, true);
	}

	new_metric = TTL - route_payload->time_to_live + 1;
	if (new_metric > 0) {
		if (routing_next_addr(routing, route_payload->sender_addr) == UINT8_MAX) {
			routing_set_addr(routing, route_payload->sender_addr, route_payload->local_sender_addr, new_metric);
		} else {
			int8_t old_metric;

			old_metric = routing_get(routing, route_payload->sender_addr).metric;
			if (old_metric > new_metric) {
				routing_set_addr(routing, route_payload->sender_addr, route_payload->local_sender_addr, new_metric);
				node_log_debug("Replaced old path with metric %d to new path with metric %d", old_metric, new_metric);
			}
		}
	}

	if (route_payload->receiver_addr == server_addr) {
		route_direct_handle_delivered(routing, route_payload, server_addr, apps);
		return true;
	}

	route_payload->local_sender_addr = server_addr;

	node_essentials_broadcast_route(route_payload, false);

	return true;
}

void handle_unicast_contest(unicast_contest_t* unicast, uint8_t cur_node_addr) {
	node_log_debug("Unicast contest request on node %d", cur_node_addr);
	node_essentials_send_unicast_first(unicast, cur_node_addr);
}

void handle_unicast_first(unicast_contest_t* unicast, uint8_t cur_node_addr) {
	bool unicast_first;

	if (get_unicast_status_by_id(unicast->app_payload.id, &unicast_first)) {
		if (!unicast_first) {
			set_unicast_status_by_id(unicast->app_payload.id, true);
		} else {
			return;
		}
	} else {
		uint8_t b[MAX_MSG_LEN];
		uint8_t buf_len;

		set_unicast_status_by_id(unicast->app_payload.id, true);
		node_log_warn("Node %d won unicast contest", unicast->node_addr);

		send_t send_payload = {
			.app_payload = unicast->app_payload,
			.addr_from = cur_node_addr,
			.addr_to = unicast->node_addr
		};
		format_create(REQUEST_SEND, &send_payload, b, &buf_len, REQUEST_SENDER_NODE);

		if (!node_essentials_get_conn_and_send(node_port(unicast->node_addr), b, buf_len)) {
			node_log_error("Failed to send response to unicast first");
		}
	}
}

bool handle_node_route_inverse(routing_table_t* routing, void* payload, uint8_t server_addr) {
	route_payload_t* route_payload;
	uint8_t next_addr;
	uint8_t b[sizeof(route_payload_t)];
	msg_len_type buf_len;
	int8_t new_metric;

	route_payload = (route_payload_t*) payload;

	node_log_warn("Inverse node %d", server_addr);

	new_metric = TTL - route_payload->time_to_live + 1;
	if (new_metric > 0) {
		if (routing_next_addr(routing, route_payload->receiver_addr) == UINT8_MAX) {
			routing_set_addr(routing, route_payload->receiver_addr, route_payload->local_sender_addr, new_metric);
		}
	}

	if (route_payload->sender_addr == server_addr) {
		node_log_debug("Route inverse request came back");
		return true;
	}

	next_addr = routing_next_addr(routing, route_payload->sender_addr);
	if (next_addr == UINT8_MAX) {
		node_log_error("Failed to get next addr to %d", route_payload->sender_addr);
		return false;
	}

	route_payload->local_sender_addr = server_addr;
	route_payload->time_to_live--;

	format_create(REQUEST_ROUTE_INVERSE, route_payload, b, &buf_len, REQUEST_SENDER_NODE);

	if (!node_essentials_get_conn_and_send(node_port(next_addr), b, buf_len)) {
		node_log_error("Failed to connect to node %d while travel back", next_addr);
		routing_del(routing, route_payload->sender_addr);
		return false;
	}

	return true;
}

__attribute__((warn_unused_result))
static bool node_handle_app_request(app_t apps[APPS_COUNT], send_t* send_payload, uint8_t addr) {
	enum app_request app_req;
	bool res;
	notify_t notify;

	app_req = send_payload->app_payload.req_type;
	res = true;
	notify.app_msg_id = send_payload->app_payload.id;

	if (app_req == APP_REQUEST_UNICAST) {
		notify.type = NOTIFY_GOT_MESSAGE;
		if (!node_essentials_notify_server(&notify)) {
			node_log_error("Failed to notify server");
		}
		notify.type = NOTIFY_UNICAST_HANDLED;
		if (!node_essentials_notify_server(&notify)) {
			node_log_error("Failed to notify server");
		}
	}
	if (node_app_handle_request(apps, &send_payload->app_payload, addr)) {
		notify.type = NOTIFY_GOT_MESSAGE;
		if (!node_essentials_notify_server(&notify)) {
			node_log_error("Failed to notify server");
			res = false;
		}
	} else {
		notify.type = NOTIFY_FAIL;
		if (!node_essentials_notify_server(&notify)) {
			node_log_error("Failed to notify fail");
		}
		res = false;
	}

	return res;
}

__attribute__((warn_unused_result))
static bool send_next(const routing_table_t* routing, send_t* ret_payload) {
	uint8_t next_addr;
	uint8_t b[MAX_MSG_LEN];
	msg_len_type buf_len;

	next_addr = routing_next_addr(routing, ret_payload->addr_to);
	if (next_addr == UINT8_MAX) {
		node_log_error("Failed to find path in table");
		// TODO: this may happen if node died after path was found
		// start broadcast from here
		return false;
	}

	format_create(REQUEST_SEND, ret_payload, b, &buf_len, REQUEST_SENDER_NODE);
	if (!node_essentials_get_conn_and_send(node_port(next_addr), b, buf_len)) {
		// TODO: this may happen if next addr from routing table is died
		// delete old addr from routing and start broadcast from here
	}

	return true;
}

bool route_direct_handle_delivered(routing_table_t* routing, route_payload_t* route_payload, uint8_t server_addr, app_t apps[APPS_COUNT]) {
	uint8_t b[MAX_MSG_LEN];
	msg_len_type buf_len;
	uint8_t next_addr_to_back;
	bool stop_inverse;
	notify_t notify;

	notify.app_msg_id = route_payload->app_payload.id;

	node_log_debug("Reached the recevier addr %d", route_payload->receiver_addr);

	if (get_inverse_by_id(route_payload->app_payload.id, &stop_inverse)) {
		if (!stop_inverse) {
			set_inverse_by_id(route_payload->app_payload.id, true);
		} else {
			return true;
		}
	} else {
		set_inverse_by_id(route_payload->app_payload.id, true);
	}

	route_payload->time_to_live = TTL;
	route_payload->local_sender_addr = server_addr;

	format_create(REQUEST_ROUTE_INVERSE, route_payload, b, &buf_len, REQUEST_SENDER_NODE);

	next_addr_to_back = routing_next_addr(routing, route_payload->sender_addr);
	if (next_addr_to_back == UINT8_MAX) {
		node_log_error("Failed to get next addr to %d", route_payload->sender_addr);
		return false;
	}

	if (!node_essentials_get_conn_and_send(node_port(next_addr_to_back), b, buf_len)) {
		routing_del(routing, route_payload->sender_addr);
		return false;
	}

	if (node_app_handle_request(apps, &route_payload->app_payload, server_addr)) {
		notify.type = NOTIFY_GOT_MESSAGE;
		if (!node_essentials_notify_server(&notify)) {
			node_log_error("Failed to notify server");
		}
	} else {
		node_log_error("Failed to handle app request");
		notify.type = NOTIFY_FAIL;
		if (!node_essentials_notify_server(&notify)) {
			node_log_error("Failed to notify fail");
		}
	}

	return true;
}

static bool is_id_set(uint16_t id) {
	uint8_t i;

	for (i = 0; i < message_num; i++) {
		if (messages[i].id == id) {
			return true;
		}
	}

	return false;
}

static void set_new_id(uint16_t id) {
	if (message_num == MAX_MESSAGE_DATA) {
		message_num = 0;
		messages[message_num].id = id;
		messages[message_num].stop_inverse = false;
		messages[message_num].was_message = false;
		messages[message_num].unicast_first = false;
		message_num++;
	} else {
		messages[message_num++].id = id;
	}
}

static void init_messages_data(void) {
	uint8_t i;

	if (!init) {
		for (i = 0; i < MAX_MESSAGE_DATA; i++) {
			messages[i].id = 0;
			messages[i].was_message = false;
			messages[i].stop_inverse = false;
			messages[i].unicast_first = false;
		}
		init = true;
	}
}

static bool get_inverse_by_id(uint16_t id, bool* stop_inverse) {
	uint8_t i;

	init_messages_data();

	for (i = 0; i < message_num; i++) {
		if (messages[i].id == id) {
			*stop_inverse = messages[i].stop_inverse;
			return true;
		}
	}

	set_new_id(id);

	return false;
}

static bool get_was_message_by_id(uint16_t id, bool* was_message) {
	uint8_t i;

	init_messages_data();

	for (i = 0; i < message_num; i++) {
		if (messages[i].id == id) {
			*was_message = messages[i].was_message;
			return true;
		}
	}

	set_new_id(id);

	return false;
}

static void set_inverse_by_id(uint16_t id, bool stop_inverse) {
	uint8_t i;

	init_messages_data();

	if (!is_id_set(id)) {
		if (message_num == MAX_MESSAGE_DATA) {
			message_num = 0;
			messages[message_num].id = id;
			messages[message_num].stop_inverse = stop_inverse;
			messages[message_num].was_message = false;
			messages[message_num].unicast_first = false;
			message_num++;
		}
	} else {
		for (i = 0; i < message_num; i++) {
			if (messages[i].id == id) {
				messages[i].stop_inverse = stop_inverse;
			}
		}
	}
}

static void set_was_message_by_id(uint16_t id, bool was_message) {
	uint8_t i;

	init_messages_data();

	if (!is_id_set(id)) {
		if (message_num == MAX_MESSAGE_DATA) {
			message_num = 0;
			messages[message_num].id = id;
			messages[message_num].stop_inverse = false;
			messages[message_num].was_message = was_message;
			messages[message_num].unicast_first = false;
			message_num++;
		}
	} else {
		for (i = 0; i < message_num; i++) {
			if (messages[i].id == id) {
				messages[i].was_message = was_message;
			}
		}
	}
}

static bool get_unicast_status_by_id(uint16_t id, bool* unicast_first) {
	uint8_t i;

	init_messages_data();

	for (i = 0; i < message_num; i++) {
		if (messages[i].id == id) {
			*unicast_first = messages[i].unicast_first;
			return true;
		}
	}

	set_new_id(id);

	return false;
}

static void set_unicast_status_by_id(uint16_t id, bool unicast_first) {
	uint8_t i;

	init_messages_data();

	if (!is_id_set(id)) {
		if (message_num == MAX_MESSAGE_DATA) {
			message_num = 0;
			messages[message_num].id = id;
			messages[message_num].stop_inverse = false;
			messages[message_num].unicast_first = unicast_first;
			messages[message_num].unicast_first = false;
			message_num++;
		}
	} else {
		for (i = 0; i < message_num; i++) {
			if (messages[i].id == id) {
				messages[i].unicast_first = unicast_first;
			}
		}
	}
}

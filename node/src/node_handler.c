#include "node_handler.h"

#include <memory.h>

#include "node_essentials.h"
#include "io.h"
#include "node_app.h"

#include <assert.h>

// if there is multiple clients then replace it with id key array and check it for occurence
// id then should be taken from server in order to be unique between nodes
static bool stop_broadcast = false;
static bool stop_inverse = false;
static bool was_message = false;

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
	struct send_to_node_ret_payload* ret_payload;
	uint8_t b[MAX_MSG_LEN];
	msg_len_type buf_len;
	int32_t node_conn;
	uint8_t next_addr;
	bool res;

	res = true;
	ret_payload = (struct send_to_node_ret_payload*) payload;

	if (ret_payload->addr_to == addr) {
		node_log_warn("Message for node itself");

		if (node_app_handle_request(apps, &ret_payload->app_payload, 0)) {
			if (!node_essentials_notify_server(NOTIFY_GOT_MESSAGE)) {
				node_log_error("Failed to notify server");
			}
		} else {
			node_log_error("Failed to handle app request");
			if (!node_essentials_notify_server(NOTIFY_FAIL)) {
				node_log_error("Failed to notify fail");
			}
		}

		return res;
	}

	node_app_setup_delivery(apps, &ret_payload->app_payload, ret_payload->addr_to);

	format_node_node_create_message(cmd_type, ret_payload, b, &buf_len);

	node_log_debug("Finding route to %d", ret_payload->addr_to);
	next_addr = routing_next_addr(routing, ret_payload->addr_to);
	if (next_addr == UINT8_MAX) {
		node_log_warn("Failed to find route");

		struct node_route_direct_payload route_payload = {
			.local_sender_addr = addr,
			.sender_addr = ret_payload->addr_from,
			.receiver_addr = ret_payload->addr_to,
			.metric = 0,
			.time_to_live = TTL,
			.app_payload = ret_payload->app_payload
			// id is not used yet
		};

		node_essentials_broadcast_route(UINT8_MAX, &route_payload, stop_broadcast);

		return false;
	}

	node_log_info("Sent message (length %d) from %d:%d to %d:%d",
		ret_payload->app_payload.message_len, ret_payload->addr_from, ret_payload->app_payload.addr_from, ret_payload->addr_to, ret_payload->app_payload.addr_to);

	node_conn = node_essentials_get_conn(node_port(next_addr));
	if (node_conn < 0) {
		node_log_error("Failed to create connection with node %d", next_addr);
		res = false;
	} else {
		if (!io_write_all(node_conn, b, buf_len)) {
			node_log_error("Failed to send request");
			res = false;
		}
	}

	return res;
}

void handle_broadcast(struct broadcast_payload* broadcast_payload) {
	node_log_warn("Broadcasting from %d", broadcast_payload->addr_from);
	node_essentials_broadcast(broadcast_payload);
}

void handle_unicast(struct broadcast_payload* broadcast_payload) {
	node_essentials_broadcast(broadcast_payload);
}

__attribute__((warn_unused_result))
static bool send_delivery(const routing_table_t* routing, uint8_t old_from, uint8_t old_to, struct app_payload* old_app_payload, app_t apps[APPS_COUNT]);

__attribute__((warn_unused_result))
static bool send_key_exchange(const routing_table_t* routing, struct app_payload* app_payload, uint8_t receiver_addr, uint8_t sender_addr);

__attribute__((warn_unused_result))
static bool node_handle_app_request(const routing_table_t* routing, app_t apps[APPS_COUNT], struct send_to_node_ret_payload* send_payload);

__attribute__((warn_unused_result))
static bool send_next(const routing_table_t* routing, struct send_to_node_ret_payload* ret_payload);

bool handle_node_send(uint8_t addr, const void* payload, const routing_table_t* routing, app_t apps[APPS_COUNT]) {
	uint8_t addr_to;
	bool res;

	node_log_warn("Send node %d", addr);

	res = true;
	addr_to = ((struct send_to_node_ret_payload*) payload)->addr_to;
	if (addr_to == addr) {
		if (!node_handle_app_request(routing, apps, (struct send_to_node_ret_payload*) payload)) {
			node_log_error("Failed to handle app request");
			res = false;
		}
	} else {
		if (!send_next(routing, (struct send_to_node_ret_payload*) payload)) {
			node_log_error("Failed to send app request next");
			res = false;
		}
	}

	return res;
}

bool route_direct_handle_delivered(routing_table_t* routing, struct node_route_direct_payload* route_payload, uint8_t server_addr, app_t apps[APPS_COUNT]);

bool handle_node_route_direct(routing_table_t* routing, uint8_t server_addr, void* payload, app_t apps[APPS_COUNT]) {
	struct node_route_direct_payload* route_payload;
	uint8_t prev_addr;

	route_payload = (struct node_route_direct_payload*) payload;

	if (was_message) {
		return 0;
	} else {
		was_message = true;
	}

	if (routing_next_addr(routing, route_payload->sender_addr) == UINT8_MAX) {
		routing_set_addr(routing, route_payload->sender_addr, route_payload->local_sender_addr, route_payload->metric);
	} else {
		if (routing_get(routing, route_payload->sender_addr).metric > route_payload->metric) {
			routing_set_addr(routing, route_payload->sender_addr, route_payload->local_sender_addr, route_payload->metric);
		}
	}

	if (route_payload->receiver_addr == server_addr) {
		route_direct_handle_delivered(routing, route_payload, server_addr, apps);
		return true;
	}

	if (route_payload->time_to_live <= 0) {
		node_log_warn("Message died with ttl %d", route_payload->time_to_live);
		return true;
	}

	prev_addr = route_payload->local_sender_addr;
	route_payload->local_sender_addr = server_addr;

	node_essentials_broadcast_route(prev_addr, route_payload, stop_broadcast);

	return true;
}

bool handle_node_route_inverse(routing_table_t* routing, void* payload, uint8_t server_addr) {
	struct node_route_inverse_payload* route_payload;
	int32_t conn_fd;
	uint8_t next_addr;
	uint8_t b[ROUTE_INVERSE_LEN + MSG_LEN];
	msg_len_type buf_len;

	route_payload = (struct node_route_inverse_payload*) payload;

	node_log_warn("Inverse node %d", server_addr);

	if (routing_next_addr(routing, route_payload->receiver_addr) == UINT8_MAX) {
		routing_set_addr(routing, route_payload->receiver_addr, route_payload->local_sender_addr, route_payload->metric);
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
	route_payload->metric++;

	format_node_node_create_message(REQUEST_ROUTE_INVERSE, route_payload, b, &buf_len);

	conn_fd = node_essentials_get_conn(node_port(next_addr));

	if (conn_fd < 0) {
		// probably notify client that message is delivered after all but log error
		// on next send there should be route direct from this node and path will be rebuilt
		node_log_error("Failed to connect to node %d while travel back", next_addr);
		routing_del(routing, route_payload->sender_addr);
		if (!node_essentials_notify_server(NOTIFY_INVERES_COMPLETED)) {
			node_log_error("Failed to notify server");
		}
		return false;
	}

	if (!io_write_all(conn_fd, b, buf_len)) {
		node_log_error("Failed to send route inverse request");
		return false;
	}

	return true;
}

void handle_stop_broadcast(void) {
	stop_broadcast = true;
}

void handle_reset_broadcast_status(void) {
	stop_broadcast = false;
	stop_inverse = false;
	was_message = false;
}

static bool send_delivery(const routing_table_t* routing, uint8_t old_from, uint8_t old_to, struct app_payload* old_app_payload, app_t apps[APPS_COUNT]) {
	uint8_t b[MAX_MSG_LEN];
	msg_len_type buf_len;
	uint8_t next_addr;
	int32_t node_conn;

	struct send_to_node_ret_payload new_send = {
		.addr_to = old_from,
		.addr_from = old_to,
		.app_payload.addr_to = old_app_payload->addr_from,
		.app_payload.addr_from = old_app_payload->addr_to,
		.app_payload.req_type = APP_REQUEST_DELIVERY,
		.app_payload.key = old_app_payload->key,
	};

	node_app_setup_delivery(apps, &new_send.app_payload, new_send.addr_to);

	format_node_node_create_message(REQUEST_SEND, &new_send, b, &buf_len);

	next_addr = routing_next_addr(routing, new_send.addr_to);
	if (next_addr == UINT8_MAX) {
		node_log_error("Failed to find path in table");
		return false;
	}

	node_conn = node_essentials_get_conn(node_port(next_addr));

	if (!io_write_all(node_conn, b, buf_len)) {
		node_log_error("Failed to send request");
		return false;
	}

	node_log_info("Sent message (length %d) from %d:%d to %d:%d",
		new_send.app_payload.message_len, new_send.addr_from, new_send.app_payload.addr_from, new_send.addr_to, new_send.app_payload.addr_to);

	return true;
}

static bool send_key_exchange(const routing_table_t* routing, struct app_payload* app_payload, uint8_t receiver_addr, uint8_t sender_addr) {
	uint8_t tmp;
	uint8_t b[MAX_MSG_LEN - APP_MESSAGE_LEN];
	msg_len_type buf_len;
	uint8_t next_addr;
	int32_t node_conn;

	node_log_warn("Exchanged keys between %d:%d (current) and %d:%d",
		receiver_addr, app_payload->addr_to, sender_addr, app_payload->addr_from);

	tmp = app_payload->addr_from;
	app_payload->addr_from = app_payload->addr_to;
	app_payload->addr_to = tmp;

	struct send_to_node_ret_payload send_payload = {
		.addr_to = sender_addr,
		.addr_from = receiver_addr,
		.app_payload = *app_payload
	};

	format_node_node_create_message(REQUEST_SEND, &send_payload, b, &buf_len);

	next_addr = routing_next_addr(routing, send_payload.addr_to);
	if (next_addr == UINT8_MAX) {
		node_log_error("Failed to find path to %d in table", send_payload.addr_to);

		struct node_route_direct_payload route_payload = {
			.local_sender_addr = receiver_addr,
			.sender_addr = receiver_addr,
			.receiver_addr = sender_addr,
			.metric = 0,
			.time_to_live = TTL,
			.app_payload = *app_payload
			// id is not used yet
		};

		node_log_warn("Sending broadcast");
		node_essentials_broadcast_route(UINT8_MAX, &route_payload, stop_broadcast);

		return true;
	}

	node_conn = node_essentials_get_conn(node_port(next_addr));

	if (!io_write_all(node_conn, b, buf_len)) {
		node_log_error("Failed to send request");
		return false;
	}

	return true;
}

__attribute__((warn_unused_result))
static bool node_handle_app_request(const routing_table_t* routing, app_t apps[APPS_COUNT], struct send_to_node_ret_payload* send_payload) {
	enum app_request app_req;
	bool res;

	app_req = send_payload->app_payload.req_type;
	res = true;

	switch (app_req) {
		case APP_REQUEST_EXCHANGED_KEY:
			node_log_debug("Get exchange key notify. Sending message");
			if (node_app_save_key(apps, &send_payload->app_payload, send_payload->addr_from)) {
				if (!send_delivery(routing, send_payload->addr_from, send_payload->addr_to, &send_payload->app_payload, apps)) {
					node_log_error("Failed to send app message");
					res = false;
				}
			} else {
				node_log_error("Failed to save key");
				res = false;
			}
			break;
		case APP_REQUEST_DELIVERY:
		case APP_REQUEST_BROADCAST:
		case APP_REQUEST_UNICAST:
			if (app_req == APP_REQUEST_UNICAST) {
				if (stop_broadcast) {
					break;
				} else {
					if (!node_essentials_notify_server(NOTIFY_GOT_MESSAGE)) {
						node_log_error("Failed to notify server");
					}
					if (!node_essentials_notify_server(NOTIFY_UNICAST_HANDLED)) {
						node_log_error("Failed to notify server");
					}
				}
			}
			if (node_app_handle_request(apps, &send_payload->app_payload, send_payload->addr_from)) {
				if (!node_essentials_notify_server(NOTIFY_GOT_MESSAGE)) {
					node_log_error("Failed to notify server");
					res = false;
				}
			} else {
				if (!node_essentials_notify_server(NOTIFY_FAIL)) {
					node_log_error("Failed to notify fail");
				}
				res = false;
			}
			break;
		case APP_REQUEST_KEY_EXCHANGE:
			if (node_app_handle_request(apps, &send_payload->app_payload, send_payload->addr_from)) {
				if (send_payload->app_payload.req_type == APP_REQUEST_EXCHANGED_KEY) {
					if (!send_key_exchange(routing, &send_payload->app_payload, send_payload->addr_to, send_payload->addr_from)) {
						node_log_error("Failed to send key exchange response");
						res = false;
					}
				}
			} else {
				res = false;
			}
			break;
		default:
			node_log_error("Unexpected app request");
	}

	return res;
}

__attribute__((warn_unused_result))
static bool send_next(const routing_table_t* routing, struct send_to_node_ret_payload* ret_payload) {
	int32_t node_conn;
	uint8_t next_addr;

	next_addr = routing_next_addr(routing, ret_payload->addr_to);
	if (next_addr == UINT8_MAX) {
		node_log_error("Failed to find path in table");
		// TODO: this may happen if node died after path was found
		// start broadcast from here
		return false;
	}

	node_conn = node_essentials_get_conn(node_port(next_addr));

	if (node_conn < 0) {
		node_log_error("Failed to create connection with node %d", ret_payload->addr_to);
		// TODO: this may happen if next addr from routing table is died
		// delete old addr from routing and start broadcast from here
		return false;
	} else {
		uint8_t b[MAX_MSG_LEN];
		msg_len_type buf_len;

		format_node_node_create_message(REQUEST_SEND, ret_payload, b, &buf_len);

		if (!io_write_all(node_conn, b, buf_len)) {
			node_log_error("Failed to send request");
			return false;
		}
	}

	return true;
}

bool route_direct_handle_delivered(routing_table_t* routing, struct node_route_direct_payload* route_payload, uint8_t server_addr, app_t apps[APPS_COUNT]) {
	uint8_t b[MAX_MSG_LEN];
	msg_len_type buf_len;
	uint8_t next_addr_to_back;
	int32_t conn_fd;

	node_log_debug("Reached the recevier addr %d", route_payload->receiver_addr);

	if (!stop_inverse) {
		// replace with finding id function
		stop_inverse = true;
	} else {
		// somebody alreaady reached this addr with this message id and coming back
		return true;
	}

	route_payload->metric = 0;
	route_payload->time_to_live = TTL;
	route_payload->local_sender_addr = server_addr;

	format_node_node_create_message(REQUEST_ROUTE_INVERSE, route_payload, b, &buf_len);

	next_addr_to_back = routing_next_addr(routing, route_payload->sender_addr);
	if (next_addr_to_back == UINT8_MAX) {
		node_log_error("Failed to get next addr to %d", route_payload->sender_addr);
		return false;
	}

	conn_fd = node_essentials_get_conn(node_port(next_addr_to_back));

	if (conn_fd < 0) {
		node_log_error("Failed to connect to %d node", next_addr_to_back);
		routing_del(routing, route_payload->sender_addr);
		return false;
	}

	if (!io_write_all(conn_fd, b, buf_len)) {
		node_log_error("Failed to send route inverse request");
		return false;
	}

	switch (route_payload->app_payload.req_type) {
		case APP_REQUEST_KEY_EXCHANGE:
			if (node_app_handle_request(apps, &route_payload->app_payload, route_payload->sender_addr)) {
				if (route_payload->app_payload.req_type == APP_REQUEST_EXCHANGED_KEY) {
					if (!send_key_exchange(routing, &route_payload->app_payload, route_payload->receiver_addr, route_payload->sender_addr)) {
						node_log_error("Failed to send key exchange response");
					}
				}
			}
			break;
		case APP_REQUEST_EXCHANGED_KEY:
				node_log_debug("Get exchange key notify. Sending message");
				if (!send_delivery(routing, route_payload->sender_addr, route_payload->receiver_addr, &route_payload->app_payload, apps)) {
					node_log_error("Failed to send app request");
					return false;
				}
			break;
		default:
			node_log_error("Unexpected app request: %d", route_payload->app_payload.req_type);
			break;
	}

	return true;
}

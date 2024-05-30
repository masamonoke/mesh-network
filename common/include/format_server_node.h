#pragma once

#include <stdint.h>
#include <stddef.h>

#include "format.h"

struct node_update_ret_payload {
	int32_t pid;
	uint16_t port;
	uint8_t addr;
};

enum notify_type {
	NOTIFY_GOT_MESSAGE,
	NOTIFY_INVERES_COMPLETED,
};

struct node_notify_payload {
	enum notify_type notify_type;
};

__attribute__((nonnull(3, 4)))
void format_server_node_create_message(enum request req,  const void* payload, uint8_t* buf, uint32_t* len);

__attribute__((nonnull(1, 2, 3)))
void format_server_node_parse_message(enum request* req, void** payload, const void* buf);

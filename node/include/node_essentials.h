#pragma once

#include <stdint.h>

#include "format_node_node.h"
#include "custom_logger.h"
#include "format_server_node.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif

#define node_log_info(format, ...) custom_log_info("Node [" format "]", ##__VA_ARGS__)
#define node_log_error(format, ...) custom_log_error("Node [" format "]", ##__VA_ARGS__)
#define node_log_warn(format, ...) custom_log_warn("Node [" format "]", ##__VA_ARGS__)
#define node_log_debug(format, ...) custom_log_debug("Node [" format "]", ##__VA_ARGS__)

#ifdef __clang__
#pragma clang diagnostic pop
#endif

__attribute__((warn_unused_result))
int32_t node_essentials_get_conn(uint16_t port);

__attribute__((warn_unused_result))
bool node_essentials_notify_server(enum notify_type notify);

__attribute__((nonnull(3)))
void node_essentials_broadcast_route(uint8_t current_addr, uint8_t banned_addr, struct node_route_direct_payload* route_payload, bool stop_broadcast);

void node_essentials_broadcast(uint8_t current_addr, struct broadcast_payload* broadcast_payload, enum request req);

void node_essentials_reset_connections(void);

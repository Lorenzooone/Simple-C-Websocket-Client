// MIT License: Lorenzooone 2026

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>

#include "useful_qualifiers.h"
#include "websocket_simple_client.h"

#if __BIG_ENDIAN__
# define htonll(x) (x)
# define ntohll(x) (x)
#else
# define htonll(x) (((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
# define ntohll(x) (((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))
#endif

//#define PRINT_DATA_IN_INIT_TIMEOUT

#define MAX_SPLITTABLE_SEND_WS_PAYLOAD_SIZE 0x200

#define WS_PING_DEFAULT_SIZE 4

#define WS_MASK_SIZE 4

struct websocket_packet_shared_header {
	uint8_t opcode : 4;
	uint8_t rsv3 : 1;
	uint8_t rsv2 : 1;
	uint8_t rsv1 : 1;
	uint8_t is_fin : 1;
	uint8_t payload_len : 7;
	uint8_t is_masked : 1;
} PACKED;

// For packets with a size 0-125 - no mask
struct websocket_packet_small_header_no_mask {
	struct websocket_packet_shared_header base_websocket_packet_header;
} PACKED;

// For packets with a size 0-125 - with mask
struct websocket_packet_small_header_masked {
	struct websocket_packet_shared_header base_websocket_packet_header;
	uint8_t masking_key[WS_MASK_SIZE];
} PACKED;

// For packets with a size 126 - no mask
struct websocket_packet_medium_header_no_mask {
	struct websocket_packet_shared_header base_websocket_packet_header;
	uint16_t real_payload_len;
} PACKED;

// For packets with a size 126 - with mask
struct websocket_packet_medium_header_masked {
	struct websocket_packet_shared_header base_websocket_packet_header;
	uint16_t real_payload_len;
	uint8_t masking_key[WS_MASK_SIZE];
} PACKED;

// For packets with a size 127 - no mask
struct websocket_packet_big_header_no_mask {
	struct websocket_packet_shared_header base_websocket_packet_header;
	uint64_t real_payload_len;
} PACKED;

// For packets with a size 127 - with mask
struct websocket_packet_big_header_masked {
	struct websocket_packet_shared_header base_websocket_packet_header;
	uint64_t real_payload_len;
	uint8_t masking_key[WS_MASK_SIZE];
} PACKED;

const uint8_t default_mask_bytes[WS_MASK_SIZE] = {0x11, 0x22, 0x33, 0x44};

const uint8_t default_ping_bytes[WS_PING_DEFAULT_SIZE] = {0x13, 0x28, 0x18, 0x58};

static void close_socket_websocket(struct websocket_client_program_data* client_program_data, bool_t do_shutdown);

union websocket_packet_header {
	struct websocket_packet_shared_header shared_header_data;
	struct websocket_packet_small_header_no_mask wsp_shn;
	struct websocket_packet_small_header_masked wsp_shm;
	struct websocket_packet_medium_header_no_mask wsp_mhn;
	struct websocket_packet_medium_header_masked wsp_mhm;
	struct websocket_packet_big_header_no_mask wsp_bhn;
	struct websocket_packet_big_header_masked wsp_bhm;
} PACKED;

static size_t get_ws_packet_header_size_from_header_data(struct websocket_packet_shared_header* partial_header_data) {
	switch(partial_header_data->payload_len) {
		case WS_PAYLOAD_LEN_16_BITS:
			if(partial_header_data->is_masked)
				return sizeof(struct websocket_packet_medium_header_masked);
			return sizeof(struct websocket_packet_medium_header_no_mask);

		case WS_PAYLOAD_LEN_64_BITS:
			// This likely won't ever be a factor on a Nintendo DS...
			if(partial_header_data->is_masked)
				return sizeof(struct websocket_packet_big_header_masked);
			return sizeof(struct websocket_packet_big_header_no_mask);

		default:
			if(partial_header_data->is_masked)
				return sizeof(struct websocket_packet_small_header_masked);
			return sizeof(struct websocket_packet_small_header_no_mask);
	}
}

static size_t get_ws_packet_payload_len_from_header_data(union websocket_packet_header* full_header_data) {
	switch(full_header_data->shared_header_data.payload_len) {
		case WS_PAYLOAD_LEN_16_BITS:
			if(full_header_data->shared_header_data.is_masked)
				return ntohs(full_header_data->wsp_mhm.real_payload_len);
			return ntohs(full_header_data->wsp_mhn.real_payload_len);

		case WS_PAYLOAD_LEN_64_BITS:
			// This likely won't ever be a factor on a Nintendo DS...
			if(full_header_data->shared_header_data.is_masked)
				return (size_t)ntohll(full_header_data->wsp_bhm.real_payload_len);
			return (size_t)ntohll(full_header_data->wsp_bhn.real_payload_len);

		default:
			return full_header_data->shared_header_data.payload_len;
	}
}

static uint8_t* get_ws_packet_payload_mask_from_header_data(union websocket_packet_header* full_header_data) {
	if(!full_header_data->shared_header_data.is_masked)
		return NULL;

	switch(full_header_data->shared_header_data.payload_len) {
		case WS_PAYLOAD_LEN_16_BITS:
			return full_header_data->wsp_mhm.masking_key;

		case WS_PAYLOAD_LEN_64_BITS:
			// This likely won't ever be a factor on a Nintendo DS...
			return full_header_data->wsp_bhm.masking_key;

		default:
			return full_header_data->wsp_shm.masking_key;
	}
}

static size_t get_ws_packet_header_size(size_t payload_size, const uint8_t* mask_key) {
	if (payload_size <= WS_PAYLOAD_LEN_LAST_7_BITS_VALUE) {
		if(mask_key)
			return sizeof(struct websocket_packet_small_header_masked);
		return sizeof(struct websocket_packet_small_header_no_mask);
	}
	if (payload_size < 0x10000) {
		if(mask_key)
			return sizeof(struct websocket_packet_medium_header_masked);
		return sizeof(struct websocket_packet_medium_header_no_mask);
	}
	// This likely won't ever be a factor on a Nintendo DS...
	if(mask_key)
		return sizeof(struct websocket_packet_big_header_masked);
	return sizeof(struct websocket_packet_big_header_no_mask);
}

static void populate_websocket_packet_header(uint8_t* data_ptr, size_t payload_len, const uint8_t* mask_key, enum websocket_opcodes ws_opcode, bool_t is_final) {
	if(data_ptr == NULL)
		return;

	struct websocket_packet_shared_header* header = (struct websocket_packet_shared_header*)data_ptr;
	memset(header, 0, sizeof(struct websocket_packet_shared_header));
	header->is_fin = is_final ? 1 : 0;
	header->opcode = ws_opcode;
	header->is_masked = (mask_key != NULL) ? 1 : 0;
	if(payload_len <= WS_PAYLOAD_LEN_LAST_7_BITS_VALUE)
		header->payload_len = payload_len;
	else if(payload_len < 0x10000)
		header->payload_len = WS_PAYLOAD_LEN_16_BITS;
	else
		header->payload_len = WS_PAYLOAD_LEN_64_BITS;

	// Handle all 6 possible different packet headers...
	switch(header->payload_len) {
		// Medium packet case...
		case WS_PAYLOAD_LEN_16_BITS:
		{
			// No mask
			if(mask_key == NULL) {
				struct websocket_packet_medium_header_no_mask* header_specified = (struct websocket_packet_medium_header_no_mask*)header;
				header_specified->real_payload_len = htons(payload_len);
				break;
			}
			// With mask
			struct websocket_packet_medium_header_masked* header_specified_masked = (struct websocket_packet_medium_header_masked*)header;
				header_specified_masked->real_payload_len = htons(payload_len);
			for(int i = 0; i < WS_MASK_SIZE; i++)
				header_specified_masked->masking_key[i] = mask_key[i];
			break;
		}

		// Big packet case...
		case WS_PAYLOAD_LEN_64_BITS:
		{
			// No mask
			if(mask_key == NULL) {
				struct websocket_packet_big_header_no_mask* header_specified = (struct websocket_packet_big_header_no_mask*)header;
				header_specified->real_payload_len = htonll((uint64_t)payload_len);
				break;
			}
			// With mask
			struct websocket_packet_big_header_masked* header_specified_masked = (struct websocket_packet_big_header_masked*)header;
				header_specified_masked->real_payload_len = htonll((uint64_t)payload_len);
			for(int i = 0; i < WS_MASK_SIZE; i++)
				header_specified_masked->masking_key[i] = mask_key[i];
			break;
		}
		// Small packet case...
		default:
		{
			// No mask
			if(mask_key == NULL)
				break;
			// With mask
			struct websocket_packet_small_header_masked* header_specified_masked = (struct websocket_packet_small_header_masked*)header;
			for(int i = 0; i < WS_MASK_SIZE; i++)
				header_specified_masked->masking_key[i] = mask_key[i];
			break;
		}
	}
}

static bool_t is_ws_packet_control(enum websocket_opcodes ws_opcode) {
	// Proper check would be...
	//return (ws_opcode == WEBSOCKET_OPCODE_CONN_CLOSE) || (ws_opcode == WEBSOCKET_OPCODE_PING) || (ws_opcode == WEBSOCKET_OPCODE_PONG) || (ws_opcode == WEBSOCKET_OPCODE_RSV_CONT_1) || (ws_opcode == WEBSOCKET_OPCODE_RSV_CONT_2) || (ws_opcode == WEBSOCKET_OPCODE_RSV_CONT_3) || (ws_opcode == WEBSOCKET_OPCODE_RSV_CONT_4) || (ws_opcode == WEBSOCKET_OPCODE_RSV_CONT_5);
	// Fast check...
	return ws_opcode >= WEBSOCKET_OPCODE_CONT_START;
}

static bool_t can_ws_packet_be_broken(enum websocket_opcodes ws_opcode) {
	return !is_ws_packet_control(ws_opcode);
}

// Generic send websocket function
static int send_websocket_frame(struct websocket_client_program_data* client_program_data, const uint8_t* payload, size_t payload_len, const uint8_t* mask_key, enum websocket_opcodes ws_opcode) {
	if(client_program_data->is_socket_closed)
		return WS_SOCKET_SEND_FRAME_ERROR_SOCKET;

	// Control payloads have this maximum size...
	if(is_ws_packet_control(ws_opcode) && (payload_len > WS_MAX_CONTROL_PAYLOAD_LEN))
		payload_len = WS_MAX_CONTROL_PAYLOAD_LEN;

	// Control input payload pointer...
	if(payload == NULL)
		payload_len = 0;

	size_t accepted_payload_len = payload_len;

	// Control payloads cannot be broken up...
	if(can_ws_packet_be_broken(ws_opcode) && (accepted_payload_len > MAX_SPLITTABLE_SEND_WS_PAYLOAD_SIZE))
		accepted_payload_len = MAX_SPLITTABLE_SEND_WS_PAYLOAD_SIZE;
	// Use the maximum packet size. Not all packets sent are guaranteed to have this size...
	uint8_t* data_ptr = (uint8_t*)malloc(get_ws_packet_header_size(accepted_payload_len, mask_key) + accepted_payload_len);
	if(data_ptr == NULL)
		return WS_SOCKET_SEND_FRAME_ERROR_NO_MALLOC;

	size_t num_transmissions = 1;
	if(accepted_payload_len != 0)
		num_transmissions = (payload_len + accepted_payload_len - 1) / accepted_payload_len;

	// Packets with size 0 can be used for control messages...
	if(num_transmissions == 0)
		num_transmissions = 1;
	for(size_t i = 0; i < num_transmissions; i++) {
		size_t curr_payload_start = accepted_payload_len * i;
		size_t curr_payload_size = payload_len - curr_payload_start;
		if(curr_payload_size > accepted_payload_len)
			curr_payload_size = accepted_payload_len;

		// Populate the header...
		populate_websocket_packet_header(data_ptr, curr_payload_size, mask_key, (i == 0) ? ws_opcode : WEBSOCKET_OPCODE_FRAME_CONT, i == (num_transmissions - 1));

		// Copy the payload over...
		size_t pos_payload = get_ws_packet_header_size(curr_payload_size, mask_key);

		// Handle cases with no payload
		if(curr_payload_size > 0) {
			memcpy(data_ptr + pos_payload, payload + curr_payload_start, curr_payload_size);

			if(mask_key != NULL) {
				for(size_t j = 0; j < curr_payload_size; j++) {
					data_ptr[pos_payload + j] ^= mask_key[j % WS_MASK_SIZE];
				}
			}
		}

		// Now do the actual send...
		if (write(client_program_data->_socket_id, data_ptr, pos_payload + curr_payload_size) == -1) {
			// In case of error, signal out...
			close_socket_websocket(client_program_data, false);
			free(data_ptr);
			return WS_SOCKET_SEND_FRAME_ERROR_SOCKET;
		}
	}

	free(data_ptr);
	return 0;
}

// Send PING to server
int send_websocket_ping(struct websocket_client_program_data* client_program_data, const uint8_t* data, size_t data_len) {
	if(data_len > WS_MAX_CONTROL_PAYLOAD_LEN)
		data_len = WS_MAX_CONTROL_PAYLOAD_LEN;

	if(data == NULL)
		data_len = 0;

	client_program_data->_last_ping_packet_len = data_len;
	if(data != NULL)
		memcpy(client_program_data->_last_ping_packet, data, client_program_data->_last_ping_packet_len);

	// Send the actual ping
	return send_websocket_frame(client_program_data, client_program_data->_last_ping_packet, data_len, default_mask_bytes, WEBSOCKET_OPCODE_PING);
}

// Send PONG to server
// Sent after receiving a ping, data must match the one present in ping
static int send_websocket_pong(struct websocket_client_program_data* client_program_data, const uint8_t* data, size_t data_len) {
	if(data_len > WS_MAX_CONTROL_PAYLOAD_LEN)
		data_len = WS_MAX_CONTROL_PAYLOAD_LEN;

	return send_websocket_frame(client_program_data, data, data_len, default_mask_bytes, WEBSOCKET_OPCODE_PONG);
}

// Send connection close to server
// Wants reason and specific message to send (can be of length 0)
int send_websocket_conn_close(struct websocket_client_program_data* client_program_data, uint16_t reason, const char* message, size_t message_len) {
	if(message_len > (WS_MAX_CONTROL_PAYLOAD_LEN - sizeof(reason)))
		message_len = WS_MAX_CONTROL_PAYLOAD_LEN - sizeof(reason);

	char* real_message = (char*)malloc(sizeof(reason) + message_len);
	if(real_message == NULL)
		return WS_SOCKET_SEND_FRAME_ERROR_PRE_NO_MALLOC;
	real_message[0] = (reason >> 8) & 0xFF;
	real_message[1] = reason & 0xFF;
	if(message != NULL)
		memcpy(real_message + 2, message, message_len);
	int ret = send_websocket_frame(client_program_data, (const uint8_t*)real_message, sizeof(reason) + message_len, default_mask_bytes, WEBSOCKET_OPCODE_CONN_CLOSE);
	free(real_message);
	return ret;
}

static int internal_send_websocket_conn_close(struct websocket_client_program_data* client_program_data, uint16_t reason, const char* message) {
	size_t len_of_message = 0;
	if(message != NULL)
		len_of_message = strlen(message);
	return send_websocket_conn_close(client_program_data, reason, message, len_of_message);
}

// Send text message to server
int send_websocket_text(struct websocket_client_program_data* client_program_data, const char* message, size_t message_len) {
	return send_websocket_frame(client_program_data, (const uint8_t*)message, message_len, default_mask_bytes, WEBSOCKET_OPCODE_TEXT_FRAME);
}

// Send binary data to server
int send_websocket_binary(struct websocket_client_program_data* client_program_data, const uint8_t* data, size_t data_len) {
	return send_websocket_frame(client_program_data, data, data_len, default_mask_bytes, WEBSOCKET_OPCODE_BINARY_FRAME);
}

static void clear_websocket_client_program_data(struct websocket_client_program_data* client_program_data) {
	client_program_data->_socket_id = 0;
	client_program_data->error_id = 0;
	client_program_data->is_socket_closed = 1;
	client_program_data->_has_websocket_connection_started = false;

	client_program_data->_ping_increment = 0;

	client_program_data->_socket_recv_ring_buffer_read_pos = 0;
	client_program_data->_socket_recv_ring_buffer_write_pos = 0;

	client_program_data->_websocket_last_recv_message_curr_write_pos = 0;
	client_program_data->_websocket_last_recv_message_opcode = WEBSOCKET_OPCODE_FRAME_NONE;
}

static void close_socket_websocket(struct websocket_client_program_data* client_program_data, bool_t do_shutdown) {
	if(client_program_data->is_socket_closed)
		return;

	if (do_shutdown)
		shutdown(client_program_data->_socket_id, 0);

	client_program_data->is_socket_closed = 1;
	close(client_program_data->_socket_id);

	if(client_program_data->on_close_handler != NULL)
		client_program_data->on_close_handler(client_program_data->user_data, client_program_data->error_id);
}

static bool_t receive_data_in_socket(struct websocket_client_program_data* client_program_data) {
	if(client_program_data->is_socket_closed)
		return false;

	const size_t max_chunk_size = 256;
	size_t read_size = SOCKET_BUFFER_SIZE - (client_program_data->_socket_recv_ring_buffer_write_pos % SOCKET_BUFFER_SIZE);
	size_t available_size_in_ring_buffer = ((client_program_data->_socket_recv_ring_buffer_read_pos + SOCKET_BUFFER_SIZE) - client_program_data->_socket_recv_ring_buffer_write_pos) % SOCKET_BUFFER_SIZE;
	if(available_size_in_ring_buffer == 0)
		available_size_in_ring_buffer = SOCKET_BUFFER_SIZE;
	available_size_in_ring_buffer -= 1;
	if(read_size > max_chunk_size)
		read_size = max_chunk_size;
	if(read_size > available_size_in_ring_buffer)
		read_size = available_size_in_ring_buffer;

	if(read_size == 0) {
		// The buffer is full and it's not been emptied...
		// Fail now.
		if(client_program_data->_has_websocket_connection_started)
			internal_send_websocket_conn_close(client_program_data, 1009, NULL);
		close_socket_websocket(client_program_data, true);
		return false;
	}

	int recvd_len = read(client_program_data->_socket_id, &(client_program_data->_socket_recv_ring_buffer[client_program_data->_socket_recv_ring_buffer_write_pos]), read_size);

	if (recvd_len > 0)
	{
		// Some data has been received
		client_program_data->_socket_recv_ring_buffer_write_pos = (client_program_data->_socket_recv_ring_buffer_write_pos + recvd_len) % SOCKET_BUFFER_SIZE;
		return true;
	}
	else if (recvd_len == 0)
	{
		// The socket has been closed.
		close_socket_websocket(client_program_data, false);
		return false;
	}
	return false;
}

// Get strstr pos in ring buffer...
static int32_t get_strstr_pos_in_ring_buffer(uint8_t* ring_buffer, size_t read_start_pos, size_t ring_buffer_size, const char* str_to_search) {
	size_t len_of_str_to_search = strlen(str_to_search);

	for(size_t i = 0; i < ring_buffer_size; i++) {
		bool_t is_same = true;
		for(size_t j = 0; j < len_of_str_to_search; j++) {
			size_t ring_buffer_pos = (read_start_pos + i + j) % ring_buffer_size;
			if(ring_buffer[ring_buffer_pos] == '\0')
				return -1;
			if(ring_buffer[ring_buffer_pos] != str_to_search[j]) {
				is_same = false;
				break;
			}
		}
		if(is_same)
			return (int32_t)((read_start_pos + i) % ring_buffer_size);
	}

	return -1;
}

void init_websocket_client_program_data(struct websocket_client_program_data* client_program_data) {
	client_program_data->timeout_no_answer_ms = 60000;
	client_program_data->is_socket_closed = 1;
	client_program_data->user_data = NULL;
	client_program_data->text_message_handler = NULL;
	client_program_data->binary_message_handler = NULL;
	client_program_data->on_close_handler = NULL;
	client_program_data->close_message_handler = NULL;
}

// This function starts the websocket connection to a server.
int init_websocket_connection(struct websocket_client_program_data* client_program_data, const char *url, const char *port, const char *path, sleep_function_ms_t sleep_function_ms_ptr, get_curr_time_ms_t get_curr_time_ms_ptr) {
	char buf[1024];

	if(client_program_data == NULL)
		return INIT_WEBSOCKET_ERROR_NULL_PTR_PROVIDED;
		
	if(get_curr_time_ms_ptr == NULL)
		return INIT_WEBSOCKET_ERROR_NO_TIME_FUNCTION;
	client_program_data->get_curr_time_ms = get_curr_time_ms_ptr;

	clear_websocket_client_program_data(client_program_data);

	struct addrinfo hint;

	if((port == NULL) || (port[0] == '\0'))
		port = "80";

	hint.ai_flags = AI_CANONNAME;
	hint.ai_family = AF_UNSPEC;
	hint.ai_socktype = SOCK_STREAM; // TCP
	hint.ai_protocol = 0;
	hint.ai_addrlen = 0;
	hint.ai_canonname = NULL;
	hint.ai_addr = NULL;
	hint.ai_next = NULL;

	struct addrinfo *result, *rp;

	int err = getaddrinfo(url, port, &hint, &result);
	if (err != 0)
	{
		fprintf(stderr, "getaddrinfo(): %d\n", err);
		return INIT_WEBSOCKET_ERROR_GETADDRINFO;
	}

	struct addrinfo *found_rp = NULL;

	for (rp = result; rp != NULL; rp = rp->ai_next)
	{
		if (rp->ai_family == AF_INET)
		{
			// This should never happen if we have asked for IPv6 addresses
			if ((hint.ai_family == AF_INET) || (hint.ai_family == AF_UNSPEC))
			{
				found_rp = rp;
				break;
			}
		}
		else if (rp->ai_family == AF_INET6)
		{
			if ((hint.ai_family == AF_INET6) || (hint.ai_family == AF_UNSPEC))
			{
				found_rp = rp;
				break;
			}
		}
	}

	if (found_rp == NULL)
	{
		fprintf(stderr, "Can't find IP info!\n");
		freeaddrinfo(result);
		return INIT_WEBSOCKET_ERROR_NO_IP;
	}

	int sfd = socket(found_rp->ai_family, found_rp->ai_socktype, found_rp->ai_protocol);
	if (sfd == -1)
	{
		perror("socket");
		freeaddrinfo(result);
		return INIT_WEBSOCKET_ERROR_SOCKET_OPEN;
	}

	if (connect(sfd, found_rp->ai_addr, found_rp->ai_addrlen) == -1)
	{
		perror("connect");
		close(sfd);
		freeaddrinfo(result);
		return INIT_WEBSOCKET_ERROR_CONNECT;
	}

	freeaddrinfo(result);

	snprintf(buf, sizeof(buf),
		"GET %s HTTP/1.1\r\n"
		"Host: %s:%s\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"User-Agent: Nintendo DS\r\n\r\n",
		path, url, port);

	// send our request
	if (write(sfd, buf, strlen(buf)) == -1)
	{
		perror("write()");
		close(sfd);
		return INIT_WEBSOCKET_ERROR_WRITE_UPGRADE;
	}

	// Put the socket in non-blocking mode:
	int opt = 1;
	int rc = ioctl(sfd, FIONBIO, (char *)&opt);
	if (rc < 0)
	{
		perror("ioctl()");
		close(sfd);
		return INIT_WEBSOCKET_ERROR_SOCKET_NONBLOCKING;
	}

	client_program_data->_last_recv_message_ms = client_program_data->get_curr_time_ms();
	client_program_data->_last_sent_ping_ms = client_program_data->get_curr_time_ms();

	// Prepare in buffer
	client_program_data->_socket_recv_ring_buffer[client_program_data->_socket_recv_ring_buffer_write_pos] = '\0';

	// Used to know when to stop receiving data
	int32_t upgrade_start = -1;
	int32_t upgrade_end = -1;

	// Store the socket...
	client_program_data->_socket_id = sfd;
	client_program_data->is_socket_closed = 0;

	// Get time...
	uint32_t curr_time_since_last_recv = client_program_data->get_curr_time_ms() - client_program_data->_last_recv_message_ms;
	bool_t success = false;

	// Look for answer that is the protocol upgrade
	while (curr_time_since_last_recv < client_program_data->timeout_no_answer_ms)
	{
		bool_t has_received_new_data = receive_data_in_socket(client_program_data);
		curr_time_since_last_recv = client_program_data->get_curr_time_ms() - client_program_data->_last_recv_message_ms;

		if(has_received_new_data)
			client_program_data->_socket_recv_ring_buffer[client_program_data->_socket_recv_ring_buffer_write_pos] = '\0'; // NULL-terminate

		if (client_program_data->is_socket_closed)
		{
			// The socket has been closed.
			fprintf(stderr, "Other side closed connection!\n");
			return INIT_WEBSOCKET_ERROR_READ_CONN_CLOSED;
		}


		// Try to determine the length of the response
		if (has_received_new_data && (upgrade_start == -1))
		{
			const char *searchstr[] = {
										"HTTP/1.1 101 Switching Protocols\r\n",
										"Upgrade: websocket\r\n",
										"Connection: Upgrade\r\n",
										"Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
										};
			bool_t has_all = true;
			const size_t num_strings = sizeof(searchstr) / sizeof(searchstr[0]);
			int32_t pos_data[num_strings];
			int32_t max_pos = 0;
			size_t max_pos_index = 0;

			const char *searchstr_alt[] = {
											NULL,
											NULL,
											NULL,
											"Sec-Websocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n",
											};

			for(size_t i = 0; i < num_strings; i++) {
				pos_data[i] = get_strstr_pos_in_ring_buffer(client_program_data->_socket_recv_ring_buffer, client_program_data->_socket_recv_ring_buffer_read_pos, SOCKET_BUFFER_SIZE, searchstr[i]);
				if(pos_data[i] < 0) {
					if(searchstr_alt[i] != NULL)
						pos_data[i] = get_strstr_pos_in_ring_buffer(client_program_data->_socket_recv_ring_buffer, client_program_data->_socket_recv_ring_buffer_read_pos, SOCKET_BUFFER_SIZE, searchstr_alt[i]);
					if(pos_data[i] < 0) {
						has_all = false;
						break;
					}
				}
				if(pos_data[i] > max_pos) {
					max_pos = pos_data[i];
					max_pos_index = i;
				}
			}
			if(has_all)
				upgrade_start = (max_pos + strlen(searchstr[max_pos_index])) % SOCKET_BUFFER_SIZE;
		}

		// Try to determine the end of the content
		if (has_received_new_data && (upgrade_start != -1))
		{
			client_program_data->_socket_recv_ring_buffer_read_pos = upgrade_start;
			// The start of the content comes after an empty line
			const char *searchstr = "\r\n\r\n";
			upgrade_end = get_strstr_pos_in_ring_buffer(client_program_data->_socket_recv_ring_buffer, client_program_data->_socket_recv_ring_buffer_read_pos, SOCKET_BUFFER_SIZE, searchstr);
			if(upgrade_end != -1)
				upgrade_end = (upgrade_end + strlen(searchstr)) % SOCKET_BUFFER_SIZE;
		}

		// If we know the end of the message, check if we have reached it
		if (upgrade_end != -1)
		{
			client_program_data->_socket_recv_ring_buffer_read_pos = upgrade_end;
			success = true;
			break;
		}

		// When using non-blocking sockets we need to give the other threads a
		// chance to use the CPU.
		if((!has_received_new_data) && (sleep_function_ms_ptr != NULL))
			sleep_function_ms_ptr(4);
	}

	if(!success) {
		// Too much time elapsed...
		close_socket_websocket(client_program_data, true);
		#ifdef PRINT_DATA_IN_INIT_TIMEOUT
		printf("%s\n", client_program_data->_socket_recv_ring_buffer);
		#endif
		fprintf(stderr, "Too much time elapsed!\n");
		return INIT_WEBSOCKET_ERROR_READ_CONN_TIMEOUT;
	}

	client_program_data->_last_recv_message_ms = client_program_data->get_curr_time_ms();
	client_program_data->_last_sent_ping_ms = client_program_data->get_curr_time_ms();
	client_program_data->_has_websocket_connection_started = true;
	return 0;
}

static void copy_bytes_from_ring_buffer_to_buffer(void* dst, const uint8_t* src, size_t len, size_t start_pos, size_t ring_buffer_size) {
	start_pos %= ring_buffer_size;
	size_t bytes_before_end = ring_buffer_size - start_pos;

	if(len <= bytes_before_end) {
		memcpy(dst, src + start_pos, len);
		return;
	}

	memcpy(dst, src + start_pos, bytes_before_end);
	memcpy(dst + bytes_before_end, src, len - bytes_before_end);
}

static struct websocket_packet_shared_header copy_bytes_to_websocket_packet_shared_header(struct websocket_client_program_data* client_program_data) {
	struct websocket_packet_shared_header header_data;
	copy_bytes_from_ring_buffer_to_buffer(&header_data, client_program_data->_socket_recv_ring_buffer, sizeof(struct websocket_packet_shared_header), client_program_data->_socket_recv_ring_buffer_read_pos, SOCKET_BUFFER_SIZE);
	return header_data;
}

static union websocket_packet_header copy_bytes_to_websocket_packet_full_header(struct websocket_client_program_data* client_program_data, size_t real_header_size) {
	union websocket_packet_header full_header_data;
	copy_bytes_from_ring_buffer_to_buffer(&full_header_data, client_program_data->_socket_recv_ring_buffer, real_header_size, client_program_data->_socket_recv_ring_buffer_read_pos, SOCKET_BUFFER_SIZE);
	return full_header_data;
}

static void extract_payload_from_websocket_packet(struct websocket_client_program_data* client_program_data, size_t real_header_size, uint8_t* dst, size_t payload_size, uint8_t* mask_bytes) {
	copy_bytes_from_ring_buffer_to_buffer(dst, client_program_data->_socket_recv_ring_buffer, payload_size, client_program_data->_socket_recv_ring_buffer_read_pos + real_header_size, SOCKET_BUFFER_SIZE);
	if(mask_bytes != NULL) {
		for(size_t i = 0; i < payload_size; i++)
			dst[i] ^= mask_bytes[i % WS_MASK_SIZE];
	}
}

static void process_websocket_data(struct websocket_client_program_data* client_program_data) {
	size_t available_bytes = ((client_program_data->_socket_recv_ring_buffer_write_pos + SOCKET_BUFFER_SIZE) - client_program_data->_socket_recv_ring_buffer_read_pos) % SOCKET_BUFFER_SIZE;

	if(available_bytes < sizeof(struct websocket_packet_shared_header))
		return;

	struct websocket_packet_shared_header partial_header_data = copy_bytes_to_websocket_packet_shared_header(client_program_data);
	//printf("%02x %02x\n", client_program_data->_socket_recv_ring_buffer[client_program_data->_socket_recv_ring_buffer_read_pos], client_program_data->_socket_recv_ring_buffer[client_program_data->_socket_recv_ring_buffer_read_pos + 1]);

	// Instant sanity check about the packet...
	if((is_ws_packet_control(partial_header_data.opcode)) && (!partial_header_data.is_fin)) {
		internal_send_websocket_conn_close(client_program_data, 1002, "Control packet without is_fin");
		close_socket_websocket(client_program_data, true);
		return;
	}

	size_t real_header_size = get_ws_packet_header_size_from_header_data(&partial_header_data);

	if(available_bytes < real_header_size)
		return;

	union websocket_packet_header full_header_data = copy_bytes_to_websocket_packet_full_header(client_program_data, real_header_size);
	size_t payload_size = get_ws_packet_payload_len_from_header_data(&full_header_data);
	size_t full_size = real_header_size + payload_size;

	if(is_ws_packet_control(partial_header_data.opcode) && (payload_size > WS_MAX_CONTROL_PAYLOAD_LEN)) {
		internal_send_websocket_conn_close(client_program_data, 1002, "Control packet too big");
		close_socket_websocket(client_program_data, true);
		return;
	}

	if(available_bytes < full_size)
		return;

	uint8_t* mask = get_ws_packet_payload_mask_from_header_data(&full_header_data);

	uint8_t* payload_data_out = NULL;
	if(is_ws_packet_control(partial_header_data.opcode)) {
		if(payload_size > 0) {
			payload_data_out = malloc(payload_size);
			if(payload_data_out == NULL) {
				internal_send_websocket_conn_close(client_program_data, 1011, NULL);
				close_socket_websocket(client_program_data, true);
				client_program_data->_socket_recv_ring_buffer_read_pos = (client_program_data->_socket_recv_ring_buffer_read_pos + full_size) % SOCKET_BUFFER_SIZE;
				return;
			}
			extract_payload_from_websocket_packet(client_program_data, real_header_size, payload_data_out, payload_size, mask);
		}
	}

	int ret = 0;
	bool_t update_last_recv = true;

	switch(partial_header_data.opcode) {
		case WEBSOCKET_OPCODE_PING:
			//printf("PING\n");
			ret = send_websocket_pong(client_program_data, payload_data_out, payload_size);
			break;
		case WEBSOCKET_OPCODE_PONG:
			//printf("RET PONG\n");
			if(payload_size != client_program_data->_last_ping_packet_len) {
				update_last_recv = false;
				break;
			}
			if(payload_size == 0)
				break;
			if(memcmp(payload_data_out, client_program_data->_last_ping_packet, payload_size) != 0)
				update_last_recv = false;
			break;
		case WEBSOCKET_OPCODE_CONN_CLOSE:
			close_socket_websocket(client_program_data, true);
			if(client_program_data->close_message_handler != NULL)
				client_program_data->close_message_handler(client_program_data->user_data, payload_data_out[1] | (payload_data_out[0] << 8), payload_data_out + sizeof(uint16_t), payload_size - sizeof(uint16_t));
			break;
		case WEBSOCKET_OPCODE_FRAME_CONT:
			// Check that previous message was fine...
			if(client_program_data->_websocket_last_recv_message_opcode == WEBSOCKET_OPCODE_FRAME_NONE) {
				ret = internal_send_websocket_conn_close(client_program_data, 1002, "Missing message start");
				close_socket_websocket(client_program_data, ret == 0);
				break;
			}

			// Check size before copying
			if((client_program_data->_websocket_last_recv_message_curr_write_pos + payload_size) > WEBSOCKET_SINGLE_MESSAGE_MAX_SIZE) {
				ret = internal_send_websocket_conn_close(client_program_data, 1009, "Message too big");
				close_socket_websocket(client_program_data, ret == 0);
				break;
			}
			extract_payload_from_websocket_packet(client_program_data, real_header_size, client_program_data->_websocket_last_recv_message + client_program_data->_websocket_last_recv_message_curr_write_pos, payload_size, mask);
			client_program_data->_websocket_last_recv_message_curr_write_pos += payload_size;
			break;
		case WEBSOCKET_OPCODE_TEXT_FRAME:
		case WEBSOCKET_OPCODE_BINARY_FRAME:
			// Check that previous message was fine...
			if(client_program_data->_websocket_last_recv_message_opcode != WEBSOCKET_OPCODE_FRAME_NONE) {
				ret = internal_send_websocket_conn_close(client_program_data, 1002, "Missing message end");
				close_socket_websocket(client_program_data, ret == 0);
				break;
			}

			// Init data
			client_program_data->_websocket_last_recv_message_curr_write_pos = 0;
			client_program_data->_websocket_last_recv_message_opcode = partial_header_data.opcode;

			// Check size before copying
			if((client_program_data->_websocket_last_recv_message_curr_write_pos + payload_size) > WEBSOCKET_SINGLE_MESSAGE_MAX_SIZE) {
				ret = internal_send_websocket_conn_close(client_program_data, 1009, "Message too big");
				close_socket_websocket(client_program_data, ret == 0);
				break;
			}
			extract_payload_from_websocket_packet(client_program_data, real_header_size, client_program_data->_websocket_last_recv_message + client_program_data->_websocket_last_recv_message_curr_write_pos, payload_size, mask);
			client_program_data->_websocket_last_recv_message_curr_write_pos += payload_size;
			break;
		default:
			ret = internal_send_websocket_conn_close(client_program_data, 1003, "Opcode not implemented");
			close_socket_websocket(client_program_data, ret == 0);
			break;
	}

	if(ret != 0)
		close_socket_websocket(client_program_data, false);

	if((!client_program_data->is_socket_closed) && (!is_ws_packet_control(partial_header_data.opcode)) && partial_header_data.is_fin) {
		// Terminate text, in case there is that...
		client_program_data->_websocket_last_recv_message[client_program_data->_websocket_last_recv_message_curr_write_pos] = '\0';

		if((client_program_data->_websocket_last_recv_message_opcode == WEBSOCKET_OPCODE_TEXT_FRAME) && (client_program_data->text_message_handler != NULL))
			client_program_data->text_message_handler(client_program_data->user_data, client_program_data->_websocket_last_recv_message, client_program_data->_websocket_last_recv_message_curr_write_pos);

		if((client_program_data->_websocket_last_recv_message_opcode == WEBSOCKET_OPCODE_BINARY_FRAME) && (client_program_data->binary_message_handler != NULL))
			client_program_data->binary_message_handler(client_program_data->user_data, client_program_data->_websocket_last_recv_message, client_program_data->_websocket_last_recv_message_curr_write_pos);

		// Signal that new data may be received...
		client_program_data->_websocket_last_recv_message_opcode = WEBSOCKET_OPCODE_FRAME_NONE;
	}

	if(update_last_recv)
		client_program_data->_last_recv_message_ms = client_program_data->get_curr_time_ms();

	if(payload_data_out != NULL)
		free(payload_data_out);

	// Free data from the socket buffer...
	client_program_data->_socket_recv_ring_buffer_read_pos = (client_program_data->_socket_recv_ring_buffer_read_pos + full_size) % SOCKET_BUFFER_SIZE;
}

// Call periodically, to ensure data is processed and
// the connection is not dropped...
// It will call socket recv in a nonblocking manner...
// It will also call socket send in certain situations...
void websocket_recv_handler(struct websocket_client_program_data* client_program_data) {
	if(client_program_data->is_socket_closed)
		return;
	

	bool_t call_recv = true;

	while(call_recv) {
		bool_t has_received_new_data = receive_data_in_socket(client_program_data);

		if(has_received_new_data)
			process_websocket_data(client_program_data);

		call_recv = has_received_new_data && (!client_program_data->is_socket_closed);
	}

	// Get time...
	uint32_t curr_time_since_last_recv = client_program_data->get_curr_time_ms() - client_program_data->_last_recv_message_ms;
	uint32_t curr_time_since_last_ping = client_program_data->get_curr_time_ms() - client_program_data->_last_sent_ping_ms;

	uint32_t time_limit_before_ping_ms = client_program_data->timeout_no_answer_ms / 4;

	if((curr_time_since_last_recv >= time_limit_before_ping_ms) && (curr_time_since_last_ping >= time_limit_before_ping_ms)) {

		// Prepare ping data
		uint8_t ping_bytes[WS_PING_DEFAULT_SIZE];
		for(size_t i = 0; i < WS_PING_DEFAULT_SIZE; i++)
			ping_bytes[i] = default_ping_bytes[i] + client_program_data->_ping_increment;

		// Send a ping to ensure connection is still ongoing
		send_websocket_ping(client_program_data, ping_bytes, WS_PING_DEFAULT_SIZE);
		client_program_data->_last_sent_ping_ms = client_program_data->get_curr_time_ms();
	}

	if(curr_time_since_last_recv >= client_program_data->timeout_no_answer_ms) {
		internal_send_websocket_conn_close(client_program_data, 1006, "Timeout");
		close_socket_websocket(client_program_data, true);
	}
		
}

void close_websocket(struct websocket_client_program_data* client_program_data) {
	internal_send_websocket_conn_close(client_program_data, 1000, "Ok");
	close_socket_websocket(client_program_data, true);
}

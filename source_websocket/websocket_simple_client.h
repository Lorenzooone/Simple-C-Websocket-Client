#ifndef __WEBSOCKET_SIMPLE_CLIENT_H
#define __WEBSOCKET_SIMPLE_CLIENT_H

// MIT License: Lorenzooone 2026

#include <stdbool.h>

// Maximum accepted packet size...
// The Nintendo DS memory is limited, so we have to stop at a certain point...
#define SOCKET_BUFFER_SIZE 16384

// Maximum accepted size of a single websocket packet, after conts are merged.
#define WEBSOCKET_SINGLE_MESSAGE_MAX_SIZE 32768

#define WS_PAYLOAD_LEN_LAST_7_BITS_VALUE 125
#define WS_PAYLOAD_LEN_16_BITS 126
#define WS_PAYLOAD_LEN_64_BITS 127

#define WS_MAX_CONTROL_PAYLOAD_LEN WS_PAYLOAD_LEN_LAST_7_BITS_VALUE

// Returned by the send_websocket_* functions
#define WS_SOCKET_SEND_FRAME_ERROR_NO_MALLOC -1
#define WS_SOCKET_SEND_FRAME_ERROR_SOCKET -2
#define WS_SOCKET_SEND_FRAME_ERROR_PRE_NO_MALLOC -3

// Returned by init_websocket_connection
#define INIT_WEBSOCKET_ERROR_GETADDRINFO -1
#define INIT_WEBSOCKET_ERROR_NO_IP -2
#define INIT_WEBSOCKET_ERROR_SOCKET_OPEN -3
#define INIT_WEBSOCKET_ERROR_CONNECT -4
#define INIT_WEBSOCKET_ERROR_WRITE_UPGRADE -5
#define INIT_WEBSOCKET_ERROR_SOCKET_NONBLOCKING -6
#define INIT_WEBSOCKET_ERROR_READ_UPGRADE_RESPONSE -7
#define INIT_WEBSOCKET_ERROR_NOT_UPGRADE_RESPONSE -8
#define INIT_WEBSOCKET_ERROR_READ_CONN_CLOSED -9
#define INIT_WEBSOCKET_ERROR_READ_CONN_TIMEOUT -10
#define INIT_WEBSOCKET_ERROR_NO_TIME_FUNCTION -11
#define INIT_WEBSOCKET_ERROR_NULL_PTR_PROVIDED -12

// Use a define because you never know...
// If you run this from VRAM, bool may not play nicely...
#define bool_t bool

enum websocket_opcodes {
	WEBSOCKET_OPCODE_FRAME_NONE = -1,
	WEBSOCKET_OPCODE_FRAME_CONT = 0,
	WEBSOCKET_OPCODE_TEXT_FRAME = 1,
	WEBSOCKET_OPCODE_BINARY_FRAME = 2,
	WEBSOCKET_OPCODE_RSV_NONCONT_1 = 3,
	WEBSOCKET_OPCODE_RSV_NONCONT_2 = 4,
	WEBSOCKET_OPCODE_RSV_NONCONT_3 = 5,
	WEBSOCKET_OPCODE_RSV_NONCONT_4 = 6,
	WEBSOCKET_OPCODE_RSV_NONCONT_5 = 7,
	WEBSOCKET_OPCODE_CONN_CLOSE = 8,
	WEBSOCKET_OPCODE_PING = 9,
	WEBSOCKET_OPCODE_PONG = 0xA,
	WEBSOCKET_OPCODE_RSV_CONT_1 = 0xB,
	WEBSOCKET_OPCODE_RSV_CONT_2 = 0xC,
	WEBSOCKET_OPCODE_RSV_CONT_3 = 0xD,
	WEBSOCKET_OPCODE_RSV_CONT_4 = 0xE,
	WEBSOCKET_OPCODE_RSV_CONT_5 = 0xF,
	WEBSOCKET_OPCODE_CONT_START = 8,
};

typedef void (*websocket_text_message_handler)(void* user_data, uint8_t* message_data, size_t message_len);
typedef void (*websocket_binary_message_handler)(void* user_data, uint8_t* message_data, size_t message_len);
typedef void (*websocket_on_close_handler)(void* user_data, int error_id);
typedef void (*websocket_close_message_handler)(void* user_data, uint16_t reason_id, uint8_t* message_data, size_t message_len);
typedef void (*sleep_function_ms_t)(int wanted_ms);
typedef uint32_t (*get_curr_time_ms_t)();

struct websocket_client_program_data {
	//===================================================================
	// public
	//===================================================================

	int error_id;
	bool_t is_socket_closed;
	uint32_t timeout_no_answer_ms;

	// Handle incoming messages, but only when they are ready...
	// User data passed back to the callbacks
	void* user_data;
	// User-provided handler for an incoming text message
	websocket_text_message_handler text_message_handler;
	// User-provided handler for an incoming binary message
	websocket_binary_message_handler binary_message_handler;
	// User-provided handler to run when the socket is closed.
	// This can be due to the server closing the connection
	// or due to an error
	websocket_on_close_handler on_close_handler;
	// User-provided handler for an incoming close request
	// from the server
	websocket_close_message_handler close_message_handler;
	// User-provided function to get the current time in milliseconds
	get_curr_time_ms_t get_curr_time_ms;

	//===================================================================
	// private
	//===================================================================

	// Fields that start with a _ should not be interacted with by external code.
	// All this data is kept in the struct to allow multiple connections
	// at the same time, instead of using static data

	int _socket_id;
	bool_t _has_websocket_connection_started;

	uint32_t _last_recv_message_ms;
	uint32_t _last_sent_ping_ms;
	uint8_t _ping_increment;

	// Receiving socket for http/websocket data...
	size_t _socket_recv_ring_buffer_read_pos;
	size_t _socket_recv_ring_buffer_write_pos;
	uint8_t _socket_recv_ring_buffer[SOCKET_BUFFER_SIZE];

	// This is for regular messages...
	// Control messages are handled transparently from the user...
	size_t _websocket_last_recv_message_curr_write_pos;
	enum websocket_opcodes _websocket_last_recv_message_opcode;
	uint8_t _websocket_last_recv_message[WEBSOCKET_SINGLE_MESSAGE_MAX_SIZE];

	// Special data for packets...
	uint8_t _last_ping_packet[WS_MAX_CONTROL_PAYLOAD_LEN];
	size_t _last_ping_packet_len;
};

// Send PING to server.
// Returns 0 in case of success.
// Returns WS_SOCKET_SEND_FRAME_ERROR_* in case of error.
int send_websocket_ping(struct websocket_client_program_data* client_program_data, const uint8_t* data, size_t data_len);

// Send connection close to server.
// Wants reason and specific message to send (can be of length 0)
// Returns 0 in case of success.
// Returns WS_SOCKET_SEND_FRAME_ERROR_* in case of error.
int send_websocket_conn_close(struct websocket_client_program_data* client_program_data, uint16_t reason, const char* message, size_t message_len);

// Send text message to server.
// Returns 0 in case of success.
// Returns WS_SOCKET_SEND_FRAME_ERROR_* in case of error.
int send_websocket_text(struct websocket_client_program_data* client_program_data, const char* message, size_t message_len);

// Send binary data to server.
// Returns 0 in case of success.
// Returns WS_SOCKET_SEND_FRAME_ERROR_* in case of error.
int send_websocket_binary(struct websocket_client_program_data* client_program_data, const uint8_t* data, size_t data_len);

// Inits the websocket client program data, must be called after allocation
void init_websocket_client_program_data(struct websocket_client_program_data* client_program_data);

// This function starts the websocket connection to a server.
// Returns 0 in case of success.
// Returns INIT_WEBSOCKET_ERROR_* in case of error.
int init_websocket_connection(struct websocket_client_program_data* client_program_data, const char *url, const char *port, const char *path, sleep_function_ms_t sleep_function_ms_ptr, get_curr_time_ms_t get_curr_time_ms_ptr);

// Call periodically from the outside, to ensure data is processed and
// the connection is not dropped...
// It will call socket recv in a nonblocking manner...
// It will also call socket send in certain situations...
void websocket_recv_handler(struct websocket_client_program_data* client_program_data);

// Called to close the websocket from code
void close_websocket(struct websocket_client_program_data* client_program_data);

#endif

// SPDX-License-Identifier: CC0-1.0
//
// SPDX-FileContributor: Lorenzooone 2026

// This example shows how to connect to a websocket and exchange data.

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include "websocket_simple_client.h"

#define CONNECTION_TARGET_IP "192.168.0.100"
#define CONNECTION_TARGET_PORT "11111"
#define CONNECTION_TARGET_PATH "/poolda"

static uint32_t get_curr_ms() {
    struct timeval tv;

    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

void sleep_function_ms(int wanted_ms) {
	usleep(wanted_ms * 1000.0);
}

void* websocket_recv_thread(void *arg) {
    struct websocket_client_program_data* client_program_data = (struct websocket_client_program_data*)arg;

    while (!client_program_data->is_socket_closed) {
    	sleep_function_ms(4);
        websocket_recv_handler(client_program_data);
    }
    return arg;
}

void init_websocket_recv_thread(struct websocket_client_program_data* client_program_data, pthread_t* thread_index) {
	if((*thread_index) != -1)
		return;

	pthread_create(thread_index, 0, websocket_recv_thread, client_program_data);
}

void stop_websocket_recv_thread(struct websocket_client_program_data* client_program_data, pthread_t* thread_index) {
	if((*thread_index) == -1)
		return;

	pthread_join(*thread_index, NULL);
	*thread_index = -1;
}

void on_text_data_from_ws(void* user_data, uint8_t* message_data, size_t message_len) {
	printf("TEXT: %.*s\n", message_len, message_data);
}

void on_binary_data_from_ws(void* user_data, uint8_t* message_data, size_t message_len) {
	printf("BIN: ");
	for(size_t i = 0; i < message_len; i++) {
		printf("%02X ", message_data[i]);
		if(((i % 8) == 7) && (i != (message_len - 1)))
			printf("\n     ");
	}
	printf("\n");
}

int main(int argc, char *argv[])
{

	pthread_t thread_index = -1;
	struct websocket_client_program_data* client_program_data = malloc(sizeof(struct websocket_client_program_data));
	init_websocket_client_program_data(client_program_data);
	//client_program_data->timeout_no_answer_ms = 2000;

	int ret = init_websocket_connection(client_program_data, CONNECTION_TARGET_IP, CONNECTION_TARGET_PORT, CONNECTION_TARGET_PATH, sleep_function_ms, get_curr_ms);
	client_program_data->text_message_handler = on_text_data_from_ws;
	client_program_data->binary_message_handler = on_binary_data_from_ws;
	if (ret == 0) {
		printf("Connected to websocket\n\n");
		init_websocket_recv_thread(client_program_data, &thread_index);

		sleep_function_ms(5000);

		send_websocket_binary(client_program_data, (const uint8_t*)"CIAO", 4);
		sleep_function_ms(5000);
		send_websocket_text(client_program_data, "CIAO2", 5);

		// Use the sleep to test out ping and pong in the background
		sleep_function_ms(20000);

		bool_t was_successful = !client_program_data->is_socket_closed;
		close_websocket(client_program_data);
		stop_websocket_recv_thread(client_program_data, &thread_index);

		free(client_program_data);
		if(was_successful)
			printf("\nConnection ended successfully\n");
		else
			printf("\nConnection ended unsuccessfully\n");
	}
	else
		printf("Connection failed. Reason: %d\n", ret);
	return 0;
}

// SPDX-License-Identifier: CC0-1.0
//
// SPDX-FileContributor: Antonio Niño Díaz, 2024-2025 - Lorenzooone 2026

// This example shows how to connect to a websocket and exchange data.

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <nds.h>
#include <dswifi9.h>

#include "websocket_simple_client.h"

#define CONNECTION_TARGET_IP "192.168.0.100"
#define CONNECTION_TARGET_PORT "11111"
#define CONNECTION_TARGET_PATH "/poolda"

static PrintConsole topScreen;
static PrintConsole bottomScreen;

static Wifi_AccessPoint AccessPoint;

static void init_ms_clock() {
	systemCounterSetup();
}

static uint32_t get_curr_ms() {
	static bool_t has_initialized_ms_clock = false;
	if(!has_initialized_ms_clock) {
		init_ms_clock();
		has_initialized_ms_clock = true;
	}
	return systemCounterTicksToMsec(systemCounterGetTicks());
}

int websocket_recv_thread(void *arg) {
    struct websocket_client_program_data* client_program_data = (struct websocket_client_program_data*)arg;

    while (!client_program_data->is_socket_closed) {
        cothread_yield();
        websocket_recv_handler(client_program_data);
    }

	return 0;
}

void init_websocket_recv_thread(struct websocket_client_program_data* client_program_data, cothread_t* thread_index) {
	if((*thread_index) != -1)
		return;

	*thread_index = cothread_create(websocket_recv_thread, client_program_data, 0, 0);
}

void stop_websocket_recv_thread(struct websocket_client_program_data* client_program_data, cothread_t* thread_index) {
	if((*thread_index) == -1)
		return;

	while(!cothread_has_joined(*thread_index))
        cothread_yield_irq(IRQ_VBLANK);

	cothread_delete(*thread_index);
	*thread_index = -1;
}

void sleep_function_ms(int wanted_ms) {
	uint32_t start_time = get_curr_ms();
	while((get_curr_ms() - start_time) < wanted_ms)
        cothread_yield_irq(IRQ_VBLANK);
}

void access_point_selection_menu(void)
{
	// Set the library in scan mode
	Wifi_ScanMode();

	int chosen = 0;

	while (1)
	{
		cothread_yield_irq(IRQ_VBLANK);

		scanKeys();
		uint16_t keys = keysDown();

		// Get number of APs in the area
		int count = Wifi_GetNumAP();

		consoleClear();

		printf("Number of AP: %d\n", count);
		printf("\n");

		if (count == 0)
			continue;

		if (keys & KEY_UP)
			chosen--;

		if (keys & KEY_DOWN)
			chosen++;

		if (chosen < 0)
			chosen = 0;
		if (chosen >= count)
			chosen = count - 1;

		int first = chosen - 5;
		if (first < 0)
			first = 0;

		int last = first + 6;
		if (last >= count)
			last = count - 1;

		for (int i = first; i <= last; i++)
		{
			Wifi_AccessPoint ap;
			Wifi_GetAPData(i, &ap);

			printf("%s [%.20s]%s\n", i == chosen ? "->" : "  ", ap.ssid,
				   (ap.flags & WFLAG_APDATA_CONFIG_IN_WFC) ? " WFC" : "");
			printf("   %-4s | Ch %2d | RSSI %d\n",
				   Wifi_ApSecurityTypeString(ap.security_type), ap.channel,
				   ap.rssi);
			printf("\n");

			if (i == chosen)
				AccessPoint = ap;
		}

		if (keys & KEY_A)
		{
			if (AccessPoint.flags & WFLAG_APDATA_COMPATIBLE)
				break;
		}
	}
}

void connect_to_firmware_access_points(void)
{
	printf("Connecting to firmware APs...\n");

	// Autoconnect to firmware access points
	Wifi_AutoConnect();

	// IP settings have been loaded from flash
}

void connect_to_other_access_points(void)
{
	// Search for all available access points
	access_point_selection_menu();

	// Setting everything to 0 will make DHCP determine the IP address
	Wifi_SetIP(0, 0, 0, 0, 0);

	bool_t wfc_settings_used = false;

	if (AccessPoint.flags & WFLAG_APDATA_CONFIG_IN_WFC)
	{
		// If the AP is known, use the password stored in the WFC settings. Ask
		// the user whether to use the saved settings or to type the password
		// manually,

		printf("WFC settings found:\n");
		printf("\n");
		printf("A: Use WFC password\n");
		printf("B: Type password manually\n");
		printf("\n");

		while (1)
		{
			cothread_yield_irq(IRQ_VBLANK);
			scanKeys();
			u16 keys = keysDown();
			if (keys & KEY_A)
			{
				wfc_settings_used = true;
				printf("Using WFC settings...\n");
				printf("\n");
				Wifi_ConnectWfcAP(&AccessPoint);
				break;
			}
			if (keys & KEY_B)
			{
				printf("Ignoring WFC settings...\n");
				printf("\n");
				break;
			}
		}
	}

	if (!wfc_settings_used)
	{
		// This AP isn't in the WFC settings. If the access point requires a
		// WEP/WPA password, ask the user to provide it. Note that you can still
		// allow the user to call this function with an AP that is saved in the
		// WFC settings, but the function will ignore the saved settings in the
		// WFC configuration and use the provided password instead.
		if (AccessPoint.security_type != AP_SECURITY_OPEN)
		{
			consoleClear();

			char password[100];
			size_t password_len;

			printf("Please, enter the password:\n");

			while (1)
			{
				password[0] = '\0';
				scanf("%s", password);

				password_len = strlen(password);
				if (password[password_len - 1] == '\n')
					password[password_len - 1] = '\0';

				bool_t valid = false;
				if (AccessPoint.security_type == AP_SECURITY_WEP)
					valid = (password_len == 13) || (password_len == 5);
				else
					valid = (password_len <= 64);

				if (valid)
					break;

				printf("Invalid key length! [%s] %zu\n", password, password_len);
			}

			Wifi_ConnectSecureAP(&AccessPoint, password, password_len);
		}
		else
		{
			Wifi_ConnectSecureAP(&AccessPoint, NULL, 0);
		}
	}

	printf("Selected network:\n");
	printf("\n");
	printf("%.31s\n", AccessPoint.ssid);
	printf("Security: %s | Ch: %d\n",
		   Wifi_ApSecurityTypeString(AccessPoint.security_type),
		   AccessPoint.channel);
	printf("\n");
}

// Callback called whenever the keyboard is pressed so that a character is
// printed on the screen.
void on_key_pressed(int key)
{
   if (key > 0)
	  printf("%c", key);
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
	// Redirect stderr to the no$gba debug console
	consoleDebugInit(DebugDevice_NOCASH);

	videoSetMode(MODE_0_2D);
	videoSetModeSub(MODE_0_2D);

	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankC(VRAM_C_SUB_BG);

	consoleInit(&topScreen, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
	consoleInit(&bottomScreen, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 3, false, true);

	// Load demo keyboard
	Keyboard *kbd = keyboardDemoInit();
	kbd->OnKeyPressed = on_key_pressed;

	consoleSelect(&topScreen);
	consoleArm7Setup(&topScreen, 1024); // Redirect ARM7 messages to the console

	printf("Initializing WiFi...\n");

	// If the ROM is loaded without holding L DSWiFi will try to boot in DSi
	// mode. If the user holds L at boot it will force DS mode even on DSi.
	scanKeys();
	u32 flags = INIT_ONLY |
				((keysHeld() & KEY_L) ? WIFI_DS_MODE_ONLY : WIFI_ATTEMPT_DSI_MODE);

	if (!Wifi_InitDefault(flags))
	{
		printf("Can't initialize WiFi!\n");
		goto end;
	}

	printf("WiFi initialized!\n");
	printf("\n");

	while (1)
	{
connect:
		Wifi_EnableWifi();

		consoleSelect(&bottomScreen);
		consoleClear();
		printf("WiFi connection options:\n");
		printf("\n");
		printf("A: Search for APs\n");
		printf("B: Connect to firmware AP\n");
		printf("\n");
		printf("\n");

		int num_wfc_caps = Wifi_GetData(WIFIGETDATA_NUMWFCAPS, 0, NULL);

		printf("APs configured in firmware: %d\n", num_wfc_caps);
		if (num_wfc_caps <= 0)
			printf("No APs setup: Option A will fail\n");

		int selection = 0;

		while (selection == 0)
		{
			cothread_yield_irq(IRQ_VBLANK);
			scanKeys();
			if (keysDown() & KEY_B)
				selection = 1;
			if (keysDown() & KEY_A)
				selection = 2;
		}

		consoleClear();

		if (selection == 1)
		{
			consoleSelect(&bottomScreen);

			connect_to_firmware_access_points();
		}
		else if (selection == 2)
		{
			consoleSelect(&bottomScreen);

			connect_to_other_access_points();
		}

		consoleSelect(&topScreen);

		consoleSelect(&bottomScreen);

		consoleClear();

		// Wait until we're connected

		printf("Connecting to AP\n");
		printf("Press B to cancel\n");
		printf("\n");

		int oldstatus = -1;
		while (1)
		{
			cothread_yield_irq(IRQ_VBLANK);

			scanKeys();
			if (keysDown() & KEY_B)
				goto connect;

			//consoleClear();
			int status = Wifi_AssocStatus();

			if (status != oldstatus)
			{
				printf("%s\n", ASSOCSTATUS_STRINGS[status]);
				oldstatus = status;
			}

			if (status == ASSOCSTATUS_CANNOTCONNECT)
			{
				printf("\n");
				printf("Cannot connect to AP\n");
				printf("Press START to restart\n");

				while (1)
				{
					cothread_yield_irq(IRQ_VBLANK);
					scanKeys();
					if (keysDown() & KEY_START)
						goto connect;
				}
			}

			if (status == ASSOCSTATUS_ASSOCIATED)
				break;
		}

		// ASSOCSTATUS_ASSOCIATED is reached if we have an IPv4 or IPv6 address.
		// DHCP for IPv4 is faster than DHCPv6, and this demo focuses on
		// selecting IPv6 or IPv4, so we need to wait for an address to be
		// assigned to us. However, this may never happen if the network doesn't
		// support IPv6, so we can't wait forever.
		//
		// You can remove this wait loop if you want. This is only here so that
		// the example can use IPv6 easier.
		printf("Waiting for an IPv6 address...\n");

		struct in6_addr ipv6 = { 0 };

		unsigned int timeout = 5 * 60; // 5 seconds

		while (1)
		{
			cothread_yield_irq(IRQ_VBLANK);
			if (Wifi_GetIPv6(&ipv6))
				break;

			timeout--;
			if (timeout == 0)
			{
				printf("Can't get IPv6 address\n");
				break;
			}
		}

		consoleClear();

		// Get network information

		consoleSelect(&topScreen);

		struct in_addr ip = { 0 }, gateway = { 0 }, mask = { 0 };
		struct in_addr dns1 = { 0 }, dns2 = { 0 };
		ip = Wifi_GetIPInfo(&gateway, &mask, &dns1, &dns2);

		printf("\n");
		printf("IPv4 information:\n");
		printf("\n");
		printf("IP:	  %s\n", inet_ntoa(ip));
		printf("Gateway: %s\n", inet_ntoa(gateway));
		printf("Mask:	%s\n", inet_ntoa(mask));
		printf("DNS1:	%s\n", inet_ntoa(dns1));
		printf("DNS2:	%s\n", inet_ntoa(dns2));
		printf("\n");
		printf("IPv6 information:\n");
		printf("\n");
		char buf[128];
		printf("IP: %s\n", inet_ntop(AF_INET6, &ipv6, buf, sizeof(buf)));

		printf("\n");

		consoleSelect(&bottomScreen);

		cothread_t thread_index = -1;
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

		printf("\n");
		printf("Press A to continue\n");

		while (1)
		{
			cothread_yield_irq(IRQ_VBLANK);

			scanKeys();
			u16 keys = keysDown();

			if (keys & KEY_A)
				break;
		}

		if (Wifi_DisconnectAP() != 0)
			printf("Error: Wifi_DisconnectAP()\n");

		consoleSelect(&topScreen);

		printf("\n");
		printf("Press A to restart\n");
		printf("Press B to end demo\n");

		while (1)
		{
			cothread_yield_irq(IRQ_VBLANK);

			scanKeys();
			if (keysDown() & KEY_A)
				break;
			if (keysDown() & KEY_B)
			{
				Wifi_DisableWifi();
				goto end;
			}
		}
	}

end:
	consoleSelect(&topScreen);
	consoleClear();

	printf("End of demo!\n");
	printf("\n");
	printf("Press START to exit");

	while (1)
	{
		cothread_yield_irq(IRQ_VBLANK);
		scanKeys();
		if (keysHeld() & KEY_START)
			break;
	}

	return 0;
}

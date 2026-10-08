#!/usr/bin/env python

# SPDX-License-Identifier: CC0-1.0
#
# SPDX-FileContributor: Lorenzooone 2026

import datetime
import asyncio
import websockets
import threading
import signal
import os
import sys
import boto3
import botocore
from random import Random
from time import sleep
import http

#import logging
#logging.basicConfig(level=logging.DEBUG)
#logging.getLogger("websockets").setLevel(logging.DEBUG)

server_wait_between_checks_seconds = 120
default_timeout_timer_seconds = 0.2 * 60 * 60

max_concurrent_connections = 10000

def is_ws_connection_allowed():
	return (max_concurrent_connections > 0) and (currently_open_connections < max_concurrent_connections)

def add_ws_connection():
	global currently_open_connections
	currently_open_connections += 1

def remove_ws_connection():
	global currently_open_connections
	currently_open_connections -= 1

class WebsocketServer (threading.Thread):
	'''
	Class which handles responding to the websocket requests.
	'''
	
	def __init__(self, host="", port=11111):
		threading.Thread.__init__(self)
		self.daemon=True
		self.host = host
		try:
			self.port = int(os.environ["PORT"])
		except KeyError as e:
			self.port = port
		
	async def cleaner(processer, path):
		pass

	async def handler(websocket, path):
		"""
		Gets the data and then calls the proper handler while keeping
		the connection active.
		"""
		# Need to understand how to signal "Not now"
		if not is_ws_connection_allowed():
			return

		add_ws_connection()
		processer = None
		time_disconnect = default_timeout_timer_seconds
		connection_start = datetime.datetime.now()
		while True:
			try:
				data = await asyncio.wait_for(websocket.recv(), timeout=server_wait_between_checks_seconds)
			except websockets.ConnectionClosed:
				print(f"Terminated")
				await WebsocketServer.cleaner(processer, path)
				break
			except asyncio.TimeoutError:
				pass
			except TimeoutError:
				pass
			except Exception as e:
				print('Websocket server error:', str(e))
				await WebsocketServer.cleaner(processer, path)
				break
			if (datetime.datetime.now() - connection_start).total_seconds() > time_disconnect:
				print(f"Timeout")
				await WebsocketServer.cleaner(processer, path)
				break

			print(path)
			print(data)
			answer = None

			if data == b"CIAO":
				answer = b"CEO"
			if data == "CIAO2":
				answer = "CEO2"

			if answer is not None:
				await websocket.send(answer)

		remove_ws_connection()

	def shared_base_http_handler(self, path):
		answer = None
		if path == '/httptest':
			answer = 0
		if path == '/httptest2':
			answer = 1
		if path == '/httptest3':
			answer = 2
		if answer is not None:
			return bytes([0x00, 0x01, 0x02, answer])
		return None

	def old_base_http_handler(self, path, request_headers):
		ret = self.shared_base_http_handler(path)
		if ret is None:
			return None
		headers = [("Content-Type", "application/octet-stream"), ("Content-Length", str(len(ret))), ("Access-Control-Allow-Origin", "*")]
		return http.HTTPStatus.OK, headers, ret

	def new_base_htpp_handler(self, connection, request):
		ret = self.shared_base_http_handler(request.path)
		if ret is None:
			return None
		headers = websockets.datastructures.Headers()
		headers["Content-Type"] = "application/octet-stream"
		headers["Content-Length"] = str(len(ret))
		headers["Access-Control-Allow-Origin"] = "*"
		return websockets.http11.Response(http.HTTPStatus.OK, "OK", headers, ret)

	def get_base_htpp_handler(self):
		if (sys.version_info[0] <= 3) and (sys.version_info[1] < 11):
			return self.old_base_http_handler
		return self.new_base_htpp_handler

	async def new_handler(websocket):
		await WebsocketServer.handler(websocket, websocket.request.path)

	def get_handler_method(self):
		if (sys.version_info[0] <= 3) and (sys.version_info[1] < 13):
			return WebsocketServer.handler
		return WebsocketServer.new_handler

	async def server_runner(self):
		from websockets.asyncio.server import serve
		server = await serve(self.get_handler_method(), self.host, self.port, process_request = self.get_base_htpp_handler())
		await server.serve_forever()

	def run(self):
		"""
		Runs the server in a second Thread in order to keep
		the program responsive.
		"""
		if (sys.version_info[0] <= 3) and (sys.version_info[1] < 14):
			loop = asyncio.new_event_loop()
			asyncio.set_event_loop(loop)
			start_server = websockets.serve(self.get_handler_method(), self.host, self.port, process_request = self.get_base_htpp_handler())
			loop.run_until_complete(start_server)
			loop.run_forever()
		else:
			asyncio.run(self.server_runner())

def exit_gracefully():
	os._exit(1)

def signal_handler(sig, frame):
	print('You pressed Ctrl+C!')
	exit_gracefully()

if __name__ == "__main__":
	currently_open_connections = 0
	ws = WebsocketServer()
	ws.start()

	signal.signal(signal.SIGINT, signal_handler)

	while True:
		sleep(1)

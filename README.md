# Simple C Websocket Client - DS and Linux example

An example showcasing a very Simple C Websocket client that works on both the Nintendo DS (compiled using BlocksDS) and Linux.

## Details

The Websocket client code is in the source\_websocket folder.
The Websocket client code supports connections lasting across time, and properly responds to server pings and messages. It relies on user-provided callbacks. It also relies on the user calling websocket\_recv\_handler periodically.
Check websocket\_simple\_client.h for documentation.

This is not a complete implementation. As an example, it lacks TLS support (for now). It reflects my own needs.

Included is a Python test server that can be run locally to test out the code.

##Licensing

Code in the source\_websocket folder is under the MIT license.

Code in the source\_ds and source\_linux is meant as an example of how to use the Websocket client code, and is under SPDX-License-Identifier: CC0-1.0.

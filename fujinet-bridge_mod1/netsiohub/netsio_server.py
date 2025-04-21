"""
NetSIO Server Implementation

This module contains the server-side components for NetSIO communication,
including the NetSIOServer and handler classes.
"""

import socketserver
import socket
import threading
import queue
import time

from netsiohub.netsio_protocol import *
from netsiohub.netsio_client import NetSIOClient

class NetSIOServer(socketserver.UDPServer):
    """UDP server implementation for NetSIO protocol"""
    
    def __init__(self, server_address, handler_class, hub=None):
        """initialize server with address, handler class, and hub reference"""
        self.allow_reuse_address = True
        super().__init__(server_address, handler_class)
        self.hub = hub
        self.server_thread = None
        
    def start(self):
        """start server in a separate thread"""
        if self.server_thread is None:
            self.server_thread = threading.Thread(target=self.serve_forever)
            self.server_thread.daemon = True
            self.server_thread.start()
            
    def stop(self):
        """stop server"""
        if self.server_thread is not None:
            self.shutdown()
            self.server_thread = None

class NetSIOHandler(socketserver.BaseRequestHandler):
    """handler for NetSIO protocol UDP packets"""
    
    def handle(self):
        """handle UDP packet"""
        data = self.request[0]        # UDP packet data
        sock = self.request[1]        # UDP socket
        
        # get peripheral IP and port from packet
        client_addr = self.client_address
        
        # parse data and call hub's handle method
        if len(data) < 2:
            # too short for a NetSIO message
            warning_print(f"Received short UDP packet from {client_addr}")
            return
            
        try:
            msg_len = data[0] + 256 * data[1]
            if len(data) < msg_len + 2:
                # too short for specified message length
                warning_print(f"Received short UDP packet from {client_addr} (specified length {msg_len}, actual length {len(data) - 2})")
                return
                
            # extract message id and arg
            msg_id = data[2]
            
            # handle the different length messages
            if msg_len == 1:
                # no arg
                arg = None
            elif msg_len == 2:
                # single byte arg
                arg = data[3]
            else:
                # multi-byte arg (stored in NetSIO as a list)
                arg = list(data[3:msg_len+2])
                
            # call hub's handle method with message
            if self.server.hub is not None:
                msg = NetSIOMsg(msg_id, arg)
                peripheral = NetSIOClient(sock=sock, addr=client_addr)
                self.server.hub.handle_peripheral_msg(msg, peripheral)
                
        except Exception as e:
            error_print(f"Error handling UDP packet from {client_addr}: {e}")
            return

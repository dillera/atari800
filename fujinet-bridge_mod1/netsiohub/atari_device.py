"""
Atari Device Implementation

This module contains Atari device specific classes for handling
communications with the Atari800 emulator.
"""

import socket
import threading
import queue
import time
import struct
from netsiohub import deviceserver

from netsiohub.netsio_protocol import *
from netsiohub.device_manager import DeviceManager

class AtDevManager(DeviceManager):
    """Atari device manager for communications with Atari800 emulator"""
    
    def __init__(self, hub=None):
        """initialize device manager with reference to hub"""
        super().__init__()
        self.hub = hub
        self.handler_class = None
        self.server = None
        
    def set_handler_class(self, handler_class):
        """set handler class for TCP server"""
        self.handler_class = handler_class
        
    def start_server(self, host, port):
        """start TCP server for Atari800 emulator connection"""
        if self.server is not None:
            self.server.shutdown()
            
        # Create server with our handler class
        self.server = deviceserver.DeviceTCPServer((host, port), self.handler_class)
        # Set hub reference for the server to pass to handlers
        self.server.hub = self.hub
        # Start server in a separate thread
        self.server_thread = threading.Thread(target=self.server.serve_forever)
        self.server_thread.daemon = True
        self.server_thread.start()
        
    def connected(self):
        """check if Atari800 emulator is connected"""
        # The hub will track connection state
        return self.hub.host_handler is not None
        
    def to_host(self, msg:NetSIOMsg):
        """send message to Atari800 emulator"""
        if self.hub.host_handler is not None:
            self.hub.host_queue.put(msg)
        
    def to_peripheral(self, msg:NetSIOMsg):
        """forward message to peripherals via hub"""
        if self.hub is not None:
            self.hub.device_manager.to_peripheral(msg)
        
    def register_peripheral(self, client):
        """Not used for Atari device manager"""
        pass
        
    def unregister_peripheral(self, client):
        """Not used for Atari device manager"""
        pass

class AtDevHandler(deviceserver.DeviceTCPHandler):
    """TCP handler for Atari800 emulator connection"""
    
    def setup(self):
        """setup handler when connection is established"""
        super().setup()
        log_trace(f"AtDevHandler.setup: New connection from {self.client_address}")
        self.disconnect_pending = False
        self.sio_event = threading.Event()
        self.sio_script_handler = None
        self.sync_cmd = None
        self.sync_result = None
        
        # Register with hub
        if self.server.hub is not None:
            self.server.hub.register_host_handler(self)
    
    def finish(self):
        """cleanup when connection is closed"""
        log_trace(f"AtDevHandler.finish: Connection closed from {self.client_address}")
        # Unregister from hub
        if self.server.hub is not None:
            self.server.hub.unregister_host_handler(self)
        super().finish()
    
    def handle(self):
        """main handler loop - process incoming TCP data"""
        buffer = b''
        while not self.disconnect_pending:
            try:
                # Read data from socket
                data = self.request.recv(4096)
                if not data:
                    # Connection closed
                    break
                    
                # Add to buffer and process complete messages
                buffer += data
                while len(buffer) >= 2:
                    # Check if we have a complete message
                    msg_len = buffer[0] + 256 * buffer[1]
                    if len(buffer) >= msg_len + 2:
                        # Extract message
                        msg_bytes = buffer[:msg_len+2]
                        buffer = buffer[msg_len+2:]
                        
                        # Parse message
                        if msg_len < 1:
                            warning_print(f"AtDevHandler.handle: Received message with invalid length {msg_len}")
                            continue
                            
                        msg_id = msg_bytes[2]
                        
                        # Handle different message lengths
                        if msg_len == 1:
                            # No arg
                            arg = None
                        elif msg_len == 2:
                            # Single byte arg
                            arg = msg_bytes[3]
                        else:
                            # Multi-byte arg
                            arg = list(msg_bytes[3:msg_len+2])
                            
                        # Create NetSIO message
                        msg = NetSIOMsg(msg_id, arg)
                        
                        # Handle sync messages directly, forward others to hub
                        if msg_id in (NETSIO_COMMAND_OFF_SYNC, NETSIO_DATA_BYTE_SYNC):
                            if self.server.hub is not None:
                                result = self.server.hub.handle_host_msg_sync(msg)
                                # Send result back
                                self.request.sendall(bytes([1, 0, result]))
                        else:
                            # Forward to hub
                            if self.server.hub is not None:
                                self.server.hub.handle_host_msg(msg)
                    else:
                        # Need more data for complete message
                        break
                        
            except socket.timeout:
                # No data available, retry
                continue
            except ConnectionResetError:
                error_print("CONNECTION RESET")
                break
            except ConnectionAbortedError:
                error_print("CONNECTION ABORTED")
                break
            except Exception as e:
                error_print(f"AtDevHandler.handle: Error: {e}")
                break
                
        # Connection ended
        log_trace(f"AtDevHandler.handle: Connection handler ending for {self.client_address}")
    
    def send_msg(self, msg:NetSIOMsg):
        """send message to Atari800 emulator"""
        try:
            self.request.sendall(msg.to_bytes())
        except Exception as e:
            error_print(f"AtDevHandler.send_msg: Error sending to {self.client_address}: {e}")
            self.disconnect_pending = True

class AtDevThread(threading.Thread):
    """Thread for sending messages to Atari800 emulator"""
    
    def __init__(self, hub):
        """initialize thread with reference to hub"""
        super().__init__()
        self.hub = hub
        self.stop_event = threading.Event()
        self.daemon = True
    
    def run(self):
        """thread main: read from queue and forward to Atari800 emulator"""
        while not self.stop_event.is_set():
            try:
                # Get message from queue, with timeout to allow stopping
                msg = self.hub.host_queue.get(timeout=0.1)
                
                # Check if host handler is available
                if self.hub.host_handler is not None:
                    try:
                        # Send message to Atari800 emulator
                        self.hub.host_handler.send_msg(msg)
                    except Exception as e:
                        error_print(f"AtDevThread.run: Error sending to host: {e}")
                        # Put message back in queue for retry
                        self.hub.host_queue.put(msg)
                else:
                    # No host handler, put message back in queue for retry
                    self.hub.host_queue.put(msg)
                    # Avoid busy loop when no host handler is available
                    time.sleep(0.1)
                    
            except queue.Empty:
                # No data available, retry
                continue
            except Exception as e:
                error_print(f"AtDevThread.run: Unexpected error: {e}")
                
        log_trace("AtDevThread ending")
    
    def stop(self):
        """stop the thread"""
        self.stop_event.set()

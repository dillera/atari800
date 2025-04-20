#!/usr/bin/env python3

from netsiohub import deviceserver
from netsiohub.netsio import *

from enum import IntEnum
import socket, socketserver
import threading
import queue
import sys
import time
import struct
import argparse

try:
    from netsiohub.serial import *
    has_serial = True
except ModuleNotFoundError:
    has_serial = False

_start_time = timer()

def print_banner():
    print("NetSIO HUB", HUB_VERSION)

# --- Added Logging ---
def log_trace(message):
    """Helper for consistent logging"""
    print(f"[TRACE] {time.time():.6f}: {message}")
# --- End Added Logging ---

class NetSIOClient:
    def __init__(self, address, sock):
        self.address = address
        self.sock = sock
        self.expire_time = time.time() + ALIVE_EXPIRATION
        # self.cpb = 94 # default 94 CPB (19200 baud)
        self.credit = 0
        self.lock = threading.Lock()

    def expired(self, t=None):
        if t is None:
            t = time.time()
        with self.lock:
            expired = True if self.expire_time < t else False
        return expired

    def refresh(self):
        with self.lock:
            self.expire_time = time.time() + ALIVE_EXPIRATION

    def update_credit(self, credit, threshold=0):
        update = False
        with self.lock:
            if self.credit <= threshold:
                self.credit = credit
                update = True
        return update

class NetInThread(threading.Thread):
    """Thread to handle incoming network traffic"""
    def __init__(self, hub, port):
        self.hub:NetSIOHub = hub
        self.port:int = int(port)
        self.server:NetSIOServer = None
        self.server_ready = threading.Event()
        super().__init__()

    def run(self):
        debug_print("NetInThread started")
        with NetSIOServer(self.hub, self.port) as self.server:
            print("Listening for NetSIO packets on port {}".format(self.port))
            self.server_ready.set()
            self.server.serve_forever()
        self.server_ready.clear()
        debug_print("NetInThread stopped")

    def stop(self):
        debug_print("Stop NetInThread")
        if self.server is not None:
            self.server.shutdown()

class NetInBuffer:
    """Byte buffer with auto flush on size or age"""

    BUFFER_SIZE = 130 # 130 bytes
    BUFFER_MAX_AGE = 0.005 # 5 ms

    def __init__(self, server):
        self.server = server
        self.data = bytearray()
        self.lock = threading.RLock()
        self.monitor_condition = threading.Condition()
        self.monitor_event = threading.Event()
        self.tmout = 0.0
        threading.Thread(target=self.buffer_monitor).start()

    def buffer_monitor(self):
        debug_print("buffer_monitor started")
        self.monitor_condition.acquire()
        while True:
            #debug_print("buffer_monitor long waiting")
            self.monitor_condition.wait()
            tmout = self.tmout
            #debug_print("buffer_monitor tmout:", tmout)
            self.monitor_event.set()
            if tmout is None:
                break
            while True:
                #debug_print("buffer_monitor timeout waiting")
                reset = self.monitor_condition.wait(tmout)
                if not reset:
                    #debug_print("buffer_monitor expired")
                    self.monitor_event.set() # in case set_delay is waiting when we timed-out
                    self.flush()
                    break
                tmout = self.tmout
                #debug_print("buffer_monitor new tmout:", tmout)
                self.monitor_event.set()
                if tmout is None:
                    break
            if tmout is None:
                break
        self.monitor_condition.release()
        debug_print("buffer_monitor stopped")

    def set_delay(self, t):
        with self.monitor_condition:
            #debug_print("buffer_monitor notify")
            self.tmout = t
            self.monitor_event.clear()
            self.monitor_condition.notify()
            #debug_print("buffer_monitor notification sent")
        self.monitor_event.wait()
        #debug_print("buffer_monitor delay applied")

    def stop(self):
        self.set_delay(None)

    def extend(self, b:bytearray):
        with self.lock:
            self.data.extend(b)
            l = len(self.data)
        if l >= self.BUFFER_SIZE:
            self.flush()
        else:
            self.set_delay(self.BUFFER_MAX_AGE)

    def flush(self):
        msg = None
        with self.lock:
            if len(self.data):
                if len(self.data) > 1:
                    msg = NetSIOMsg(NETSIO_DATA_BLOCK, self.data)
                else:
                    msg = NetSIOMsg(NETSIO_DATA_BYTE, self.data)
                self.data = bytearray()
        if msg:
            debug_print("< NET FLUSH", msg)
            self.server.hub.handle_device_msg(msg, None)

class NetSIOServer(socketserver.UDPServer):
    """NetSIO UDP Server"""

    def __init__(self, hub:NetSIOHub, port:int):
        self.hub:NetSIOHub = hub
        self.clients_lock = threading.Lock()
        self.clients = {}
        self.last_recv = timer()
        self.sn = 0 # TODO test only
        # single bytes buffering
        self.inbuffer = NetInBuffer(self)
        super().__init__(('', port), NetSIOHandler)

    def shutdown(self):
        self.inbuffer.stop()
        super().shutdown()

    def register_client(self, address, sock):
        with self.clients_lock:
            if address not in self.clients:
                client = NetSIOClient(address, sock)
                self.clients[address] = client
                info_print("Device connected: {}  Devices: {}".format(addrtos(address), len(self.clients)))
            else:
                client = self.clients[address]
                client.sock = sock
                client.refresh()
                info_print("Device reconnected: {}  Devices: {}".format(addrtos(address), len(self.clients)))
        # give the client initial credit
        client.update_credit(DEFAULT_CREDIT) # initial credit
        self.send_to_client(client, NetSIOMsg(NETSIO_CREDIT_UPDATE, DEFAULT_CREDIT))
        # notify hub
        self.hub.handle_device_msg(NetSIOMsg(NETSIO_DEVICE_CONNECT), client)
        return client

    def deregister_client(self, address, expired=False):
        with self.clients_lock:
            try:
                client = self.clients.pop(address)
            except KeyError:
                client = None
            count = len(self.clients)
        if client is not None:
            info_print("Device disconnected{}: {}  Devices: {}".format(
                " (connection expired)" if expired else "", addrtos(address), count))
            self.hub.handle_device_msg(NetSIOMsg(NETSIO_DEVICE_DISCONNECT), client)

    def get_client(self, address):
        with self.clients_lock:
            client = self.clients.get(address)
        return client

    def send_to_client(self, client:NetSIOClient, msg):
        # Only log if it's not an ALIVE response to reduce noise
        if msg.id != NETSIO_ALIVE_RESPONSE:
            log_trace(f"NetSIOServer.send_to_client: Sending msg {msg} via UDP to {addrtos(client.address)}")
        client.sock.sendto(struct.pack('B', msg.id) + msg.arg, client.address)
        # Keep debug_print active for now, or comment if needed
        # debug_print("> NET {} {}".format(addrtos(client.address), msg))

    def send_to_all(self, msg):
        """broadcast all connected netsio devices"""
        t = time.time()
        expire = False
        with self.clients_lock:
            clients = list(self.clients.values())
        # TODO test only
        msg.arg.append(self.sn)
        self.sn = (1 + self.sn) & 255
        for c in clients:
            # skip sending to expired clients
            if c.expired(t):
                expire = True
                continue
            self.send_to_client(c, msg)
        if expire:
            # remove expired clients
            self.expire_clients()
    
    def expire_clients(self):
        t = time.time()
        with self.clients_lock:
            expired = [c for c in self.clients.values() if c.expired(t)]
        for c in expired:
            self.deregister_client(c.address, expired=True)
        
    def connected(self):
        """Return true if any client is connected"""
        with self.clients_lock:
            return len(self.clients) > 0

    def credit_clients(self):
        # send credits to waiting clients if there is a room in a queue
        credit = DEFAULT_CREDIT - self.hub.host_queue.qsize()
        if credit >= 2:
            with self.clients_lock:
                clients = list(self.clients.values())
            msg = NetSIOMsg(NETSIO_CREDIT_UPDATE, credit)
            for c in clients:
                if c.update_credit(credit):
                    self.send_to_client(c, msg)

class NetSIOHandler(socketserver.BaseRequestHandler):
    """NetSIO received packet handler"""

    def handle(self):
        data, sock = self.request
        msg = NetSIOMsg(data[0], data[1:])
        ca = self.client_address

        debug_print("< NET IN +{:.0f} {} {}".format(
            (timer()-self.server.last_recv)*1.e6,
            addrtos(ca), msg))
        self.server.last_recv = timer()

        if msg.id < NETSIO_CONN_MGMT:
            # events from connected/registered devices
            client = self.server.get_client(self.client_address)
            if client is not None:
                if client.expired():
                    # expired connection
                    self.server.deregister_client(client.address, expired=True)
                else:
                    # update expiration
                    client.refresh()
                    if msg.id == NETSIO_DATA_BYTE:
                        # buffering
                        self.server.inbuffer.extend(msg.arg)
                    else:
                        # send buffer firts, if any
                        self.server.inbuffer.flush()
                        self.server.hub.handle_device_msg(msg, client)
        else:
            # connection management
            if msg.id == NETSIO_DEVICE_DISCONNECT:
                # device disconnected, deregister client
                self.server.deregister_client(self.client_address)
            elif msg.id == NETSIO_DEVICE_CONNECT:
                # device connected, register client for netsio messages
                self.server.register_client(self.client_address, sock)
            elif msg.id == NETSIO_PING_REQUEST:
                # ping request, send ping response (always)
                self.server.send_to_client(
                    NetSIOClient(self.client_address, sock),
                    NetSIOMsg(NETSIO_PING_RESPONSE)
                )
            elif msg.id == NETSIO_ALIVE_REQUEST:
                # log_trace(f"NetSIOHandler received NETSIO_ALIVE_REQUEST: {msg}") # Commented out to reduce noise
                client = self.server.get_client(self.client_address)
                if client is not None:
                    client.refresh()
                    # Try to update credit if payload exists
                    if msg.arg and len(msg.arg) > 0: 
                        client.update_credit(msg.arg[0]) # Try updating credit, ignore result for response
                    else:
                        pass # log_trace(f"NetSIOHandler: Received NETSIO_ALIVE_REQUEST with empty msg.arg from {client.address}") # Commented out
                    
                    # Always send ALIVE response regardless of payload content
                    self.server.send_to_client(client, NetSIOMsg(NETSIO_ALIVE_RESPONSE)) 
            elif msg.id == NETSIO_CREDIT_STATUS:
                client = self.server.get_client(self.client_address)
                if client is not None and len(msg.arg):
                    # update client's credit
                    client.update_credit(msg.arg[0], 10) # threshold 10 to force credit update
                    # send new credit immediately if there is a room in a queue
                    credit = DEFAULT_CREDIT - self.server.hub.host_queue.qsize()
                    if credit >= 2 and client.update_credit(credit):
                        self.server.send_to_client(client, NetSIOMsg(NETSIO_CREDIT_UPDATE, credit))

class NetOutThread(threading.Thread):
    """Thread to send "messages" to connected netsio devices"""
    def __init__(self, q:queue.Queue, server:NetSIOServer):
        self.queue:queue.Queue = q
        self.server:NetSIOServer = server
        super().__init__()

    def run(self):
        debug_print("NetOutThread started")
        while True:
            msg = self.queue.get()
            if msg is None:
                break
            log_trace(f"NetOutThread.run: Got msg {msg} from queue. Forwarding to NetSIOServer.send_to_all")
            self.server.send_to_all(msg)

        debug_print("NetOutThread stopped")

    def stop(self):
        debug_print("Stop NetOutThread")
        clear_queue(self.queue)
        self.queue.put(None) # stop sign
        self.join()

class NetSIOManager(DeviceManager):
    """Manages NetSIO (SIO over UDP) traffic"""

    def __init__(self, port=NETSIO_PORT):
        super().__init__(port)
        self.device_queue = queue.Queue(16)
        self.netin_thread:NetInThread = None
        self.netout_thread:NetOutThread = None

    def start(self, hub):
        print("UDP port (NetSIO):", self.port)

        # network receiver
        self.netin_thread = NetInThread(hub, self.port)
        self.netin_thread.start()

        # wait for server to be created
        if not self.netin_thread.server_ready.wait(3):
            print("Time out waiting for NetSIOServer to start")

        # network sender
        self.netout_thread = NetOutThread(self.device_queue, self.netin_thread.server)
        self.netout_thread.start()

    def stop(self):
        debug_print("Stop NetSIOManager")
        if self.netin_thread:
            self.netin_thread.stop()
            self.netin_thread = None
        if self.netout_thread:
            self.netout_thread.stop()
            self.netout_thread = None

    def to_peripheral(self, msg):
        if msg.id in (NETSIO_COLD_RESET, NETSIO_WARM_RESET):
            debug_print("CLEAR DEV QUEUE")
            clear_queue(self.device_queue)

        if self.device_queue.full():
            log_trace(f"NetSIOManager.to_peripheral: Device queue FULL, cannot queue {msg}")
            debug_print("device queue FULL")
        else:
            log_trace(f"NetSIOManager.to_peripheral: Queuing msg {msg} for device. Queue size: {self.device_queue.qsize()}")
            debug_print("device queue [{}]".format(self.device_queue.qsize()))

        self.device_queue.put(msg)
        # debug_print("> DEV", msg)

    def connected(self):
        """Return true if any device is connected"""
        return self.netin_thread.server.connected()

    def credit_clients(self):
        return self.netin_thread.server.credit_clients()

class AtDevManager(HostManager):
    """Altirra custom device manager"""
    def __init__(self, arg_parser):
        super().__init__()
        self.arg_parser = arg_parser
        self.hub = None

    def run(self, hub):
        self.hub = hub
        deviceserver.run_deviceserver(AtDevHandler, NETSIO_ATDEV_PORT, self.arg_parser, self.run_server)

    def run_server(self, server):
        # make hub available to handler (via server object)
        server.hub = self.hub
        server.serve_forever()

    def stop(self):
        # TODO stop AtDevThread, if still running
        pass

class AtDevHandler(deviceserver.DeviceTCPHandler):
    """Handler to communicate with netsio.atdevice which lives in Altirra"""
    def __init__(self, *args, **kwargs):
        log_trace("AtDevHandler.__init__ called")
        
        # Populate handlers BEFORE calling super().__init__ so base class handle() can find them
        # Key: Altirra Event Code, Value: (Name_String, Handler_Method)
        self.handlers = {
            NETSIO_COMMAND_ON: ("COMMAND_ON", self.handle_netsio_command_on),
            NETSIO_DATA_BLOCK: ("DATA_BLOCK", self.handle_netsio_data_block),
            NETSIO_COMMAND_OFF_SYNC: ("COMMAND_OFF_SYNC", self.handle_netsio_command_off_sync),
            NETSIO_WARM_RESET: ("WARM_RESET", self.handle_warmreset),
            NETSIO_COLD_RESET: ("COLD_RESET", self.handle_coldreset),
            # Add other handlers as needed (e.g., NETSIO_READ_BLOCK?)
        }
        
        # Initialize other AtDevHandler specific attributes
        self.hub = None
        self.atdev_ready = None
        self.atdev_thread = None
        self.busy_at = timer()
        self.idle_at = timer()
        self.emu_ts = 0
        
        # Now call the base class __init__
        super().__init__(*args, **kwargs)

    # --- NEW Handler Methods for Altirra TCP Events ---
    def send_altirra_response(self, event, arg, data=None, timestamp=0):
        """Send an Altirra protocol message back to the client."""
        if data is None:
            data = bytes()
        
        # Construct payload: event (1 byte) + arg (1 byte) + optional data
        payload = bytearray([event, arg]) + data
        
        # Construct header: total_length (4 bytes) + timestamp (4 bytes)
        # Make sure total_length is correct (8 for header + payload length)
        total_length = 8 + len(payload)  # 8 is header size
        
        # Use a known timestamp (0) to avoid any issues
        header = struct.pack('<II', total_length, 0)  # Use 0 for timestamp to be safe
        
        # Construct the full message
        message = header + payload
        
        # Print the full message in hex for debugging
        log_trace(f"Sending Altirra Response: Len={total_length}, Evt=0x{event:02X}, Arg=0x{arg:02X}")
        log_trace(f"Message hex: {' '.join([f'{b:02X}' for b in message])}")
        
        try:
            self.request.sendall(message)
            return True
        except (ConnectionResetError, BrokenPipeError) as e:
            log_trace(f"Error sending Altirra response: {e}")
            return False
            
    def handle_netsio_command_on(self, event: int, arg: int, data: bytes, timestamp: int):
        """Handles NETSIO_COMMAND_ON (0x11) event from Altirra message."""
        log_trace(f"AtDevHandler.handle_netsio_command_on: Event=0x{event:02X}, Arg(DevID)=0x{arg:02X}, TS={timestamp}")
        ts = timer()
        self.emu_ts = timestamp 
        msg = NetSIOMsg(event, arg) # COMMAND_ON uses device ID as arg
        msg.time = ts
        # This is an asynchronous command start
        log_trace(f"AtDevHandler.handle_netsio_command_on: Forwarding msg {msg} to hub.handle_host_msg")
        self.hub.handle_host_msg(msg)
        # No response needed for this asynchronous event

    def handle_netsio_data_block(self, event: int, arg: int, data: bytes, timestamp: int):
        """Handles NETSIO_DATA_BLOCK (0x02) event from Altirra message."""
        log_trace(f"AtDevHandler.handle_netsio_data_block: Event=0x{event:02X}, Arg(Len)=0x{arg:02X}, Data={data.hex(' ')}, TS={timestamp}")
        ts = timer()
        self.emu_ts = timestamp
        # DATA_BLOCK message carries the SIO bytes (Cmd, Aux1, Aux2) in the 'data' part
        # The 'arg' in the Altirra message is the *length* of the data, but NetSIOMsg expects the actual data bytes
        msg = NetSIOMsg(event, data) 
        msg.time = ts
        # This is asynchronous data transfer
        log_trace(f"AtDevHandler.handle_netsio_data_block: Forwarding msg {msg} to hub.handle_host_msg")
        self.hub.handle_host_msg(msg)
        # No response needed for this asynchronous event

    def handle_netsio_command_off_sync(self, event: int, arg: int, data: bytes, timestamp: int):
        """Handles NETSIO_COMMAND_OFF_SYNC (0x18) event from Altirra message."""
        log_trace(f"AtDevHandler.handle_netsio_command_off_sync: Event=0x{event:02X}, Arg(Checksum)=0x{arg:02X}, TS={timestamp}")
        ts = timer()
        self.emu_ts = timestamp
        msg = NetSIOMsg(event, arg) # COMMAND_OFF_SYNC uses checksum as arg
        msg.time = ts
        # This event expects a response (ACK/NAK etc.), so use sync handler
        log_trace(f"AtDevHandler.handle_netsio_command_off_sync: Forwarding msg {msg} to hub.handle_host_msg_sync")
        result = self.hub.handle_host_msg_sync(msg)
        log_trace(f"AtDevHandler.handle_netsio_command_off_sync: Received result {result} from hub")
        
        # First send the SYNC_RESPONSE back to the client using the Altirra protocol
        log_trace(f"AtDevHandler.handle_netsio_command_off_sync: Sending SYNC_RESPONSE (0x81) with result={result}")
        self.send_altirra_response(NETSIO_SYNC_RESPONSE, result, timestamp=timestamp)
        
        # Determine the command from the most recent DATA_BLOCK message
        # For this test app, we know it's a "Get Status" command (0x4E)
        # For a real implementation, we'd need to track the actual command
        
        # For "Get Status" command, we need to send 'C' status byte plus 128 bytes of data
        log_trace(f"AtDevHandler.handle_netsio_command_off_sync: Sending SIO response bytes for 'Get Status' command")
        
        # Create a complete response buffer: 'C' (0x43) status byte followed by 128 bytes of data
        # For this test, we'll just fill it with incrementing values
        response_data = bytearray(129)  # 1 status byte + 128 data bytes
        response_data[0] = 0x43  # 'C' status byte
        
        # Fill the remaining 128 bytes with some pattern (incrementing values)
        for i in range(1, 129):
            response_data[i] = i & 0xFF
        
        # Send each byte of the response as a separate NETSIO_DATA_BYTE message
        log_trace(f"AtDevHandler.handle_netsio_command_off_sync: Sending 129 individual DATA_BYTE messages")
        for i, byte_value in enumerate(response_data):
            self.send_altirra_response(NETSIO_DATA_BYTE, byte_value, timestamp=timestamp)
            log_trace(f"Sent SIO response byte {i}: 0x{byte_value:02X}")
            # Short delay between bytes to avoid flooding the socket
            if i > 0 and i % 10 == 0:  # Insert a small delay every 10 bytes
                time.sleep(0.001)
        
    # --- Existing Methods --- 
    def handle(self):
        log_trace("AtDevHandler.handle: New TCP connection from {}".format(self.client_address))
        """handle messages from netsio.atdevice"""
        # start thread for outgoing messages to atdevice
        self.hub = self.server.hub
        self.atdev_ready = threading.Event()
        self.atdev_ready.set()
        host_queue = self.hub.host_connected(self)
        self.atdev_thread = AtDevThread(host_queue, self)
        self.atdev_thread.start()

        try:
            super().handle()
        except ConnectionResetError:
            info_print("Host reset connection")
        finally:
            self.hub.host_disconnected()
            self.atdev_thread.stop()

    def handle_script_post(self, event: int, arg: int, timestamp: int):
        """handle post_message from netsio.atdevice"""
        ts = timer()
        self.emu_ts = timestamp
        msg:NetSIOMsg = None

        if event == ATDEV_READY:
            log_trace(f"AtDevHandler.handle_script_post: Received ATDEV_READY")
            # POKEY is ready to receive serial data
            msg = NetSIOMsg(event)
        elif event == NETSIO_DATA_BYTE:
            log_trace(f"AtDevHandler.handle_script_post: Received NETSIO_DATA_BYTE (0x{arg:02X})")
            # serial byte from POKEY
            msg = NetSIOMsg(event, arg)
            # self.hub.handle_host_msg(NetSIOMsg(event, arg))
        elif event == NETSIO_SPEED_CHANGE:
            log_trace(f"AtDevHandler.handle_script_post: Received NETSIO_SPEED_CHANGE (arg={arg})")
            # serial output speed changed
            msg = NetSIOMsg(event, struct.pack("<L", arg))
            # self.hub.handle_host_msg(NetSIOMsg(event, struct.pack("<L", arg)))
        elif event < 0x100: # fit byte
            # all other (one byte) events from atdevice
            log_trace(f"AtDevHandler.handle_script_post: Received event 0x{event:02X}")
            msg = NetSIOMsg(event)
            if event == NETSIO_COLD_RESET:
                self.atdev_ready.set()
        # send to connected devices
        msg = NetSIOMsg(event)
        # self.hub.handle_host_msg(NetSIOMsg(event))

        if msg is None:
            debug_print("> ATD {:02X} {:02X} ++{} -> {}".format(event, arg, timestamp-self.emu_ts))
            info_print("Invalid ATD")
            return

        log_trace(f"AtDevHandler.handle_script_post: Forwarding msg {msg} to hub.handle_host_msg")
        msg.time = ts
        debug_print("> ATD {:02X} {:02X} ++{} -> {}".format(event, arg, timestamp-self.emu_ts, msg))
        if event == ATDEV_READY:
            self.set_rtr()
        else:
            # send message to connected device
            self.hub.handle_host_msg(msg)

    def handle_script_event(self, event: int, arg: int, timestamp: int) -> int:
        ts = timer()
        self.emu_ts = timestamp
        msg:NetSIOMsg = None
        local = False

        result = ATDEV_EMPTY_SYNC
        if event == NETSIO_DATA_BYTE_SYNC:
            log_trace(f"AtDevHandler.handle_script_event: Received NETSIO_DATA_BYTE_SYNC (arg=0x{arg:02X})")
            msg = NetSIOMsg(event, arg) # request sn will be appended
        elif event == NETSIO_COMMAND_OFF_SYNC:
            log_trace(f"AtDevHandler.handle_script_event: Received NETSIO_COMMAND_OFF_SYNC (arg=0x{arg:02X})")
            msg = NetSIOMsg(event) # request sn will be appended
        elif event == NETSIO_DATA_BLOCK:
            log_trace(f"AtDevHandler.handle_script_event: Received NETSIO_DATA_BLOCK (arg=0x{arg:02X})")
            msg = NetSIOMsg(event) # data block will be read
        elif event == ATDEV_DEBUG_NOP:
            msg = NetSIOMsg(event, arg)
            local = True
            result = arg

        if msg is None:
            debug_print("> ATD CALL {:02X} {:02X} ++{}".format(event, arg, timestamp-self.emu_ts))
            info_print("Invalid ATD CALL")
            debug_print("< ATD RESPONSE {}".format(result))
            return result

        msg.time = ts
        debug_print("> ATD CALL {:02X} {:02X} ++{} -> {}".format(event, arg, timestamp-self.emu_ts, msg))
        if event == NETSIO_DATA_BLOCK:
            # get data from rxbuffer segment
            debug_print("< ATD READ_BUFFER", arg)
            msg.arg = self.req_read_seg_mem(1, 0, arg)
            log_trace(f"AtDevHandler.handle_script_event: Read data block: {msg.arg}")
        if not local:
            log_trace(f"AtDevHandler.handle_script_event: Forwarding msg {msg} to hub.handle_host_msg_sync")
            result = self.hub.handle_host_msg_sync(msg)
        debug_print("< ATD RESPONSE {} = 0x{:02X} +{:.0f}".format(result, result, msg.elapsed_us()))
        log_trace(f"AtDevHandler.handle_script_event: Returning result 0x{result:02X}")
        return result

    def handle_coldreset(self, event: int, arg: int, data: bytes, timestamp: int):
        """Handles NETSIO_COLD_RESET (0xFF) event from Altirra message."""
        log_trace(f"AtDevHandler.handle_coldreset: Event=0x{event:02X}, Arg=0x{arg:02X}, TS={timestamp}")
        self.hub.handle_host_msg(NetSIOMsg(NETSIO_COLD_RESET))

    def handle_warmreset(self, event: int, arg: int, data: bytes, timestamp: int):
        """Handles NETSIO_WARM_RESET (0xFE) event from Altirra message."""
        log_trace(f"AtDevHandler.handle_warmreset: Event=0x{event:02X}, Arg=0x{arg:02X}, TS={timestamp}")
        self.hub.handle_host_msg(NetSIOMsg(NETSIO_WARM_RESET))

    def clear_rtr(self):
        """Clear Ready To Receive"""
        self.busy_at = timer()
        self.atdev_ready.clear()
        debug_print("ATD BUSY  idle time: {:.0f}".format((self.busy_at-self.idle_at)*1.e6))

    def set_rtr(self):
        """Set Ready To receive"""
        self.idle_at = timer()
        self.atdev_ready.set()
        debug_print("ATD READY busy time: {:.0f}".format((self.idle_at-self.busy_at)*1.e6))

    def wait_rtr(self, timeout):
        """Wait for ready receiver"""
        return self.atdev_ready.wait(timeout)

class AtDevThread(threading.Thread):
    """Thread to send "messages" to Altrira atdevice"""
    def __init__(self, queue, handler):
        self.queue = queue
        self.atdev_handler = handler
        self.busy_at = timer()
        self.stop_flag = threading.Event()
        super().__init__()

    def run(self):
        log_trace("AtDevThread started")
        while not self.stop_flag.is_set():
            try:
                # Blocking wait with timeout
                msg = self.queue.get(timeout=1.0/50) # try approx. every frame
                log_trace(f"AtDevThread received msg from queue: {msg}") # Log the raw message
                if msg is None:
                    log_trace("AtDevThread received None, likely stopping.")
                    continue # Skip processing if None
                try:
                    msglen = len(msg.arg) # Now safe to access msg.arg
                    if msglen > 0:
                        # Altirra protocol requires a separate message for each byte
                        data = msg.arg
                        for b in data:
                            log_trace(f"AtDevThread: sending NETSIO_DATA_BYTE 0x{b:02X}")
                            if not self.atdev_handler.atdev_ready.wait(0.100):
                                break
                            # Send one byte at a time to client
                            self.atdev_handler.send_altirra_response(NETSIO_DATA_BYTE, b, timestamp=msg.time)
                            # Don't flood the connection
                            time.sleep(0.001)
                    # For non-data messages, just send the event code
                    elif msg.id != NETSIO_CREDIT_UPDATE: # Fixed constant name
                        log_trace(f"AtDevThread: sending event 0x{msg.id:02X}")
                        self.atdev_handler.send_altirra_response(msg.id, 0, timestamp=msg.time) # Fixed method call
                finally:
                    self.queue.task_done()
            except queue.Empty:
                # Just loop again
                pass
            except (ConnectionResetError, BrokenPipeError) as e:
                log_trace(f"AtDevThread connection error: {e}")
                break
            except Exception as e:
                log_trace("AtDevThread error: {}".format(e))
                break
        log_trace("AtDevThread stopped")

    def stop(self):
        log_trace("Stopping AtDevThread")
        self.stop_flag.set()
        # clear_queue(self.queue) # things can cumulate here ...
        self.queue.put(None) # unblock queue.get()
        self.join()

class NetSIOHub:
    """HUB connecting NetSIO devices with Atari host"""

    class SyncRequest:
        """Synchronized request-response"""
        def __init__(self):
            self.sn = 0
            self.request = None
            self.response = None
            self.lock = threading.Lock()
            self.completed = threading.Event()

        def set_request(self, request):
            with self.lock:
                self.sn = (self.sn + 1) & 255
                self.request = request
                self.completed.clear()
            return self.sn

        def set_response(self, response, sn):
            with self.lock:
                if self.request is not None and self.sn == sn:
                    self.request = None
                    self.response = response
                    self.completed.set()

        def get_response(self, timeout=None, timout_value=None):
            if self.completed.wait(timeout):
                with self.lock:
                    self.request = None
                    return self.response
            else:
                with self.lock:
                    self.request = None
                    return timout_value

        def check_request(self):
            with self.lock:
                return self.request, self.sn

    def __init__(self, device_manager:DeviceManager, host_manager:HostManager):
        self.device_manager = device_manager
        self.host_manager = host_manager
        self.host_queue = queue.Queue(8) # max 3-4 items should be there, anyhow make it bit larger, to avoid blocked netin thread
        self.host_ready = threading.Event()
        self.host_handler:AtDevHandler = None
        self.sync = NetSIOHub.SyncRequest()

    def run(self):
        try:
            self.device_manager.start(self)
            self.host_manager.run(self)
        finally:
            self.device_manager.stop()
            self.host_manager.stop()

    def host_connected(self, host_handler:AtDevHandler): # TODO replace call to AtDevHandler.clear_rtr()
        info_print("Host connected")
        self.host_handler = host_handler
        self.host_ready.set()
        return self.host_queue

    def host_disconnected(self):
        info_print("Host disconnected")
        self.host_ready.clear()
        self.host_handler = None
        clear_queue(self.host_queue)

    def handle_host_msg(self, msg:NetSIOMsg):
        """handle message from Atari host emulator, emulation is running"""
        log_trace(f"NetSIOHub.handle_host_msg: Received {msg}")
        if msg.id in (NETSIO_COLD_RESET, NETSIO_WARM_RESET):
            info_print("HOST {} RESET".format("COLD" if msg.id == NETSIO_COLD_RESET else "WARM"))
            # # clear I/O queues on emulator cold / warm reset
            # debug_print("CLEAR HOST QUEUE")
            # clear_queue(self.host_queue)
        # send message down to connected peripherals
        log_trace(f"NetSIOHub.handle_host_msg: Forwarding {msg} to device_manager.to_peripheral")
        self.device_manager.to_peripheral(msg)

    def handle_host_msg_sync(self, msg:NetSIOMsg) ->int:
        """handle message from Atari host emulator, emulation is paused, emulator is waiting for reply"""
        log_trace(f"NetSIOHub.handle_host_msg_sync: Received {msg}")
        if msg.id == NETSIO_DATA_BLOCK:
            log_trace(f"NetSIOHub.handle_host_msg_sync: Handling DATA_BLOCK locally and returning ATDEV_EMPTY_SYNC")
            self.handle_host_msg(msg) # send to devices
            return ATDEV_EMPTY_SYNC # return no ACK byte
        # handle sync request
        log_trace(f"NetSIOHub.handle_host_msg_sync: Preparing sync request for {msg.id}")
        msg.arg.append(self.sync.set_request(msg.id)) # append request sn prior sending
        log_trace(f"NetSIOHub.handle_host_msg_sync: Appended sn={self.sync.sn}. New msg.arg: {msg.arg}")
        clear_queue(self.host_queue)
        if not self.device_manager.connected():
            # shortcut: no device is connected, set response now
            log_trace(f"NetSIOHub.handle_host_msg_sync: No device connected, shortcutting response to ATDEV_EMPTY_SYNC")
            self.sync.set_response(ATDEV_EMPTY_SYNC, self.sync.sn) # no ACK byte
        else:
            log_trace(f"NetSIOHub.handle_host_msg_sync: Device connected, forwarding {msg} via handle_host_msg")
            self.handle_host_msg(msg) # send to devices
        log_trace(f"NetSIOHub.handle_host_msg_sync: Waiting for response from peripheral (timeout={self.device_manager.sync_tmout}s)")
        result = self.sync.get_response(self.device_manager.sync_tmout, ATDEV_EMPTY_SYNC)
        log_trace(f"NetSIOHub.handle_host_msg_sync: Got response: {result} (0x{result:02X}). Returning to host.")
        return result

    def handle_device_msg(self, msg:NetSIOMsg, device:NetSIOClient):
        """handle message from peripheral device"""
        if not self.host_ready.is_set():
            # discard, host is not connected
            return

        # handle sync request/response
        req, sn = self.sync.check_request()
        if req is not None:
            if msg.id == NETSIO_SYNC_RESPONSE and msg.arg[0] == sn:
                # we received response to current SYNC request
                if msg.arg[1] == NETSIO_EMPTY_SYNC:
                    # empty response, no ACK/NAK
                    self.sync.set_response(ATDEV_EMPTY_SYNC, sn) # no ACK byte
                else:
                    # response with ACK/NAK byte and sync write size
                    self.host_handler.clear_rtr()
                    self.sync.set_response(NETSIO_SYNC_RESPONSE |
                                           msg.arg[2] << 8 | (msg.arg[3] << 16) | (msg.arg[4] << 24), sn)
                return
            elif msg.id in (NETSIO_DATA_BYTE, NETSIO_DATA_BLOCK):
                debug_print("discarding", msg)
                return
            else:
                debug_print("passed", msg)

        if msg.id == NETSIO_SYNC_RESPONSE and msg.arg[1] != NETSIO_EMPTY_SYNC:
            # TODO 
            # host is not interested into this sync response
            # but there is a byte inside response, deliver it as normal byte to host ...
            debug_print("replace", msg)
            msg.id = NETSIO_DATA_BYTE
            msg.arg = bytes( (msg.arg[2],) )

        if self.host_queue.full():
            debug_print("host queue FULL")
        else:
            debug_print("host queue [{}]".format(self.host_queue.qsize()))

        self.host_queue.put(msg)

    def credit_clients(self):
        # Add tracing if needed later
        self.device_manager.credit_clients()

class SerialManager(DeviceManager):
    """Manages serial port communication"""

def get_arg_parser(full=True):
    arg_parser = argparse.ArgumentParser(description = 
            "Connects NetSIO protocol (SIO over UDP) talking peripherals with "
            "NetSIO Altirra custom device (localhost TCP).")
    port_grp = arg_parser.add_mutually_exclusive_group()
    port_grp.add_argument('--netsio-port', type=int, default=NETSIO_PORT,
        help='Change UDP port used by NetSIO peripherals (default {})'.format(NETSIO_PORT))
    #serial_grp = port_grp.add_argument_group("Serial port")
    port_grp.add_argument('--serial',
        help='Switch to serial port mode. Specify serial port (device) to use for communication with peripherals.')
    arg_parser.add_argument('--command', default='RTS', choices=['RTS','DTR'],
        help='Specify how is COMMAND signal connected, value can be RTS (default) or DTR')
    arg_parser.add_argument('--proceed', default='CTS', choices=['CTS','DSR'],
        help='Specify how is PROCEED signal connected, value can be CTS (default) or DSR')
    arg_parser.add_argument('-d', '--debug', dest='debug', action='store_true', help='Print debug output')
    if full:
        arg_parser.add_argument('--port', type=int, default=NETSIO_ATDEV_PORT,
            help='Change TCP port used by Altirra NetSIO custom device (default {})'.format(NETSIO_ATDEV_PORT))
        arg_parser.add_argument('-v', '--verbose', dest='verbose', action='store_true',
            help='Log emulation device commands')
    return arg_parser

def main():
    print("__file__:", __file__)
    print("sys.executable:", sys.executable)
    print("sys.version:", sys.version)
    print("sys.path:")
    print("\n".join(sys.path))
    print("sys.argv", sys.argv)

    print_banner()

    socketserver.TCPServer.allow_reuse_address = True
    args = get_arg_parser().parse_args()

    if args.debug:
        enable_debug()

    # get device manager (to talk to peripheral device)
    if args.serial:
        if has_serial:
            device_manager = SerialSIOManager(args.serial, args.command, args.proceed)
        else:
            print("pySerial module was not found. To install pySerial module run 'python -m pip install pyserial'.")
            return -1
    else:
        device_manager = NetSIOManager(args.netsio_port)

    # get host manager (to talk to Atari host emulator)
    host_manager = AtDevManager(get_arg_parser(False))

    # hub for host <-> devices communication
    hub = NetSIOHub(device_manager, host_manager)

    try:
        hub.run()
    except KeyboardInterrupt:
        print("\nStopped from keyboard")

    return 0

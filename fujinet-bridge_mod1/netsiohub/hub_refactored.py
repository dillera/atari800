"""
NetSIO Hub Implementation

This module contains the main NetSIOHub class which coordinates communication
between the Atari800 emulator and peripheral devices.
"""

import socket
import threading
import queue
import time
import argparse

# Import from our refactored modules
from netsiohub.netsio_protocol import *
from netsiohub.netsio_client import NetSIOClient
from netsiohub.netsio_server import NetSIOServer, NetSIOHandler
from netsiohub.device_manager import NetSIOManager
from netsiohub.atari_device import AtDevManager, AtDevHandler, AtDevThread

class NetSIOHub:
    """Main hub class to coordinate communication between Atari800 emulator and peripheral devices"""
    
    def __init__(self):
        """initialize hub"""
        # Host connection (Atari800 emulator)
        self.host_handler = None
        self.host_queue = queue.Queue()
        self.host_thread = None
        
        # Synchronization manager
        self.sync = SyncManager()
        
        # SIO command reconstruction state
        self.pending_sio_command = None
        
        # Peripheral device manager
        self.device_manager = NetSIOManager(self)
        
        # Atari device manager
        self.atari_manager = AtDevManager(self)
        self.atari_manager.set_handler_class(AtDevHandler)
        
        # UDP server for peripheral communications
        self.udp_server = None
        
    def start(self, tcp_host, tcp_port, udp_host, udp_port):
        """start hub services"""
        info_print(f"Starting NetSIO Hub")
        info_print(f"Atari host (TCP): {tcp_host}:{tcp_port}")
        info_print(f"Peripheral devices (UDP): {udp_host}:{udp_port}")
        
        # Start TCP server for Atari800 emulator
        self.atari_manager.start_server(tcp_host, tcp_port)
        
        # Start UDP server for peripheral devices
        self.udp_server = NetSIOServer((udp_host, udp_port), NetSIOHandler, self)
        self.udp_server.start()
        
        # Start host communication thread
        self.start_host_thread()
    
    def stop(self):
        """stop hub services"""
        info_print("Stopping NetSIO Hub")
        
        # Stop host thread
        if self.host_thread is not None:
            self.host_thread.stop()
            self.host_thread = None
        
        # Stop UDP server
        if self.udp_server is not None:
            self.udp_server.stop()
            self.udp_server = None
        
        # Clear host handler reference
        self.host_handler = None
        clear_queue(self.host_queue)
    
    def start_host_thread(self):
        """start thread for sending messages to Atari800 emulator"""
        if self.host_thread is None:
            self.host_thread = AtDevThread(self)
            self.host_thread.start()
    
    def register_host_handler(self, handler):
        """register TCP handler for Atari800 emulator"""
        info_print(f"Atari800 emulator connected from {handler.client_address}")
        self.host_handler = handler
        clear_queue(self.host_queue)
    
    def unregister_host_handler(self, handler):
        """unregister TCP handler for Atari800 emulator"""
        if self.host_handler == handler:
            info_print(f"Atari800 emulator disconnected from {handler.client_address}")
            self.host_handler = None
            clear_queue(self.host_queue)
    
    def handle_peripheral_msg(self, msg:NetSIOMsg, peripheral:NetSIOClient):
        """handle message from peripheral device"""
        log_trace(f"NetSIOHub.handle_peripheral_msg: Received message ID=0x{msg.id:02X} from {peripheral.addr}")
        
        # Register the peripheral client if first message
        if msg.id == NETSIO_HELLO:
            self.device_manager.register_peripheral(peripheral)
            log_trace(f"Registered peripheral device at {peripheral.addr}")
            return
        
        # Handle message based on type
        if msg.id == NETSIO_COMMAND_ACK or msg.id == NETSIO_DATA_ACK:
            # ACK message is a response to a sync command
            result = msg.arg if msg.arg is not None else 0
            self.sync.set_response(result, msg.id)
        else:
            # Forward other messages to host
            self.to_host(msg)
    
    def to_host(self, msg:NetSIOMsg):
        """forward message to Atari800 emulator"""
        if self.host_handler is not None:
            self.host_queue.put(msg)
        else:
            log_trace(f"NetSIOHub.to_host: No host connected, dropping message ID=0x{msg.id:02X}")
    
    def handle_host_msg(self, msg:NetSIOMsg):
        """handle message from Atari host emulator, emulation is running"""
        log_trace(f"NetSIOHub.handle_host_msg: Received message ID=0x{msg.id:02X}")
        
        # Detailed logging based on message type
        if msg.id == NETSIO_COMMAND_ON:
            device_id = msg.arg[0] if isinstance(msg.arg, (bytes, bytearray)) and len(msg.arg) > 0 else msg.arg
            log_trace(f"NetSIOHub.handle_host_msg: COMMAND_ON for device 0x{device_id:02X}")
        elif msg.id == NETSIO_DATA_BLOCK:
            if isinstance(msg.arg, (bytes, bytearray)):
                data_hex = ' '.join([f'{b:02X}' for b in msg.arg])
                log_trace(f"NetSIOHub.handle_host_msg: DATA_BLOCK with data: {data_hex}")
                
                # If this is a 5-byte block, it might be a full SIO command frame
                if len(msg.arg) == 5:
                    log_trace(f"NetSIOHub.handle_host_msg: Possible SIO command frame detected: {data_hex}")
                    log_sio_command(msg.arg[0], msg.arg[1], msg.arg[2], msg.arg[3], msg.arg[4])
        elif msg.id == NETSIO_COMMAND_OFF_SYNC:
            # Log forwarding of COMMAND_OFF_SYNC, including payload (checksum)
            checksum_val = msg.arg[0] if isinstance(msg.arg, (list, bytearray, bytes)) and len(msg.arg) > 0 else None
            sync_num = msg.arg[1] if isinstance(msg.arg, (list, bytearray, bytes)) and len(msg.arg) > 1 else None # Original SN from emulator

            if checksum_val is not None:
                checksum_str = f"0x{checksum_val:02X}"
            else:
                checksum_str = 'N/A'
            log_trace(f"NetSIOHub.handle_host_msg: COMMAND_OFF_SYNC (SyncNum={sync_num}, Checksum={checksum_str})")
        # Add other message types if needed
        else:
            log_trace(f"NetSIOHub.handle_host_msg: Message ID=0x{msg.id:02X}")
        
        if msg.id in (NETSIO_COLD_RESET, NETSIO_WARM_RESET):
            info_print("HOST {} RESET".format("COLD" if msg.id == NETSIO_COLD_RESET else "WARM"))
        
        # --- SIO Command Reconstruction Logic ---
        if msg.id == NETSIO_COMMAND_ON:
            # Start of an SIO command sequence
            dev_id = msg.arg # Arg holds the Device ID
            
            # Fix any bytearray data type issues
            if isinstance(dev_id, (bytearray, bytes)) and len(dev_id) > 0:
                dev_id_int = dev_id[0]
                self.pending_sio_command = [dev_id_int]
                log_trace(f"NetSIOHub.handle_host_msg: Stored and fixed SIO DevID 0x{dev_id_int:02X}")
            else:
                self.pending_sio_command = [dev_id]
                log_trace(f"NetSIOHub.handle_host_msg: Stored SIO DevID 0x{dev_id:02X}")
            
            # Forward original COMMAND_ON message to maintain signal timing
            self.device_manager.to_peripheral(msg)
            return
        elif msg.id == NETSIO_DATA_BLOCK:
            if self.pending_sio_command is not None and len(self.pending_sio_command) == 1:
                # We have DevID, now get Cmd, Aux1, Aux2
                # Expecting data = [Cmd, Aux1, Aux2]
                if msg.arg and len(msg.arg) == 3:
                    cmd, aux1, aux2 = msg.arg[0], msg.arg[1], msg.arg[2]
                    self.pending_sio_command.extend([cmd, aux1, aux2])
                    log_trace(f"NetSIOHub.handle_host_msg: Stored SIO Cmd=0x{cmd:02X}, Aux1=0x{aux1:02X}, Aux2=0x{aux2:02X}")
                    
                    # Forward original DATA_BLOCK message to maintain signal timing
                    self.device_manager.to_peripheral(msg)
                    return
                else:
                    log_warning(f"NetSIOHub.handle_host_msg: Received DATA_BLOCK with unexpected data length {len(msg.arg) if msg.arg else 0} during SIO command reconstruction. Discarding.")
                    self.pending_sio_command = None # Reset state
                    # Forward message anyway since we're breaking the sequence
                    self.device_manager.to_peripheral(msg)
                    return
            else:
                # DATA_BLOCK received out of sequence, treat as normal message
                log_trace(f"NetSIOHub.handle_host_msg: Received DATA_BLOCK outside SIO sequence.")
                if self.pending_sio_command is not None:
                    log_trace(f"NetSIOHub.handle_host_msg: Resetting pending SIO command state.")
                    self.pending_sio_command = None # Reset state
                # Fall through to default forwarding
        elif self.pending_sio_command is not None:
            # Received a different message while waiting for DATA_BLOCK or COMMAND_OFF_SYNC
            log_warning(f"NetSIOHub.handle_host_msg: Received unexpected message ID=0x{msg.id:02X} during SIO command reconstruction. Resetting state.")
            self.pending_sio_command = None # Reset state
            # Fall through to default forwarding
        # --- End SIO Command Reconstruction Logic ---

        # Default behavior: Forward non-SIO-sequence messages immediately
        log_trace(f"NetSIOHub.handle_host_msg: Forwarding message ID=0x{msg.id:02X} to peripherals")
        self.device_manager.to_peripheral(msg)

    def handle_host_msg_sync(self, msg:NetSIOMsg) ->int:
        """handle message from Atari host emulator, emulation is paused, emulator is waiting for reply"""
        log_trace(f"NetSIOHub.handle_host_msg_sync: Received sync message ID=0x{msg.id:02X}")

        # --- SIO Command Reconstruction Logic ---
        if msg.id == NETSIO_COMMAND_OFF_SYNC:
            if self.pending_sio_command is not None and len(self.pending_sio_command) == 4:
                # We have DevID, Cmd, Aux1, Aux2. Now get Checksum.
                checksum = msg.arg[0] if isinstance(msg.arg, (list, bytearray, bytes)) and len(msg.arg) > 0 else None
                original_sync_num = msg.arg[1] if isinstance(msg.arg, (list, bytearray, bytes)) and len(msg.arg) > 1 else None # Original SN from emulator

                if checksum is not None:
                    self.pending_sio_command.append(checksum)
                    
                    # Create a proper SIO frame from our collected data
                    # Ensure all items are integers for bytes() conversion
                    sio_frame = []
                    for item in self.pending_sio_command:
                        if isinstance(item, (bytearray, bytes)):
                            if len(item) > 0:
                                sio_frame.append(item[0])
                            else:
                                sio_frame.append(0)
                        else:
                            sio_frame.append(item)
                    
                    # Convert to bytes for logging
                    sio_frame_bytes = bytes(sio_frame)
                    log_trace(f"NetSIOHub.handle_host_msg_sync: Reconstructed SIO Frame: {' '.join(f'{b:02X}' for b in sio_frame_bytes)}")
                    
                    # Reset state *before* sending to avoid state corruption
                    self.pending_sio_command = None 
                    
                    # Set up the sync request for the original message
                    sync_sn = self.sync.set_request(msg.id)
                    log_trace(f"NetSIOHub.handle_host_msg_sync: Set sync request SN={sync_sn} for COMMAND_OFF_SYNC.")
                    
                    clear_queue(self.host_queue) # Clear queue before sync wait
                    if not self.device_manager.connected():
                        log_warning(f"NetSIOHub.handle_host_msg_sync: No device connected during SIO sync. Returning NAK.")
                        self.sync.set_response(ATDEV_NAK_RESPONSE, sync_sn) # Peripheral disconnected
                    else:
                        # Forward the original COMMAND_OFF_SYNC to maintain proper signal timing
                        log_trace(f"NetSIOHub.handle_host_msg_sync: Forwarding original COMMAND_OFF_SYNC to peripheral.")
                        self.device_manager.to_peripheral(msg)

                    log_trace(f"NetSIOHub.handle_host_msg_sync: Waiting for SIO sync response (timeout={self.device_manager.sync_tmout}s)")
                    result = self.sync.get_response(self.device_manager.sync_tmout, ATDEV_NAK_RESPONSE) # Default to NAK on timeout
                    log_trace(f"NetSIOHub.handle_host_msg_sync: Got SIO sync response: 0x{result:02X}. Returning to host.")
                    return result
                else:
                    log_warning(f"NetSIOHub.handle_host_msg_sync: Received COMMAND_OFF_SYNC with invalid checksum data during SIO reconstruction. Resetting state.")
                    self.pending_sio_command = None # Reset state
                    return ATDEV_NAK_RESPONSE # Indicate error to emulator
            else:
                # COMMAND_OFF_SYNC received out of sequence
                log_warning(f"NetSIOHub.handle_host_msg_sync: Received COMMAND_OFF_SYNC outside expected SIO sequence. Resetting state and returning NAK.")
                if self.pending_sio_command is not None:
                    log_warning(f"NetSIOHub.handle_host_msg_sync: Pending command state was: {self.pending_sio_command}")
                    self.pending_sio_command = None # Reset state
                return ATDEV_NAK_RESPONSE # Indicate error to emulator
        # --- End SIO Command Reconstruction Logic ---

        # Handle DATA_BYTE_SYNC message
        if msg.id == NETSIO_DATA_BYTE_SYNC:
            log_trace(f"NetSIOHub.handle_host_msg_sync: DATA_BYTE_SYNC with value: 0x{msg.arg:02X}")
            sync_sn = self.sync.set_request(msg.id)
            log_trace(f"NetSIOHub.handle_host_msg_sync: Set sync request SN={sync_sn} for DATA_BYTE_SYNC.")
            clear_queue(self.host_queue)
            if not self.device_manager.connected():
                self.sync.set_response(ATDEV_EMPTY_SYNC, sync_sn)
            else:
                self.device_manager.to_peripheral(msg) # Forward original message
            result = self.sync.get_response(self.device_manager.sync_tmout, ATDEV_EMPTY_SYNC)
            log_trace(f"NetSIOHub.handle_host_msg_sync: Got DATA_BYTE_SYNC response: 0x{result:02X}. Returning to host.")
            return result

        # Default for unexpected sync messages
        log_warning(f"NetSIOHub.handle_host_msg_sync: Unhandled sync message ID=0x{msg.id:02X}. Returning NAK.")
        return ATDEV_NAK_RESPONSE # Indicate error


# Main entry point when run as module
def main():
    """main function when run as module"""
    # Parse command line arguments
    parser = argparse.ArgumentParser(description='NetSIO Hub for Atari800 emulator and peripherals')
    parser.add_argument('--tcp-host', dest='tcp_host', default='127.0.0.1',
                        help='TCP host address for Atari800 emulator (default: 127.0.0.1)')
    parser.add_argument('--tcp-port', dest='tcp_port', type=int, default=9996,
                        help='TCP port for Atari800 emulator (default: 9996)')
    parser.add_argument('--udp-host', dest='udp_host', default='0.0.0.0',
                        help='UDP host address for peripheral devices (default: 0.0.0.0)')
    parser.add_argument('--udp-port', dest='udp_port', type=int, default=9997,
                        help='UDP port for peripheral devices (default: 9997)')
    args = parser.parse_args()
    
    # Create and start hub
    hub = NetSIOHub()
    hub.start(args.tcp_host, args.tcp_port, args.udp_host, args.udp_port)
    
    # Wait for KeyboardInterrupt
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        info_print("Received KeyboardInterrupt, shutting down")
    
    # Stop hub
    hub.stop()
    
if __name__ == '__main__':
    main()

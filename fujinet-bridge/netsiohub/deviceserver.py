# Device server for Altirra custom devices (V2 / Altirra 4.0 protocol)
# Copyright (C) 2020 Avery Lee, All rights reserved.
#
# This software is provided 'as-is', without any express or implied
# warranty.  In no event will the authors be held liable for any
# damages arising from the use of this software.
# 
# Permission is granted to anyone to use this software for any purpose,
# including commercial applications, and to alter it and redistribute it
# freely, subject to the following restrictions:
# 
# 1. The origin of this software must not be misrepresented; you must
#    not claim that you wrote the original software. If you use this
#    software in a product, an acknowledgment in the product
#    documentation would be appreciated but is not required.
# 2. Altered source versions must be plainly marked as such, and must
#    not be misrepresented as being the original software.
# 3. This notice may not be removed or altered from any source
#    distribution.


import socketserver
import struct
import signal
import sys
import argparse
import binascii
import logging
import socket
import time
# from deviceserverframework import DeviceTCPHandler, DeviceServerFactory, run_deviceserver_with_factory

class DeviceSegment:
    """
    Proxy for Segment variables in the device script.
    """

    __slots__ = ['handler', 'segment_index']

    def __init__(self, handler, segment_index):
        self.handler = handler
        self.segment_index = segment_index

    def read(self, offset: int, len: int):
        """
        Read data from [offset:offset+len] in the segment.
        """
        return self.handler.req_read_seg_mem(self.segment_index, offset, len)

    def write(self, offset: int, data:bytes):
        """
        Write data to [offset:offset+len] in the segment.
        """
        return self.handler.req_write_seg_mem(self.segment_index, offset, data)

    def fill(self, offset: int, val: int, len: int):
        """
        Fill [offset:offset+len] with byte val.
        """

        return self.handler.req_fill_seg_mem(self.segment_index, offset, val, len)

    def copy(self, dst_offset: int, src_segment: 'DeviceSegment', src_offset: int, len: int):
        """
        Copy src_segment[src_offset:src_offset+len] to dst_offset[dst_offset:dst_offset+len].
        """

        return self.handler.req_copy_seg_mem(self.segment_index, dst_offset, src_segment.segment_index, src_offset, len)

class DeviceMemoryLayer:
    """
    Proxy for MemoryLayer variables in the device script.
    """

    __slots__ = ['handler', 'layer_index']

    def __init__(self, handler, layer_index):
        self.handler = handler
        self.layer_index = layer_index

    def enable(self, read: bool, write: bool):
        """
        Enable or disable the memory layer for read and write memory accesses.
        """

        self.handler.req_enable_layer(self.layer_index, read, write)

    def set_offset(self, offset: int):
        """
        Set the starting byte offset of a direct mapped layer within the mapped segment.
        """

        if offset < 0 or offset & 255:
            raise ValueError('Invalid segment offset')

        self.handler.req_set_layer_offset(self.layer_index, offset)

    def set_segment_and_offset(self, segment: DeviceSegment, segment_offset: int):
        """
        Set the mapped segment and starting byte offset for a direct mapped layer.
        """

        if segment_offset < 0 or segment_offset & 255:
            raise ValueError('Invalid segment offset')

        self.handler.req_set_layer_segment_and_offset(self.layer_index, segment.segment_index, segment_offset)

    def set_readonly(self, ro: bool):
        """
        Set the read-only status of a layer. If a layer is read-only, writes to
        the segment are blocked from lower priority segments but the segment
        is not changed. This can only be applied to direct-mapped layers and not
        control layers.
        """

        self.handler.req_set_layer_readonly(self.layer_index, ro)

class DeviceTCPHandler(socketserver.BaseRequestHandler):
    """
    Base socketserver handler for implementing the custom device server
    protocol. You should subclass this type in your own code and implement
    handle_*() methods. Use req_*() methods or the methods on the reflected
    device objects to call back into the emulator.

    For convenience, memory layers and segment variables are reflected from the
    device script back into this handler to allow them to be referred to by name,
    isolating the server from some changes in the device script. A segment variable
    'foo' becomes self.seg_foo, and a memory layer 'bar' becomes self.layer_bar.
    """

    def __init__(self, *args, **kwargs):
        self.verbose = False
        self.handlers = {};

        self.handlers[0] = ("None", self.handle_none)
        self.handlers[1] = ("Debug read byte", self.wrap_debugreadbyte)
        self.handlers[2] = ("Read byte", self.wrap_readbyte)
        self.handlers[3] = ("Write byte", self.wrap_writebyte)
        self.handlers[4] = ("Cold reset", self.wrap_coldreset)
        self.handlers[5] = ("Warm reset", self.wrap_warmreset)
        self.handlers[6] = ("Error", self.handle_error)
        self.handlers[7] = ("Script event", self.wrap_script_event)
        self.handlers[8] = ("Script post", self.handle_script_post)
        # Add direct handlers for FujiNet SIO commands
        self.handlers[0x53] = ("NETSIO_DATA_BYTE_SYNC", self.handle_netsio_data_byte_sync)
        self.handlers[0x5F] = ("NETSIO_COMMAND_OFF_SYNC", self.handle_netsio_command_off_sync)

        self.counter = 0
        
        # Initialize SIO command tracking
        self.last_sio_command = None
        self.last_sio_device_id = 0
        self.debug_mode = True  # Enable detailed debugging
        self.last_sio_aux1 = 0
        self.last_sio_aux2 = 0

        super().__init__(*args, **kwargs)

    def handle(self):
        """
        Main protocol service loop.
        """
        print("\n***** DEVICESERVER.PY HANDLE METHOD CALLED *****")
        print(f"***** HANDLER TYPE: {self.__class__.__name__} *****")
        print(f"***** CLIENT IP: {self.client_address} *****\n")
        
        self.verbose = self.server.cmdline_args.verbose
        print("Connection received from emulator - ready to handle NetSIO messages")
        
        # For tracking SIO command state across multiple packets
        self.sio_command_buffer = bytearray()
        self.current_device_id = 0
        self.current_command = None
        
        # Increase timeout for debugging
        self.request.settimeout(60)  # 60 second timeout
        
        while True:
            print("\n===== STARTING NEW PACKET HANDLING LOOP =====")
            # Read the header (17 bytes)
            command_packet = bytearray()
            while len(command_packet) < 17:
                command_subpacket = self.request.recv(17 - len(command_packet))
                if len(command_subpacket) == 0:
                    print("Connection closed")
                    return
                command_packet.extend(command_subpacket)

            print(f"<- ATD RAW PACKET: {binascii.hexlify(command_packet).decode('ascii')}")
            packet_id, param1, param2, timestamp = struct.unpack('<BIIQ', command_packet)
            print(f"<- ATD UNPACKED: ID=0x{packet_id:02X}, P1=0x{param1:08X}, P2=0x{param2:08X}, TS={timestamp}")
            
            # Read payload data immediately for commands that expect it
            payload_data = None
            if packet_id == 0x02:  # DATA_BLOCK - read param2 bytes of data
                payload_size = param2
                if payload_size > 0:
                    try:
                        payload_data = self.request.recv(payload_size)
                        if len(payload_data) < payload_size:
                            print(f"Error: Incomplete payload received. Expected {payload_size}, got {len(payload_data)}")
                            return
                        print(f"   Read DATA_BLOCK payload ({len(payload_data)} bytes): {payload_data.hex()}")
                    except socket.timeout:
                        print("Error: Timeout reading payload data")
                        return
                    except socket.error as e:
                        print(f"Error reading payload data: {e}")
                        return
            elif packet_id == 0x18:  # COMMAND_OFF_SYNC - read 1 byte checksum
                try:
                    payload_data = self.request.recv(1)
                    if len(payload_data) < 1:
                        print(f"Error: Missing checksum for COMMAND_OFF_SYNC")
                        return
                    print(f"   Read COMMAND_OFF_SYNC checksum: {payload_data.hex()}")
                except socket.timeout:
                    print("Error: Timeout reading checksum")
                    return
                except socket.error as e:
                    print(f"Error reading checksum: {e}")
                    return
            elif param2 > 0:  # Any other command with payload size > 0
                try:
                    payload_data = self.request.recv(param2)
                    if len(payload_data) < param2:
                        print(f"Error: Incomplete payload received. Expected {param2}, got {len(payload_data)}")
                        return
                    print(f"   Read generic payload ({len(payload_data)} bytes): {payload_data.hex()}")
                except socket.timeout:
                    print("Error: Timeout reading payload data")
                    return
                except socket.error as e:
                    print(f"Error reading payload data: {e}")
                    return
            
            # Process special SIO events (0x11, 0x02, 0x18)
            if packet_id == 0x11:  # COMMAND_ON (Start of SIO frame)
                print("\n***** RECEIVED SIO COMMAND_ON (0x11) *****")
                # Reset state for new command
                self.sio_command_buffer.clear()
                self.current_device_id = param2
                print(f"***** SIO Event: COMMAND_ON for device 0x{param2:02X} *****")
            elif packet_id == 0x02 and payload_data:  # DATA_BLOCK (SIO command bytes)
                print("\n***** RECEIVED SIO DATA_BLOCK (0x02) *****")
                # Store command data for later processing
                self.sio_command_buffer = bytearray(payload_data)
                if len(self.sio_command_buffer) >= 3:
                    cmd = self.sio_command_buffer[0]
                    aux1 = self.sio_command_buffer[1]
                    aux2 = self.sio_command_buffer[2]
                    self.current_command = cmd
                    print(f"***** SIO Event: DATA_BLOCK with CMD=0x{cmd:02X}, AUX1=0x{aux1:02X}, AUX2=0x{aux2:02X} *****")
            elif packet_id == 0x18:  # COMMAND_OFF_SYNC (End of SIO frame, wait for sync response)
                print("\n***** RECEIVED SIO COMMAND_OFF_SYNC (0x18) *****")
                # Process the complete SIO command
                sync_number_request = param2
                sio_checksum = payload_data[0] if payload_data else None
                
                checksum_str = f"{sio_checksum:02X}" if sio_checksum is not None else 'N/A'
                print(f"***** SIO Event: COMMAND_OFF_SYNC received. SyncReq={sync_number_request}, Checksum={checksum_str} *****")
                
                # Handle the SIO command response
                if self.current_command == 0x53:  # 'S' Status command
                    print("\n***** PROCESSING STATUS COMMAND (0x53) *****")
                    self.respond_sio_status(self.current_device_id, sync_number_request)
                    print("***** STATUS COMMAND PROCESSING COMPLETE *****\n")
                else:
                    # Default response for unhandled commands (NAK)
                    print(f"\n***** UNHANDLED COMMAND: 0x{self.current_command:02X if self.current_command else 0:02X} - SENDING NAK *****")
                    self.respond_sio_nak(sync_number_request)
                    print("***** NAK RESPONSE COMPLETE *****\n")
                
                # Reset state after command is processed
                self.current_command = None
                continue
            
            # Normal packet processing for non-SIO events
            try:
                command_name, handler = self.handlers[packet_id]
            except KeyError:
                print(f"Unhandled command: {packet_id:02X}")
                continue
            
            if self.verbose:
                print(f"Calling Handler: {command_name}")

            handler(param1, param2, timestamp)
    
    def respond_sio_status(self, device_id, sync_number_request):
        """Handles sending the specific Altirra response sequence for an SIO Status (0x53) command."""
        print(f"Responding to STATUS command for device 0x{device_id:02X} with sync #{sync_number_request}")
        
        # Step 1: Create and send ACK response (event 0x01, arg is the device ID)
        ack_response = self.create_altirra_response(0x01, device_id, b'\x41')  # 'A' for ACK
        self.send_response(ack_response)
        print(f"Sent ACK response for device 0x{device_id:02X}")
        
        # Step 2: Create fake disk status data (129 bytes: 1 byte status code + 128 bytes sector data)
        # Status byte 0xFF = read only disk
        status_data = bytearray(129)  # Initialize all zeros
        status_data[0] = 0xFF  # First byte is status (0xFF = read only)
        status_data[1] = 0x01  # Mark drive as ON
        
        # Step 3: Create and send COMPLETE response with the status data
        complete_response = self.create_altirra_response(0x03, device_id, status_data)
        self.send_response(complete_response)
        print(f"Sent COMPLETE response with {len(status_data)} bytes of status data")
        
        # Step 4: Create and send SYNC response with the requested sync number
        sync_response = self.create_altirra_response(0x10, sync_number_request, b'')
        self.send_response(sync_response)
        print(f"Sent SYNC response #{sync_number_request}")
    
    def respond_sio_nak(self, sync_number_request, nak_code='N'):
        """Sends a NAK response followed by a SYNC response."""
        # Create NAK response
        nak_data = nak_code.encode('ascii')
        nak_response = self.create_altirra_response(0x01, 0, nak_data)
        self.send_response(nak_response)
        print(f"Sent NAK response: {nak_code}")
        
        # Create SYNC response
        sync_response = self.create_altirra_response(0x10, sync_number_request, b'')
        self.send_response(sync_response)
        print(f"Sent SYNC response #{sync_number_request}")
    
    def create_altirra_response(self, event, arg, data):
        """Formats a response packet for Altirra."""
        # Create header (17 bytes)
        header = struct.pack('<BIIQ', event, 0, arg, 0)
        
        # Convert data to bytes if it's not already
        if isinstance(data, bytearray):
            data = bytes(data)
        
        # Combine header and data
        response = header + data
        print(f"Created response packet: Event=0x{event:02X}, Arg=0x{arg:08X}, DataLen={len(data)}")
        return response
    
    def send_response(self, response_bytes):
        """Sends formatted response bytes back to the emulator."""
        try:
            print(f"-> Sending response ({len(response_bytes)} bytes): {response_bytes[:17].hex()} + {response_bytes[17:].hex() if len(response_bytes) > 17 else ''}")
            bytes_sent = self.request.sendall(response_bytes)
            print(f"   Response sent successfully (bytes_sent={bytes_sent})")
        except socket.error as e:
            print(f"Error sending response: {e}")
    
    def wrap_debugreadbyte(self, address, param2, timestamp) -> int:
        rvalue = self.handle_debugreadbyte(address, timestamp)

        self.request.sendall(struct.pack('<Bi', 1, rvalue))

    def wrap_readbyte(self, address, param2, timestamp) -> int:
        rvalue = self.handle_readbyte(address, timestamp)

        self.request.sendall(struct.pack('<Bi', 1, rvalue))

    def wrap_writebyte(self, param1, param2, timestamp) -> int:
        self.handle_writebyte(param1, param2, timestamp)
        self.request.sendall(b'\x01\0\0\0\0')

    def wrap_coldreset(self, param1, param2, timestamp) -> int:
        # The V1 protocol unfortunately lacks an extension point and rejects unknown
        # calls, so the host abuses the cold reset command with different parameters as
        # the init command to detect downlevel hosts. We do not support V1 hosts here
        # but must still play along.
        if param2 >= 0x7F000001 and param2 <= 0x7FFFFFFF:
            # tell host that we support V2 protocol
            self.request.sendall(b'\x0C\x02')
            self.wrap_init()

        self.handle_coldreset(timestamp)
        self.request.sendall(b'\x01\0\0\0\0')

    def wrap_warmreset(self, param1, param2, timestamp) -> int:
        self.handle_warmreset(timestamp)
        self.request.sendall(b'\x01\0\0\0\0')

    def wrap_script_event(self, param1, param2, timestamp) -> int:
        self.request.sendall(struct.pack('<Bi', 1, self.handle_script_event(param1, param2, timestamp)))

    def wrap_init(self):
        self.reflect_vars()

    #----------------------------------------------------------------------------------
    # Extension points
    #
    # These are expected to be selectively replaced by subclasses to implement server
    # behavior. There is generally no need to chain back to super.
    #

    def handle_none(self, param1, param2, timestamp) -> int:
        pass

    def handle_debugreadbyte(self, address, timestamp) -> int:
        """
        Handler invoked on a debug read from a network-based memory layer binding.
        The return value is the byte that would be read from the given address.
        This must be implemented without side effects; it is expected that debug
        reads may be issued at any time without affecting emulation state.

        The default handler returns an internal counter for initial debugging.
        """

        return self.counter

    def handle_readbyte(self, address, timestamp) -> int:
        """
        Handler invoked on a non-debug read from a network-based memory layer binding.
        The return value is the byte read from the given address. This handler is
        expected to apply any side effects from the read operation, such as clearing
        pending interrupts or advancing an internal read address.

        The default handler returns an internal auto-incrementing counter for initial
        debugging.
        """

        v = self.counter
        self.counter = (self.counter + 1) & 0xFF
        return v

    def handle_writebyte(self, address, value, timestamp) -> None:
        """
        Handler invoked on a write to a network-based memory layer binding.
        """

        self.counter = value

    def handle_init(self) -> None:
        """
        Handler invoked when the emulator is initially connected. Good for once init
        actions that must NOT occur on a cold reset.
        """

        pass

    def handle_coldreset(self, timestamp) -> None:
        """
        Handler invoked when a cold reset (power off/on) occurs in the emulator.
        """

        pass

    def handle_warmreset(self, timestamp) -> None:
        """
        Handler invoked when a warm reset (system reset) occurs in the emulator.
        """

        pass

    def handle_error(self, param1, param2, timestamp) -> int:
        """
        Handler invoked when the emulator detects a device server protocol error.
        This indicates either a programming error in the device server or incompatible
        emulator/server versions.
        """

        msg = self._readall(param2).decode('utf-8')
        print("Error from emulator: " + msg)
        return 0

    def handle_script_event(self, param1, param2, timestamp) -> int:
        """
        Synchronous call from device script to the device server. Emulation is halted
        while this executes and the return value is sent back to the script.
        """

        return 0

    def handle_script_post(self, param1, param2, timestamp) -> None:
        """
        Asynchornous call from device script to the device server. Emulation is NOT
        halted and continues while this is processed by the server.
        """

        pass

    def handle_netsio_data_byte_sync(self, param1, param2, timestamp) -> None:
        """
        Handler invoked on a NETSIO_DATA_BYTE_SYNC packet.
        This handler is called when packet ID 0x53 is received, which IS the SIO status command.
        - param1 contains parameters for the command
        - param2 contains packed arguments (DevID, AUX1, AUX2)
        """
        # IMPORTANT: The packet ID 0x53 is NETSIO_DATA_BYTE_SYNC, not the SIO Status command
        # This message carries a SIO command, with the actual command in param1
        sio_cmd = param1  # This may be different SIO commands (0x53='S', 0x52='R', etc.)
        packed_args = param2
        
        # TROUBLESHOOTING: For param1=0x18080000 (seen in logs), extract the actual SIO command
        # There are several possible encodings:
        actual_cmd = 0x53  # Default to Status command
        
        # Check if param1 is 0x18080000 which appears in the logs for Status command
        if param1 == 0x18080000:
            actual_cmd = 0x53  # 'S' (Status)
            print(f"Recognized special case: param1=0x18080000 -> Status command (0x53)")
        elif (param1 >> 24) == 0x53:  # Check if high-order byte is 0x53
            actual_cmd = 0x53
            print(f"Extracted command from high-order byte: 0x{actual_cmd:02X}")
        elif (param1 & 0xFF) == 0x53:  # Check if low-order byte is 0x53
            actual_cmd = 0x53
            print(f"Extracted command from low-order byte: 0x{actual_cmd:02X}")
        else:
            # Try to find the command byte in param1
            for shift in range(0, 32, 8):
                candidate = (param1 >> shift) & 0xFF
                if candidate >= 0x20 and candidate <= 0x7F:  # Printable ASCII range
                    actual_cmd = candidate
                    print(f"Found possible ASCII command at shift {shift}: 0x{candidate:02X} ('{chr(candidate)}')")
                    break
        
        # Extract the device ID, AUX1, and AUX2 from the packed args
        # The emulator seems to pack them in a specific format
        sio_dev_id = packed_args & 0xFF  # First byte is device ID
        sio_aux2 = (packed_args >> 8) & 0xFF  # Second byte is AUX2
        sio_aux1 = (packed_args >> 16) & 0xFF  # Third byte is AUX1

        # IMPORTANT: For testing, we need to handle the FujiNet device ID (0x70)
        # If device ID is 0, assume it's for FujiNet for now
        if sio_dev_id == 0:
            sio_dev_id = 0x70  # FujiNet device ID
            print(f"Device ID is 0, assuming FujiNet device (0x70) for testing")
        
        print(f"SIO Command: raw=0x{sio_cmd:08X}, parsed=0x{actual_cmd:02X} ('{chr(actual_cmd)}'), D={sio_dev_id:02X}, A1={sio_aux1:02X}, A2={sio_aux2:02X}")
        
        # Store latest command for later reference (e.g. when handling Command OFF Sync)
        self.last_sio_command = actual_cmd
        self.last_sio_device_id = sio_dev_id
        self.last_sio_aux1 = sio_aux1
        self.last_sio_aux2 = sio_aux2

        # In a complete implementation, forward to FujiNet peripheral via the hub
        # and wait for a response. For now, we'll generate a dummy response.
        
        # Check for different SIO commands
        if actual_cmd == 0x53:  # 'S' in ASCII - Status command
            print(f"Handling STATUS command (0x53) for device ID: 0x{sio_dev_id:02X}")
            
            # Check if this is for the FujiNet device (ID 0x70)
            is_fujinet = (sio_dev_id == 0x70)
            
            # For Status command (S), emulator expects 129 bytes total:
            # 1. First byte is ACK/NAK
            # 2. Followed by 4 bytes of status
            # 3. Followed by 128-4=124 bytes of drive status data 
            
            # Create status bytes (4 bytes) - this is a mockup for testing
            # In a real implementation, this would come from FujiNet
            if is_fujinet:
                # FujiNet device specific status bytes
                status_bytes = bytes([
                    0x10,  # Status byte 1 (e.g., drive online and write-protected)
                    0x00,  # Status byte 2
                    0x00,  # Status byte 3
                    0x00   # Status byte 4
                ])
            else:
                # Generic device status
                status_bytes = bytes([0x00, 0x00, 0x00, 0x00])
            
            # Create complete response - ACK + status + padding to 129 bytes
            dummy_data = bytes([1]) + status_bytes + bytes([0x00] * 124)
            
            print(f"Sending complete status response: {len(dummy_data)} bytes")
            print(f"Response data: {dummy_data[:10].hex()} ... (truncated)")
              
            # IMPORTANT: For the Altirra protocol, we need to send a synchronous response
            # matching what the emulator expects.
            
            # Send the ACK packet (ID=0, P1=1, P2=0) before sending the data packet
            self.request.sendall(struct.pack('<Bi', 0, 1))  # ACK
            print(f"Sent ACK for status response")
            
            # Send the data packet (ID=0, P1=length, P2=0, Data)
            self.request.sendall(struct.pack('<Bi', 0, len(dummy_data)))
            self.request.sendall(dummy_data)
            print(f"Sent status data: {len(dummy_data)} bytes")
             
            print(f"SIO command processing complete")
            
            # Directly proceed to sending Drive Status response for the FujiNet device
            # Since this is the initial boot sequence for the D1: virtual disk drive
            
        elif actual_cmd == 0x52:  # 'R' in ASCII - Read sector
            print(f"Handling READ SECTOR command (0x52) for device ID: 0x{sio_dev_id:02X}")
            
            # For Read Sector, emulator expects:
            # 1. ACK response for the command
            # 2. Data (sector contents) - typically 128 bytes for single density
            
            # First send ACK
            self.request.sendall(struct.pack('<Bi', 1, 1))  # ACK
            print(f"Sent READ SECTOR ACK")
            
            # Then send sector data (dummy data for now)
            sector_data = bytes([0xAA] * 128)  # Just fill with pattern
            self.request.sendall(sector_data)
            print(f"Sent read sector data: {len(sector_data)} bytes")
            
        elif actual_cmd == 0x50:  # 'P' in ASCII - Get PERCOM block (extended disk info)
            print(f"Handling GET PERCOM BLOCK command (0x50) for device ID: 0x{sio_dev_id:02X}")
            
            # For Get PERCOM Block, emulator expects:
            # 1. ACK response for the command
            # 2. PERCOM block data (12 bytes)
            
            # First send ACK
            self.request.sendall(struct.pack('<Bi', 1, 1))  # ACK
            print(f"Sent PERCOM BLOCK ACK")
            
            # Then send PERCOM data
            # Values for standard single density disk (90KB - 720 sectors of 128 bytes)
            percom_data = bytes([
                0x01,  # Tracks per side (high byte)
                0x28,  # Tracks per side (low byte) = 40 tracks 
                0x01,  # Sectors per track (high byte)
                0x12,  # Sectors per track (low byte) = 18 sectors
                0x00, 0x80,  # 128 bytes per sector
                0x00,  # Drive is online
                0x01,  # Transfer speed (standard)
                0x00, 0x00, 0x00, 0x00  # Reserved
            ])
            self.request.sendall(percom_data)
            print(f"Sent PERCOM block data: {len(percom_data)} bytes")
        elif actual_cmd == 0x00 and param1 == 0x18:  # Direct Command OFF Sync detection
            print(f"*** HANDLING DIRECT COMMAND OFF SYNC (0x18) ***")
            
            # Try to read any checksum byte if it exists
            try:
                self.request.settimeout(0.05)
                checksum_byte = self.request.recv(1, socket.MSG_PEEK)
                if len(checksum_byte) > 0:
                    checksum_byte = self.request.recv(1)  # Actually consume it
                    print(f"Consumed Command OFF Sync checksum: {checksum_byte.hex()}")
                else:
                    print("No checksum byte found")
            except Exception as e:
                print(f"No checksum byte available: {e}")
            finally:
                self.request.settimeout(None)
            
            # Format response exactly according to Altirra protocol
            print("*** SENDING SYNC RESPONSE AFTER COMMAND OFF SYNC ***")
            
            # Format consistent with Altirra protocol documentation exactly:
            sync_pkt_size = 5  # 5 byte payload
            sync_pkt_id = 0x81  # SYNC response ID
            sync_response = struct.pack('<BBBBBBB', 
                                       sync_pkt_size, sync_pkt_id,  # Header (2 bytes)
                                       0x81, 0, 1, 1, 0)  # Payload (5 bytes)
            
            print(f"Sending complete Sync response: {sync_response.hex()}")
            self.request.sendall(sync_response)
            
            print(f"Sent NetSIO Sync response (0x81): sync_number=0, ack_type=1, ack_byte=1, write_size=0")

    def handle_netsio_command_off_sync(self, param1, param2, timestamp) -> None:
        """
        Handler invoked on a NETSIO_COMMAND_OFF_SYNC packet.
        Called when the command frame has been completely sent and the 
        emulator is waiting for a device response.
        
        According to NetSIO protocol:
        - param1 is likely not used
        - param2 contains the sync_number to match in the sync response
        """
        sync_number = param2 & 0xFF
        
        print(f">>> NETSIO_COMMAND_OFF_SYNC HANDLER CALLED: P1=0x{param1:08X}, P2=0x{param2:08X}, sync_number={sync_number}")
        print(f"Last SIO command: CMD=0x{self.last_sio_command:02X}, D=0x{self.last_sio_device_id:02X}, A1=0x{self.last_sio_aux1:02X}, A2=0x{self.last_sio_aux2:02X}")
        
        # DEBUGGING - Dump handler call details
        print(f"About to send Sync response with sync_number={sync_number}")

        # CRITICAL: The format for Altirra protocol has specific requirements:
        # 1. First we send a message header with 5 bytes length
        # 2. Then we send the sync response params (5 bytes)
        # 3. The first byte of the params must be 0x81 (Sync event code)
        # 4. sync_number must be properly extracted from param2
        
        # Generate proper Sync response (0x81) according to the NetSIO protocol:
        # - sync_number: uint8 - sync request number (match the one from the request)
        # - ack_type: uint8 - acknowledgment type (1=ACK, 2=NAK)
        # - ack_byte: uint8 - acknowledgment byte
        # - write_size: uint16 - LSB+MSB write size next sync (we only use LSB for now)
        
        # Use proper SIO response codes
        ack_type = 1  # 1=ACK, 2=NAK
        ack_byte = 1  # Command accepted
        write_size = 0  # No write planned for next sync
        
        # Format according to Altirra protocol documentation exactly:
        sync_pkt_size = 5  # 5 byte payload
        sync_pkt_id = 0x81  # SYNC response ID
        sync_response = struct.pack('<BBBBBBB', 
                                   sync_pkt_size, sync_pkt_id,  # Header (2 bytes)
                                   0x81, sync_number, ack_type, ack_byte, write_size)  # Payload (5 bytes)
        
        print(f"Sending complete Sync response: {sync_response.hex()}")
        self.request.sendall(sync_response)
        
        print(f"Sent NetSIO Sync response (0x81): sync_number={sync_number}, ack_type={ack_type}, ack_byte={ack_byte}, write_size={write_size}")

    def forward_to_peripheral(self, cmd, dev_id, aux1, aux2, data=None):
        """
        Forward an SIO command to the FujiNet peripheral via UDP.
        This is a placeholder for future implementation.
        
        Args:
            cmd: SIO command byte (e.g., 0x53 for Status)
            dev_id: Device ID (e.g., 0x70 for FujiNet)
            aux1, aux2: Command parameters
            data: Optional data to send with the command
        """
        print(f"[PLACEHOLDER] Forwarding SIO command to peripheral: CMD=0x{cmd:02X}, D=0x{dev_id:02X}, A1=0x{aux1:02X}, A2=0x{aux2:02X}")
        # In a real implementation, this would create a UDP packet and send it to the peripheral
        # Then wait for the response and return it to the emulator

    #----------------------------------------------------------------------------------
    # Request functions
    #

    def req_enable_layer(self, layer_index: int, read: bool, write: bool):
        """
        Enable or disable a memory layer for read or write access. A memory layer
        enabled for an access type handles accesses of that type over the covered
        address space and prevents the access from being seen by lower priority layers.
        """

        self.request.sendall(struct.pack('<BBB', 2, layer_index, (2 if read else 0) + (1 if write else 0)))

    def req_set_layer_offset(self, layer_index: int, offset: int):
        """
        Change the starting byte offset for a layer. The starting offset must be
        page-aligned and the resulting mapped window must be entirely within the
        segment. It is a fatal error otherwise, though not all cases are diagnosed
        here.
        """

        if offset < 0 or offset & 255:
            raise ValueError('Invalid segment offset')

        self.request.sendall(struct.pack('<BBI', 3, layer_index, offset))

    def req_set_layer_segment_and_offset(self, layer_index: int, segment_index: int, segment_offset: int):
        """
        Change both the mapped segment and starting byte offset for a layer. The
        segment must be mappable, the offset must be page aligned, and the resulting
        mapped region must be completely contained within the segment.
        """

        if segment_offset < 0 or segment_offset & 255:
            raise ValueError('Invalid segment offset')

        self.request.sendall(struct.pack('<BBBI', 4, layer_index, segment_index, segment_offset))

    def req_set_layer_readonly(self, layer_index: int, ro: bool):
        """
        Change the read-only status of a layer. If a layer is read-only, writes to
        the segment are blocked from lower priority segments but the segment
        is not changed. This can only be applied to direct-mapped layers and not
        control layers.
        """

        self.request.sendall(struct.pack('<BBB', 5, layer_index, 1 if ro else 0))

    def req_read_seg_mem(self, segment_index: int, offset: int, len: int):
        """
        Read a block of memory from a segment as a bytes object. The region to read
        must be fully contained within the segment.
        """

        if offset < 0:
            raise ValueError('Invalid segment offset')

        if len <= 0:
            if len == 0:
                return bytes()

            raise ValueError('Invalid length')


        self.request.sendall(struct.pack('<BBII', 6, segment_index, offset, len))
        return self._readall(len)

    def req_write_seg_mem(self, segment_index: int, offset: int, data:bytes):
        """
        Write data from a bytes object to a segment. The region to write must be
        fully contained within the segment.
        """

        if offset < 0:
            raise ValueError('Invalid segment offset')

        if len(data) == 0:
            return

        self.request.sendall(struct.pack('<BBII', 7, segment_index, offset, len(data)))
        self.request.sendall(data)

    def req_fill_seg_mem(self, segment_index: int, offset: int, val: int, len: int):
        """
        Fill a region of a segment with a constant value. The region to fill must be
        fully contained within the segment.
        """

        if offset < 0:
            raise ValueError('Invalid segment offset')

        if len <= 0:
            if len == 0:
                return

            raise ValueError('Invalid fill length')

        self.request.sendall(struct.pack('<BBIBI', 13, segment_index, offset, val, len))

    def req_copy_seg_mem(self, dst_segment_index: int, dst_offset: int, src_segment_index: int, src_offset: int, len: int):
        """
        Copy data from one segment region to another. The source and destination
        regions must be entirely contained within their respective segments. The
        source and destination regions may be within the same segment and may overlap;
        if so, the copy direction is selected so that the entire contents of the source
        region are preserved in the copy to the destination (memmove semantics).
        """

        if dst_offset < 0:
            raise ValueError('Invalid destination segment offset')

        if src_offset < 0:
            raise ValueError('Invalid source segment offset')

        if len < 0:
            raise ValueError('Invalid copy length')

        if len > 0:
            self.request.sendall(struct.pack('<BBIBII', 8, dst_segment_index, dst_offset, src_segment_index, src_offset, len))

    def req_interrupt(self, aux1: int, aux2: int):
        """
        Issue a script interrupt with the arguments (aux1, aux2). This requires an
        accompanying binding in the device script to the script interrupt event, which
        is invoked in response.
        """

        self.request.sendall(struct.pack('<BII', 9, aux1, aux2))

    def req_get_segment_names(self):
        """
        Retrieve a list of segment variable names from the device script, in ascending
        index order (first = index 0). This is not normally called directly as the init
        handler will call it and reflect the segment names.
        """

        self.request.sendall(b'\x0A')
        return self._read_names()

    def req_get_layer_names(self):
        """
        Retrieve a list of memory layer names from the device script, in ascending
        index order (first = index 0). This is not normally called directly as the init
        handler will call it and reflect the memory layer names.
        """

        self.request.sendall(b'\x0B')
        return self._read_names()

    def reflect_vars(self):
        """
        Reflect variables from the device script into the handler so that segments and
        memory layers can be referred to by name. Segments are prefixed with seg_ and
        memory layers with layer_.
        """

        for segment_index, segment_name in enumerate(self.req_get_segment_names(), start=0):
            setattr(self, "seg_" + segment_name, DeviceSegment(self, segment_index))

        for layer_index, layer_name in enumerate(self.req_get_layer_names(), start=0):
            setattr(self, "layer_" + layer_name, DeviceMemoryLayer(self, layer_index))

    def _read_names(self):
        """
        Read a list of names from the inbound stream in the form: name count,
        [name len, name chars*]*.
        """

        num_names = struct.unpack('<I', self._readall(4))[0]
        names = []

        for i in range(0, num_names):
            name_len = struct.unpack('<I', self._readall(4))[0]
            print(name_len)
            names.append(self._readall(name_len).decode('utf-8'))

        return names

    def _readall(self, readlen):
        """
        Read the specified number of bytes from the inbound stream, blocking if
        necessary until they all arrive.
        """

        seg_data = bytearray()
        while len(seg_data) < readlen:
            seg_subdata = self.request.recv(readlen - len(seg_data))
            if len(seg_subdata) == 0:
                raise ConnectionError

            seg_data.extend(seg_subdata)

        return seg_data

    def debug_receive_bytes(self, count, timeout=5.0):
        """Receive bytes with debug output for each byte"""
        if not self.debug_mode:
            return self._readall(count)
        
        print(f"DEBUG: Reading {count} bytes from socket...")
        data = b''
        for i in range(count):
            byte = self.request.recv(1)
            if not byte:
                print(f"DEBUG: Connection closed after {i} bytes")
                break
            print(f"DEBUG: Received byte {i}: 0x{byte[0]:02X} ({byte[0]})")
            data += byte
        return data

class NetSIOHandler(DeviceTCPHandler):
    def setup(self):
        super().setup() # Call base class setup
        self.log_message(f"NetSIOHandler setup for {self.client_address}")
        # NetSIO specific state
        self.client_connected = True
        self.sio_command_buffer = bytearray() # Buffer for SIO command bytes (Cmd, Aux1, Aux2)
        self.current_device_id = 0 # Track device ID from COMMAND_ON

    def handle(self):
        """Overrides the base handler to implement NetSIO logic."""
        self.log_message(f"*** ENTERING NetSIOHandler.handle for {self.client_address} ***") # Added log
        self.log_message(f"NetSIO Handler starting for {self.client_address}")
        self.connection = self.request # Use self.connection for clarity
        self.connection.settimeout(30) # Set a timeout for blocking operations

        loop_count = 0
        while self.client_connected:
            loop_count += 1
            self.log_message(f"Handle loop iteration {loop_count}. Waiting for header...") # Added log
            try:
                header_data = self.connection.recv(17, socket.MSG_WAITALL) # Wait for full header
                read_len = len(header_data) if header_data is not None else -1 # Handle None case
                self.log_message(f"Read {read_len} bytes for header.") # Added log

                if not header_data:
                    self.log_message("Connection closed by emulator (no header).")
                    self.client_connected = False
                    break

                self.log_message(f"<- ATD RAW PACKET: {binascii.hexlify(header_data).decode('ascii')}")

                # Unpack the 17-byte header 
                try:
                    # Format: '<BIIQ' -> Byte (ID), UInt (P1), UInt (P2), ULongLong (TS)
                    packet_id, param1, param2, ts = struct.unpack('<BIIQ', header_data)
                except struct.error as e:
                    self.log_message(f"Error unpacking header: {e}")
                    continue # Skip to next message

                self.log_message(f"<- ATD UNPACKED: ID=0x{packet_id:02X}, P1=0x{param1:08X}, P2=0x{param2:08X}, TS={ts}")

                # --- Read payload data if expected based on packet_id --- 
                payload_data = b''
                actual_data_len = 0
                if packet_id == 0x02: # DATA_BLOCK
                    actual_data_len = param2 # Length is in param2 according to emulator logs
                elif packet_id == 0x18: # COMMAND_OFF_SYNC
                     # Emulator sends 1 byte payload (checksum) but puts sync# in param2.
                     actual_data_len = 1
                # Add other packet IDs that have payloads here if needed
                
                if actual_data_len > 0:
                    try:
                        # Ensure we don't try to read more than a reasonable amount
                        if actual_data_len > 4096: 
                             self.log_message(f"Error: Excessive payload length requested: {actual_data_len}")
                             continue # Skip this message
                        
                        # Use MSG_WAITALL to ensure full payload is read (or timeout/error occurs)
                        payload_data = self.connection.recv(actual_data_len, socket.MSG_WAITALL)
                        
                        if len(payload_data) < actual_data_len:
                            # This case should be less likely with MSG_WAITALL unless connection drops
                            self.log_message(f"Error: Incomplete payload received. Expected {actual_data_len}, got {len(payload_data)}")
                            self.client_connected = False # Assume connection issue
                            break 
                        self.log_message(f"   Read {len(payload_data)} payload bytes: {payload_data.hex()}")
                    except socket.timeout:
                        self.log_message(f"Error: Timeout reading {actual_data_len} payload bytes")
                        self.client_connected = False # Assume connection issue
                        break
                    except socket.error as e:
                        self.log_message(f"Error reading {actual_data_len} payload bytes: {e}")
                        self.client_connected = False # Assume connection issue
                        break
                # --- End Payload Read --- 

                # --- NetSIO Packet Handling Logic --- 
                if packet_id == 0x00:
                    pass # Ignore null events, often padding
                
                # Check for SIO-related command sequence packets
                elif packet_id in [0x11, 0x02, 0x18]: # COMMAND_ON, DATA_BLOCK, COMMAND_OFF_SYNC
                     # Call the specific SIO event handler within this class
                     self.handle_sio_event(packet_id, param1, param2, payload_data)
                     
                # Handle Direct SIO Command (0x53) - Legacy/incorrect path?
                elif packet_id == 0x53: 
                    self.log_message(f"WARNING: Received direct SIO command packet ID 0x53 - expected sequence 0x11, 0x02, 0x18.")
                    # Attempt to handle as SIO Status using params, but this is unreliable
                    device_id_guess = param2 & 0xFF # Guess based on last known pattern for status
                    sync_num_guess = 0 
                    self.log_message(f"   Attempting legacy handling for guessed D=0x{device_id_guess:02X}")
                    self.respond_sio_status(device_id_guess, sync_num_guess)

                else:
                    self.log_message(f"Unhandled packet ID: {packet_id:02X}")
                    # Consider sending an error response back?

            except socket.timeout:
                self.log_message("Socket timeout waiting for command header.")
                # Keep listening unless timeout is very long? Or disconnect?
                # For now, just log and continue loop.
                # self.client_connected = False 
                # break
            except ConnectionResetError:
                 self.log_message("Connection reset by peer.")
                 self.client_connected = False
                 break
            except Exception as e:
                # Catch other potential errors during recv/processing
                self.log_message(f"Unhandled exception in handle loop: {e}")
                import traceback
                traceback.print_exc()
                self.client_connected = False # Disconnect on unexpected errors
                break
                
        self.log_message("NetSIO Handler loop finished.")
        # Ensure finish is called if necessary by base class or framework
        # self.finish() 


    def handle_sio_event(self, event_id, param1, param2, payload_data):
        """Handles the sequence of SIO-related events (0x11, 0x02, 0x18)."""
        # This method is part of NetSIOHandler
        self.log_message(f"Handling SIO event: ID=0x{event_id:02X}, P1=0x{param1:08X}, P2=0x{param2:08X}, Payload='{payload_data.hex()}'")

        if event_id == 0x11: # NETSIO_COMMAND_ON
            self.current_device_id = param2 # param2 holds the device ID in COMMAND_ON
            self.sio_command_buffer = bytearray() # Clear buffer for new command sequence
            self.log_message(f"   SIO Event: COMMAND_ON for device 0x{self.current_device_id:02X}. Buffer cleared.")

        elif event_id == 0x02: # NETSIO_DATA_BLOCK
            # param2 holds the length, payload_data holds the SIO cmd bytes (cmd, aux1, aux2)
            if payload_data:
                self.sio_command_buffer.extend(payload_data)
                self.log_message(f"   SIO Event: DATA_BLOCK received {len(payload_data)} bytes. Buffer now {len(self.sio_command_buffer)} bytes: {self.sio_command_buffer.hex()}")
            else:
                 self.log_message(f"   SIO Event: WARNING - DATA_BLOCK (0x02) received but no payload data was read?")


        elif event_id == 0x18: # NETSIO_COMMAND_OFF_SYNC
            sync_number_request = param2 # param2 holds the requested sync number
            sio_checksum = payload_data[0] if payload_data else None # Checksum is the 1-byte payload
            
            # Determine checksum string representation before logging
            sio_checksum_str = f"{sio_checksum:02X}" if sio_checksum is not None else 'N/A'
            self.log_message(f"   SIO Event: COMMAND_OFF_SYNC received. SyncReq={sync_number_request}, Checksum={sio_checksum_str}")
            
            # Verify checksum (optional but good practice)
            calculated_checksum = self.calculate_sio_checksum(self.sio_command_buffer)
            if sio_checksum is not None and calculated_checksum != sio_checksum:
                 self.log_message(f"   SIO Event: WARNING - Checksum mismatch! Got 0x{sio_checksum:02X}, calculated 0x{calculated_checksum:02X}")
                 # Handle error? Send NAK? For now, proceed.
            
            # Assume buffer now holds the complete 3-byte SIO command (cmd, aux1, aux2)
            if len(self.sio_command_buffer) == 3:
                sio_command = self.sio_command_buffer[0]
                sio_aux1 = self.sio_command_buffer[1]
                sio_aux2 = self.sio_command_buffer[2]
                
                self.log_message(f"   SIO Command Parsed from sequence: D=0x{self.current_device_id:02X}, C=0x{sio_command:02X}, A1=0x{sio_aux1:02X}, A2=0x{sio_aux2:02X}")
                
                # --- Process the actual SIO command --- #
                if sio_command == 0x53: # Status Command ('S')
                    self.respond_sio_status(self.current_device_id, sync_number_request)
                
                elif sio_command == 0x52: # Read Sector ('R')
                     # Placeholder: Need logic to handle read sector command
                     # This would involve getting sector number from Aux1/Aux2,
                     # reading data (e.g., from a disk image or peripheral),
                     # and sending ACK, Data Block (128 bytes), SYNC.
                     self.log_message(f"   SIO READ SECTOR (0x52) received - Placeholder")
                     # Respond NAK for now as it's unimplemented
                     self.respond_sio_nak(sync_number_request, 'N') # 'N' for NAK
                     self.log_message(f"   Sent NAK for unimplemented SIO Read Sector 0x52")                     

                elif sio_command == 0x50: # Get PERCOM Block ('P')
                    # Placeholder: Handle PERCOM block request
                    # Need to send ACK, Data Block (12 bytes), SYNC.
                    self.log_message(f"   SIO GET PERCOM (0x50) received - Placeholder")
                    # Respond NAK for now
                    self.respond_sio_nak(sync_number_request, 'N')
                    self.log_message(f"   Sent NAK for unimplemented SIO PERCOM 0x50")

                else:
                     # Handle other SIO commands if needed, otherwise send NAK
                     self.log_message(f"   Unhandled SIO command in sequence: 0x{sio_command:02X}")
                     self.respond_sio_nak(sync_number_request, 'N') # Send NAK for unknown command
                     self.log_message(f"   Sent NAK for unhandled SIO command 0x{sio_command:02X}")

            else:
                 # Error case: Incorrect number of bytes received before COMMAND_OFF_SYNC
                 self.log_message(f"   SIO Event: ERROR - Expected 3 bytes in command buffer for SIO cmd, got {len(self.sio_command_buffer)}")
                 self.respond_sio_nak(sync_number_request, 'E') # Send 'E' for Error
                 self.log_message(f"   Sent Error response")

        else:
             # Should not happen given the calling condition in handle()
             self.log_message(f"   Internal Error: handle_sio_event called with unexpected ID: {event_id:02X}")


    def respond_sio_status(self, device_id, sync_number_request):
        """Handles sending the specific Altirra response sequence for an SIO Status (0x53) command."""
        # This method is part of NetSIOHandler
        self.log_message(f"Responding to SIO Status (0x53) for device 0x{device_id:02X}, SyncReq={sync_number_request}")
        
        # 1. Send ACK (Altirra Event 0x80)
        # For ACK, the 'arg' field holds the SIO status byte ('A' = ACK)
        ack_response = self.create_altirra_response(0x80, ord('A'), b'') 
        if not self.send_response(ack_response):
            self.log_message("Failed to send ACK response")
            return
        self.log_message(f"   Sent ACK (0x80) response with status 'A'")
        
        # 2. Send Status Data Block (Altirra Event 0x82)
        # Create status data (D1: ON, Single Density for now)
        status_data = bytearray(129) # Atari expects 129 bytes for status
        if device_id >= 0x31 and device_id <= 0x38:
            drive_index = device_id - 0x31
            # Simulate Drive 1 ON, others OFF for testing boot
            if drive_index == 0: 
                status_data[0] = 0x01 # Drive Status (Bit 0: 1=ON)
                status_data[1] = 0x10 # Drive Type (Bit 4: 1=SD, 0=DD) 
                status_data[2] = 0x01 # Timeout LSB (non-zero indicates ready)
                status_data[3] = 0x00 # Timeout MSB
                self.log_message(f"   Prepared status data for D{drive_index + 1}: ON, SD, Ready")
            else:
                 # Keep other drives OFF
                 self.log_message(f"   Prepared status data for D{drive_index + 1}: OFF")
        else:
             self.log_message(f"   WARNING: Status requested for non-disk device 0x{device_id:02X}")
             # Send default 'OFF' status?
        
        # For DATA_BLOCK response (0x82), 'arg' holds the length
        data_response = self.create_altirra_response(0x82, len(status_data), status_data) 
        if not self.send_response(data_response):
            self.log_message("Failed to send Status Data Block response")
            return
        self.log_message(f"   Sent Status Data Block (0x82) response: {len(status_data)} bytes")
        
        # 3. Send SYNC (Altirra Event 0x81) with the requested sync number
        # For SYNC response (0x81), 'arg' holds the sync number
        sync_response = self.create_altirra_response(0x81, sync_number_request, b'') 
        if not self.send_response(sync_response):
            self.log_message("Failed to send SYNC response")
            return
        self.log_message(f"   Sent SYNC (0x81) response with sync number {sync_number_request}")
        
        self.log_message(f"SIO command processing complete for Status (0x53)")

    def respond_sio_nak(self, sync_number_request, nak_code='N'):
        """Sends a NAK response followed by a SYNC response."""
        # This method is part of NetSIOHandler
        self.log_message(f"Responding with NAK ('{nak_code}') for SyncReq={sync_number_request}")
        
        # 1. Send NAK (Altirra Event 0x80 with NAK code as arg)
        nak_response = self.create_altirra_response(0x80, ord(nak_code), b'') 
        if not self.send_response(nak_response):
            self.log_message("Failed to send NAK response")
            return
        self.log_message(f"   Sent NAK (0x80) response with code '{nak_code}'")

        # 2. Send SYNC (Altirra Event 0x81) with the requested sync number
        sync_response = self.create_altirra_response(0x81, sync_number_request, b'') 
        if not self.send_response(sync_response):
            self.log_message("Failed to send SYNC response after NAK")
            return
        self.log_message(f"   Sent SYNC (0x81) response with sync number {sync_number_request} after NAK")


    def calculate_sio_checksum(self, data):
        """Calculate the simple Atari SIO checksum (sum of bytes modulo 256)."""
        # This method is part of NetSIOHandler
        checksum = 0
        for byte in data:
            checksum = (checksum + byte) % 256
        return checksum

    def create_altirra_response(self, event, arg, data):
        """Formats a response packet for Altirra."""
        # This method is part of NetSIOHandler
        packet_len = 17 + len(data)
        # Format: <BIIQ = ID, P1, P2, TS (P1=event, P2=arg for responses)
        # Timestamp (TS) is usually 0 for responses from server?
        header = struct.pack('<BIIQ', event, 0, arg, 0) # Using event as ID, P1=0, P2=arg
        # Ensure header is 17 bytes (pad if necessary, though struct should handle it)
        header = header.ljust(17, b'\x00') 
        return header + data

    def send_response(self, response_bytes):
        """Sends formatted response bytes back to the emulator."""
        # This method is part of NetSIOHandler
        if not self.client_connected:
            self.log_message("Attempted to send response, but client disconnected.")
            return False
        try:
            self.connection.sendall(response_bytes)
            # Verbose logging of sent data can be helpful but noisy
            # self.log_message(f"-> Sent {len(response_bytes)} bytes: {response_bytes.hex()}")
            return True
        except socket.error as e:
            self.log_message(f"Socket error sending response: {e}")
            self.client_connected = False
            return False
            
    def finish(self):
        # This method is part of NetSIOHandler
        super().finish() # Call base class finish if needed
        self.log_message(f"NetSIOHandler finished for {self.client_address}")
        self.client_connected = False
        # Any other cleanup specific to NetSIOHandler

    def log_message(self, message):
        # Simple logging prefix
        logging.info(f"[NetSIOHandler {self.client_address}] {message}")


# --- Main Execution --- #
def print_banner():
    print("Altirra Custom Device Server v0.8")
    print()

def run_deviceserver(
    handler: type,
    port: int = 6502,
    arg_parser = argparse.ArgumentParser(description = "Starts a localhost TCP server to handle emulator requests for a custom device."),
    run_handler = None,
    post_argparse_handler = None
):
    """
    Bootstrap the device server. Call this from your startup module to print
    the startup banner, parse command line arguments, and run the TCP server.

    To add arguments, override the default argument parser and supply a pre-populated
    instance via the arg_parser argument. The parsed arugments are passed to
    post_argparse_handler() and stashed as self.server.cmdline_args.
    """

    print_banner()

    arg_parser.add_argument('--port', type=int, default=port, help='Change TCP port (default: {})'.format(port))
    arg_parser.add_argument('-v', '--verbose', dest='verbose', action='store_true', help='Log emulation device commands')

    args = arg_parser.parse_args()

    if post_argparse_handler is not None:
        post_argparse_handler(args)

    with socketserver.TCPServer(("localhost", args.port), handler) as server:
        server.cmdline_args = args

        print(f"\n***** RUN_DEVICESERVER CALLED *****")
        print(f"***** HANDLER CLASS: {handler.__name__} *****")
        print(f"***** PORT: {args.port} *****\n")
        
        print("Waiting for localhost connection from emulator on port {} -- Ctrl+Break to stop".format(args.port))

        if run_handler is not None:
            run_handler(server)
        else:
            server.serve_forever()

if __name__ == '__main__':
    logging.basicConfig(level=logging.DEBUG, format='%(asctime)s %(levelname)-8s %(message)s')
    # Use the factory to handle connections
    #factory = NetSIOHubFactory(NetSIOHandler, "NetSIO Hub Device")
    #run_deviceserver_with_factory(factory)

    print(f"\n***** RUN_DEVICESERVER CALLED *****")
    print(f"***** HANDLER CLASS: NetSIOHandler *****")
    print(f"***** PORT: 9996 *****\n")
    
    run_deviceserver(NetSIOHandler, port=9996)

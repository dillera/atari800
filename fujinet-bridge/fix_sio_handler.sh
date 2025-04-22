#!/bin/bash

# Script to apply SIO handler fixes to the NetSIO hub

# Fix the DeviceTCPHandler in deviceserver.py to properly handle SIO commands
cat > /tmp/deviceserver_fix.py << 'EOF'
import re

file_path = '/Users/dillera/code/atari800/fujinet-bridge/netsiohub/deviceserver.py'

with open(file_path, 'r') as file:
    content = file.read()

# Add debug prints to the handle method
handle_pattern = r'def handle\(self\):\s+"""[\s\S]+?"""'
handle_replacement = '''def handle(self):
        """
        Main protocol service loop.
        """
        print("\\n***** DEVICESERVER.PY HANDLE METHOD CALLED *****")
        print(f"***** HANDLER TYPE: {self.__class__.__name__} *****")
        print(f"***** CLIENT IP: {self.client_address} *****\\n")
        
        self.verbose = self.server.cmdline_args.verbose
        print("Connection received from emulator - ready to handle NetSIO messages")
        
        # Increase timeout for debugging
        self.request.settimeout(60)  # 60 second timeout
        
        # For tracking SIO command state across multiple packets
        self.sio_command_buffer = bytearray()
        self.current_device_id = 0
        self.current_command = None'''

content = re.sub(handle_pattern, handle_replacement, content)

# Add SIO command processing logic
packet_processing_pattern = r'packet_id, param1, param2, timestamp = struct\.unpack\(\'<BIIQ\', command_packet\)[\s\S]+?handler\(param1, param2, timestamp\)'
packet_processing_replacement = '''packet_id, param1, param2, timestamp = struct.unpack('<BIIQ', command_packet)
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
                print("\\n***** RECEIVED SIO COMMAND_ON (0x11) *****")
                # Reset state for new command
                self.sio_command_buffer.clear()
                self.current_device_id = param2
                print(f"***** SIO Event: COMMAND_ON for device 0x{param2:02X} *****")
            elif packet_id == 0x02 and payload_data:  # DATA_BLOCK (SIO command bytes)
                print("\\n***** RECEIVED SIO DATA_BLOCK (0x02) *****")
                # Store command data for later processing
                self.sio_command_buffer = bytearray(payload_data)
                if len(self.sio_command_buffer) >= 3:
                    cmd = self.sio_command_buffer[0]
                    aux1 = self.sio_command_buffer[1]
                    aux2 = self.sio_command_buffer[2]
                    self.current_command = cmd
                    print(f"***** SIO Event: DATA_BLOCK with CMD=0x{cmd:02X}, AUX1=0x{aux1:02X}, AUX2=0x{aux2:02X} *****")
            elif packet_id == 0x18:  # COMMAND_OFF_SYNC (End of SIO frame, wait for sync response)
                print("\\n***** RECEIVED SIO COMMAND_OFF_SYNC (0x18) *****")
                # Process the complete SIO command
                sync_number_request = param2
                sio_checksum = payload_data[0] if payload_data else None
                
                checksum_str = f"{sio_checksum:02X}" if sio_checksum is not None else 'N/A'
                print(f"***** SIO Event: COMMAND_OFF_SYNC received. SyncReq={sync_number_request}, Checksum={checksum_str} *****")
                
                # Handle the SIO command response
                if self.current_command == 0x53:  # 'S' Status command
                    print("\\n***** PROCESSING STATUS COMMAND (0x53) *****")
                    self.respond_sio_status(self.current_device_id, sync_number_request)
                    print("***** STATUS COMMAND PROCESSING COMPLETE *****\\n")
                else:
                    # Default response for unhandled commands (NAK)
                    print(f"\\n***** UNHANDLED COMMAND: 0x{self.current_command:02X if self.current_command else 0:02X} - SENDING NAK *****")
                    self.respond_sio_nak(sync_number_request)
                    print("***** NAK RESPONSE COMPLETE *****\\n")
                
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

            handler(param1, param2, timestamp)'''

content = re.sub(packet_processing_pattern, packet_processing_replacement, content)

# Add SIO response methods
methods_to_add = '''
    def respond_sio_status(self, device_id, sync_number_request):
        """Handles sending the specific Altirra response sequence for an SIO Status (0x53) command."""
        print(f"Responding to STATUS command for device 0x{device_id:02X} with sync #{sync_number_request}")
        
        # Step 1: Create and send ACK response (event 0x01, arg is the device ID)
        ack_response = self.create_altirra_response(0x01, device_id, b'\\x41')  # 'A' for ACK
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
'''

# Find the position to insert the new methods (after the handle method)
handle_end_pattern = r'handler\(param1, param2, timestamp\)\s+'
match = re.search(handle_end_pattern, content)
if match:
    insert_position = match.end()
    content = content[:insert_position] + methods_to_add + content[insert_position:]

# Write the modified content back to the file
with open(file_path, 'w') as file:
    file.write(content)

print("DeviceTCPHandler in deviceserver.py has been updated with SIO command handling.")
EOF

# Run the Python script to apply the changes
python3 /tmp/deviceserver_fix.py

# Make the debug_cycle.sh script executable
chmod +x /Users/dillera/code/atari800/fujinet-bridge/debug_cycle.sh

echo "SIO handler fixes have been applied. You can now run ./debug_cycle.sh to test the changes."

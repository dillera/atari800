"""
NetSIO Client Implementation

This module contains the client-side components for NetSIO communication,
including the NetSIOClient class and related buffer handling.
"""

import socket
import threading
import queue
import time

from netsiohub.netsio_protocol import *

class NetInBuffer:
    """Buffer for incoming network data that extracts NetSIO messages"""
    
    def __init__(self):
        """initialize empty buffer"""
        self.data = bytearray()
        self.messages = queue.Queue()
        self.lock = threading.Lock()
        
    def get_messages(self) ->list:
        """get and remove all buffered messages"""
        messages = []
        while not self.messages.empty():
            messages.append(self.messages.get())
        return messages

    def extend(self, b:bytearray):
        """extend buffer with new data and extract messages"""
        with self.lock:
            self.data.extend(b)
            # process as many full messages as we have
            while len(self.data) >= 2:
                # get message length and check if we have a complete message
                msg_len = self.data[0] + 256 * self.data[1]
                if len(self.data) >= msg_len + 2:
                    # extract message id and arg
                    msg_id = self.data[2]
                    # handle the different length messages
                    if msg_len == 1:
                        # no arg
                        arg = None
                    elif msg_len == 2:
                        # single byte arg
                        arg = self.data[3]
                    else:
                        # multi-byte arg (stored in NetSIO as a list)
                        arg = list(self.data[3:msg_len+2])
                    # add to message buffer
                    self.messages.put(NetSIOMsg(msg_id, arg))
                    # remove from data buffer
                    del self.data[0:msg_len+2]
                else:
                    # don't have a complete message, will retry next time
                    return

class NetInThread(threading.Thread):
    """thread that reads from a socket, buffers the data, and puts complete messages into a queue"""
    
    def __init__(self, sock, msg_queue, in_buffer=None):
        """initialize thread with socket to read from and queue to send messages to"""
        super().__init__()
        self.sock = sock
        self.msg_queue = msg_queue
        self.in_buffer = in_buffer if in_buffer is not None else NetInBuffer()
        self.stop_event = threading.Event()
        self.daemon = True
    
    def run(self):
        """thread main: read from socket, process bytes, extract NetSIO messages and add to queue"""
        try:
            while not self.stop_event.is_set():
                try:
                    data = self.sock.recv(4096)
                    if not data:
                        # remote socket closed
                        break
                    self.in_buffer.extend(data)
                    for msg in self.in_buffer.get_messages():
                        self.msg_queue.put(msg)
                except socket.timeout:
                    # no data available, retry
                    continue
                except ConnectionResetError:
                    # connection lost
                    error_print("CONNECTION RESET")
                    break
                except ConnectionAbortedError:
                    # connection lost
                    error_print("CONNECTION ABORTED")
                    break
                except OSError as e:
                    # socket error
                    error_print(f"SOCKET ERROR: {e}")
                    break
        except Exception as e:
            error_print(f"NetInThread unexpected error: {e}")
        self.sock.close()

    def stop(self):
        """stop the thread"""
        self.stop_event.set()
        
class NetOutThread(threading.Thread):
    """thread that reads from a queue and writes messages to a socket"""
    
    def __init__(self, sock, msg_queue):
        """initialize thread with socket to write to and queue to read messages from"""
        super().__init__()
        self.sock = sock
        self.msg_queue = msg_queue
        self.stop_event = threading.Event()
        self.daemon = True
    
    def run(self):
        """thread main: read from queue, convert to NetSIO message bytes, and write to socket"""
        try:
            while not self.stop_event.is_set():
                try:
                    msg = self.msg_queue.get(timeout=0.1)
                    try:
                        self.sock.sendall(msg.to_bytes())
                    except (ConnectionResetError, ConnectionAbortedError, OSError) as e:
                        error_print(f"NetOutThread.run: Socket error: {e}")
                        break
                except queue.Empty:
                    # no data available, retry
                    continue
        except Exception as e:
            error_print(f"NetOutThread unexpected error: {e}")
        self.sock.close()

    def stop(self):
        """stop the thread"""
        self.stop_event.set()

class NetSIOClient:
    """NetSIO protocol client"""
    
    def __init__(self, sock=None, addr=None):
        """initialize client with socket and remote address"""
        self.sock = sock
        self.addr = addr
        self.in_queue = queue.Queue()
        self.out_queue = queue.Queue()
        self.in_thread = None
        self.out_thread = None
        
    def fileno(self):
        """return socket file number for select()"""
        return self.sock.fileno()
        
    def start(self):
        """start client threads"""
        if self.in_thread is None:
            self.in_thread = NetInThread(self.sock, self.in_queue)
            self.in_thread.start()
        if self.out_thread is None:
            self.out_thread = NetOutThread(self.sock, self.out_queue)
            self.out_thread.start()
            
    def stop(self):
        """stop client threads"""
        if self.in_thread is not None:
            self.in_thread.stop()
            self.in_thread = None
        if self.out_thread is not None:
            self.out_thread.stop()
            self.out_thread = None
            
    def send(self, msg:NetSIOMsg):
        """add message to outgoing queue"""
        self.out_queue.put(msg)
        
    def receive(self) ->NetSIOMsg:
        """get first message from incoming queue if available, otherwise None"""
        if self.in_queue.empty():
            return None
        else:
            return self.in_queue.get()

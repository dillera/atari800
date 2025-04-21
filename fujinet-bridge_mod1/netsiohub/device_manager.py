"""
Device Manager Implementation

This module contains the device management classes for coordinating 
communication between peripherals and the host.
"""

from abc import ABC, abstractmethod
import socket
import threading
import queue
import time

from netsiohub.netsio_protocol import *
from netsiohub.netsio_client import NetSIOClient

class DeviceManager(ABC):
    """Abstract base class for device management"""
    
    def __init__(self):
        """initialize device manager"""
        self.peripheral_clients = {}
        
    @abstractmethod
    def connected(self):
        """check if any peripherals are connected"""
        pass
        
    @abstractmethod
    def to_host(self, msg:NetSIOMsg):
        """send message to host"""
        pass
        
    @abstractmethod
    def to_peripheral(self, msg:NetSIOMsg):
        """send message to all peripherals"""
        pass
        
    @abstractmethod
    def register_peripheral(self, client:NetSIOClient):
        """register peripheral client"""
        pass
        
    @abstractmethod
    def unregister_peripheral(self, client:NetSIOClient):
        """unregister peripheral client"""
        pass

class NetSIOManager(DeviceManager):
    """Device manager for NetSIO protocol"""
    
    def __init__(self, hub=None):
        """initialize device manager with reference to hub"""
        super().__init__()
        self.hub = hub
        self.client_lock = threading.Lock()
        self.sync_tmout = 1.0  # Default sync timeout
        
    def connected(self):
        """check if any peripherals are connected"""
        with self.client_lock:
            return len(self.peripheral_clients) > 0
        
    def to_host(self, msg:NetSIOMsg):
        """send message to host via hub"""
        if self.hub is not None:
            self.hub.to_host(msg)
        
    def to_peripheral(self, msg:NetSIOMsg):
        """send message to all peripherals"""
        with self.client_lock:
            # Make a copy of the dict values to avoid modifying while iterating
            clients = list(self.peripheral_clients.values())
            
        for client in clients:
            try:
                client.send(msg)
            except Exception as e:
                error_print(f"Error sending to peripheral {client.addr}: {e}")
                # Don't unregister here to avoid deadlock, let the client handler do it
        
    def register_peripheral(self, client:NetSIOClient):
        """register peripheral client"""
        with self.client_lock:
            addr_str = f"{client.addr[0]}:{client.addr[1]}"
            if addr_str in self.peripheral_clients:
                # Peripheral already registered, stop the old client
                old_client = self.peripheral_clients[addr_str]
                old_client.stop()
            
            # Start the new client and register it
            client.start()
            self.peripheral_clients[addr_str] = client
            log_trace(f"Registered peripheral {addr_str}")
        
    def unregister_peripheral(self, client:NetSIOClient):
        """unregister peripheral client"""
        with self.client_lock:
            addr_str = f"{client.addr[0]}:{client.addr[1]}"
            if addr_str in self.peripheral_clients:
                if self.peripheral_clients[addr_str] == client:
                    # Stop the client and remove it from the registry
                    client.stop()
                    del self.peripheral_clients[addr_str]
                    log_trace(f"Unregistered peripheral {addr_str}")
                else:
                    log_trace(f"Client {addr_str} already replaced, not unregistering")

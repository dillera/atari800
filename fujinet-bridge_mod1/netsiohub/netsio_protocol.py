"""
NetSIO Protocol Definitions

This module contains all protocol-related constants, message classes, 
and utilities for the NetSIO protocol.
"""

# Import original NetSIO definitions to maintain compatibility
from netsiohub.netsio import *

# Additional constants needed for the FujiNet integration
ATDEV_NAK_RESPONSE = 0x8F  # NAK response for sync messages
ATDEV_EMPTY_SYNC = 0x00    # Empty sync response

# Logging utilities from existing codebase
from netsiohub.netsio import debug_print, info_print, error_print, warning_print, log_trace, log_sio_command

# Re-export these for use by the other modules
__all__ = [
    # Constants
    'ATDEV_NAK_RESPONSE', 
    'ATDEV_EMPTY_SYNC',
    # Classes from netsio.py
    'NetSIOMsg',
    'SyncManager',
    'clear_queue',
    # Logging functions
    'debug_print', 
    'info_print', 
    'error_print', 
    'warning_print',
    'log_trace',
    'log_sio_command'
]

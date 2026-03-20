# Coding Conventions

**Analysis Date:** 2026-03-20

## Naming Patterns

**Files:**
- C source files: `lowercase_with_underscores.c` and `.h` (e.g., `antic.c`, `gtia.h`, `log.c`)
- JavaScript/Node.js: `camelCase.js` (e.g., `index.js`) or `lowercase_with_underscores.js`
- Python: `lowercase_with_underscores.py` (e.g., `atari800_ai.py`)

**Functions:**
- C: `snake_case` (e.g., `ANTIC_Initialise`, `Log_print`, `ANTIC_Frame`)
- JavaScript: `camelCase` for functions (e.g., `sendCommand`, `formatScreen`, `isEmulatorRunning`)
- JavaScript: `PascalCase` for class/constructor names (e.g., `Server`, `Atari800AI`)
- Python: `snake_case` for functions (e.g., `connect`, `disconnect`, `_send`)

**Variables:**
- C: `UPPERCASE` for constants and globals (e.g., `SOCKET_PATH`, `ANTIC_CHACTL`, `GTIA_M0PL`), `camelCase` for local/static variables (e.g., `consol_override`, `GTIA_speaker`)
- JavaScript: `camelCase` for variables (e.g., `emulatorProcess`, `expectedLength`, `SOCKET_PATH` for constants)
- Python: `snake_case` for variables (e.g., `socket_path`, `self.sock`), `UPPERCASE_SNAKE_CASE` for constants (e.g., `AKEY_A`, `DEFAULT_SOCKET`)

**Types & Classes:**
- C: No formal type naming convention; typedef structs use descriptive names (see symbol table in `monitor.c`: `symtable_rec`)
- JavaScript: Classes use `PascalCase` (e.g., `Atari800AI`, `Server`)
- Python: Classes use `PascalCase` (e.g., `Atari800AI`)
- Custom types in C: `UBYTE` (unsigned byte), `UWORD` (unsigned word) - defined in `atari.h`

## Code Style

**Formatting:**
- **Indentation**: Tabs in C files (observed in `log.c`, `monitor.c`, `gtia.c`)
- **Indentation**: 2 spaces in JavaScript (observed in `mcp-server/index.js`)
- **Indentation**: 4 spaces in Python (observed in `atari800_ai.py`)
- **Line length**: No strict limit enforced; pragmatic line wrapping observed
- **Brace style**: Opening braces on same line (JavaScript), K&R style in C

**Linting:**
- No ESLint, Prettier, or similar tool detected in JavaScript/TypeScript
- No Pylint, Black, or similar tool detected in Python
- C code uses traditional GCC conventions without automated linting
- Manual code review appears to be the primary quality mechanism

## Import Organization

**JavaScript (ES modules):**
1. External framework imports (`@modelcontextprotocol/sdk` imports)
2. Node.js built-in modules (`net`, `spawn`, `fs`, `path`, `fileURLToPath`)
3. Local module imports (none in `mcp-server/index.js`)

Example from `mcp-server/index.js`:
```javascript
import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from '@modelcontextprotocol/sdk/types.js';
import net from 'net';
import { spawn } from 'child_process';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
```

**Python:**
1. Standard library imports
2. Third-party library imports
3. Local module imports

Example from `atari800_ai.py`:
```python
import socket
import json
import time
import base64
from typing import Optional, List, Dict, Any, Union
from contextlib import contextmanager
```

**C:**
1. Standard library headers (`#include <stdio.h>`, `#include <string.h>`)
2. Configuration headers (`#include "config.h"`)
3. Project headers (`#include "antic.h"`, `#include "gtia.h"`)

## Error Handling

**JavaScript:**
- Try-catch blocks for exception handling
- Promise-based error handling with `.catch()` or try-await patterns
- Errors returned as response objects with `isError: true` flag
- Error messages included in response content for user-friendly feedback

Example from `mcp-server/index.js`:
```javascript
try {
  // operation
  return { content: [{ type: 'text', text: result }] };
} catch (error) {
  return {
    content: [{ type: 'text', text: `Error: ${error.message}...` }],
    isError: true,
  };
}
```

**Python:**
- Custom exceptions for connection errors (e.g., `ConnectionError`, `TimeoutError`)
- Context managers (`__enter__`/`__exit__`) for resource management
- Defensive checks before operations (e.g., `if not self.sock:`)

Example from `atari800_ai.py`:
```python
def connect(self):
    self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    self.sock.connect(self.socket_path)
    response = self._send({"cmd": "ping"})
    if response.get("status") != "ok":
        raise ConnectionError("Failed to connect to emulator")
    return self
```

**C:**
- Return codes for error indication (not observed in examined files, but standard in C)
- Inline error checking with conditional logic
- Preprocessor conditionals for platform-specific error handling

## Logging

**Framework:** console-based (Node.js), print-based (Python), custom (C)

**JavaScript:**
- Uses `console.error()` for status messages
- Only one logging call observed: `console.error('Atari 800 MCP Server running')`
- No structured logging framework in use

**Python:**
- Uses `print()` for user-facing output
- Includes formatted error messages with `print(f"...")`
- Error output goes to stdout, following shell conventions

**C:**
- Custom logging via `Log_print()` function in `log.c`
- Supports platform-specific output (macOS: `ControlManagerMessagePrint`, Android: `__android_log_write`, default: `printf`)
- Optional buffered logging with `BUFFERED_LOG` configuration

## Comments

**When to Comment:**
- Above function definitions explaining purpose and parameters (docstrings preferred)
- Before complex logic blocks explaining algorithm or workaround
- Inline comments for non-obvious decisions (rare in observed code)

**JSDoc/TSDoc:**
- Not used in JavaScript files
- Block comments document function purposes

Example from `mcp-server/index.js`:
```javascript
/**
 * Atari 800 MCP Server
 *
 * Provides MCP tools for controlling the Atari 800 emulator via AI interface.
 */
```

**Python docstrings:**
- Module-level docstring with usage examples (see `atari800_ai.py` top-level docstring)
- Class docstring explaining purpose: `"""Client for Atari800 AI interface"""`
- Function docstrings with one-line summary: `"""Connect to the emulator"""`
- Type hints for function signatures observed throughout

Example from `atari800_ai.py`:
```python
def _send(self, cmd: dict) -> dict:
    """Send a command and receive response"""
```

**C comments:**
- File header with copyright and license (GPL 2)
- Section headers with comment blocks: `/* GTIA Registers ---------------------------------------------------------- */`
- Inline comments for important constants: `/* Number of cycles per scanline. */`

## Function Design

**Size:** Functions tend to be pragmatic length; no strict limits enforced. Functions in `index.js` average 10-50 lines.

**Parameters:**
- JavaScript: Use object destructuring in MCP callbacks: `const { name, arguments: args } = request.params;`
- Python: Explicit parameters with type hints preferred: `def key(self, code: int, shift: bool = False) -> bool:`
- C: Minimal parameters; use global state for configuration

**Return Values:**
- JavaScript: Return consistent response object structures: `{ content: [{ type: 'text', text: '...' }], isError?: boolean }`
- Python: Return boolean for success/failure operations, dict/list for data queries
- C: Mix of return codes and side effects through globals

## Module Design

**Exports:**
- JavaScript: Use ES6 named and default exports; `mcp-server/index.js` is entry point
- Python: Module-level functions and classes available for import; example script in `if __name__ == "__main__":`
- C: Header files declare exports; implementation in `.c` files

**Barrel Files:** Not used in this codebase. Python uses direct imports.

**Organization patterns:**
- JavaScript: Single file modules (e.g., `index.js` contains server setup and tool handlers)
- Python: Class-based organization with private methods (prefix `_`): `Atari800AI` class encapsulates all emulator interactions
- C: Modular organization with paired `.h`/`.c` files; public APIs declared in headers, implementation in source

**Private vs Public:**
- Python: `_send()` method uses underscore prefix to indicate private/internal method
- C: Static functions for private scope, extern declarations for public APIs
- JavaScript: All functions are module-level private unless explicitly exported

---

*Convention analysis: 2026-03-20*

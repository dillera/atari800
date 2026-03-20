# Testing Patterns

**Analysis Date:** 2026-03-20

## Test Framework

**Status:** No automated testing framework detected

**No test files found** - The codebase contains no test files (`.test.js`, `.spec.js`, `.test.py`, `.spec.py`, etc.)

**No test runners configured** - No Jest, Vitest, Mocha, Pytest, or similar test framework configurations found in:
- `package.json` (mcp-server): No test dependencies
- `.eslintrc*`, `jest.config.*`, `vitest.config.*` files: Not present
- `pyproject.toml` or `setup.py`: Not present

**Build system:** Autotools-based C build (`configure.ac`, `Makefile.in`)

## Test File Organization

**Current State:**
- No test files or test directories exist
- No CI/CD test pipeline configured in `.github/` (workflows present but no test references)
- Manual testing appears to be the primary verification method

**Recommended Structure (if testing were added):**
- **JavaScript/Node.js**: `mcp-server/__tests__/` or `mcp-server/*.test.js` alongside source
- **Python**: `tests/test_atari800_ai.py` or `atari800_ai.test.py` alongside source
- **C**: `src/tests/` directory or alongside implementation files

## Testing Approach

**Manual Testing:**
- `mcp-server/README.md` describes manual usage of MCP tools
- `atari800_ai.py` includes `if __name__ == "__main__":` example usage block for manual verification

**Example: Python Module Self-Test**
```python
if __name__ == "__main__":
    import sys

    print("Atari800 AI Client")
    print("==================")

    try:
        with Atari800AI() as atari:
            print("Connected to emulator")

            # Get CPU state
            cpu = atari.cpu()
            print(f"CPU: PC=${cpu['pc']:04X} A=${cpu['a']:02X} X=${cpu['x']:02X} Y=${cpu['y']:02X}")

            # Show screen
            print("\nScreen:")
            atari.print_screen()

            # Run for a bit
            print("\nRunning 60 frames...")
            atari.run(frames=60)

            # Show screen again
            print("\nScreen after 60 frames:")
            atari.print_screen()

    except FileNotFoundError:
        print(f"Error: Emulator not running. Start with:")
        print(f"  ./src/atari800 -ai -xl -run your_program.xex")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)
```

## Test Patterns (Observed)

**Integration Test Pattern (Manual):**
Location: `atari800_ai.py` main block

Demonstrates:
- Connecting to running emulator with context manager
- Verifying connection with CPU state query
- Running emulation frames
- Verifying output by displaying screen

This serves as both documentation and a basic smoke test.

**Connection Verification Pattern:**
```python
def connect(self):
    """Connect to the emulator"""
    self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    self.sock.connect(self.socket_path)
    # Verify connection
    response = self._send({"cmd": "ping"})
    if response.get("status") != "ok":
        raise ConnectionError("Failed to connect to emulator")
    return self
```

Verifies:
- Socket connectivity
- Emulator responsiveness (ping command)
- Protocol correctness (response parsing)

## Error Cases Tested (Manual)

**Python (`atari800_ai.py`):**
- `FileNotFoundError`: Handles emulator not running when connecting
- `ConnectionError`: Handles failed ping verification
- `ConnectionError`: Handles socket closure during communication
- `json.JSONDecodeError`: Handles malformed responses (caught in `_send()`)

**JavaScript (`mcp-server/index.js`):**
- Socket connection timeouts: `client.setTimeout(10000)`
- JSON parse errors: `reject(new Error(\`Failed to parse response...\`));`
- Network errors: `client.on('error', (err) => { reject(err); })`
- Tool not found: `default:` case returns error response
- Emulator not running: Catch block provides helpful message

## Mocking

**Current State:** No mocking framework or patterns detected

**Manual Stub Pattern (Observed):**
- `mcp-server/index.js` simulates emulator responses from actual socket communication
- `atari800_ai.py` waits for real emulator socket rather than mocking

**For Future Tests:**
- **JavaScript**: Mock `net.createConnection()` to simulate socket communication
- **Python**: Mock `socket.socket()` for isolated unit tests
- **Protocol**: JSON command/response protocol can be stubbed with fixed response objects

Example of what could be mocked:
```javascript
// Mock socket that returns pre-defined responses
const mockSocket = {
  write: (data) => {},
  on: (event, handler) => {
    if (event === 'data') {
      // Simulate emulator response
      handler(JSON.stringify({ status: 'ok', data: [...] }));
    }
  },
  end: () => {},
};
```

## Coverage

**Current Status:** Not enforced

**No coverage tools detected** - No Jest coverage, Pytest coverage, or LCOV configuration

**Implicit Coverage Gaps:**
- All C code (`src/` directory) - No unit test coverage visible
- All JavaScript code (`mcp-server/index.js`) - No test coverage
- Python client library (`atari800_ai.py`) - No test coverage

## Test Types

**Unit Tests:** None detected

**Integration Tests:**
- Manual: `atari800_ai.py` main block demonstrates end-to-end integration
- Manual: MCP tools can be tested via Claude Code integration

**E2E Tests:**
- None automated
- Manual testing via emulator: Requires running actual Atari800 emulator with AI interface

**Fuzzing/Property Testing:** None

## Common Testing Scenarios (For Future Implementation)

**JavaScript/MCP Server:**
```javascript
// Socket send/receive cycle
describe('sendCommand', () => {
  it('should send command and receive response', async () => {
    const cmd = { cmd: 'ping' };
    const response = await sendCommand(cmd);
    expect(response.status).toBe('ok');
  });

  it('should handle connection timeout', async () => {
    // Would need socket mock that delays
  });

  it('should parse malformed JSON gracefully', async () => {
    // Would need socket mock that returns invalid JSON
  });
});
```

**Python Client:**
```python
# Connection lifecycle
def test_context_manager():
    with Atari800AI() as atari:
        assert atari.sock is not None
    # socket should be closed after context

def test_ping_failure():
    atari = Atari800AI()
    # Mock socket.connect to succeed
    # Mock _send to return {"status": "error"}
    with pytest.raises(ConnectionError):
        atari.connect()
```

**Protocol Validation:**
```python
# Message format: [length]\n[json]
def test_send_message_format():
    # Verify sent data is: f"{len(data)}\n" + json.dumps(cmd)

def test_receive_message_format():
    # Verify parsing: read until \n, parse length, read that many bytes
```

## Continuous Integration

**Current State:** No automated test CI/CD

**`.github/` directory exists** - Contains CI workflow files but no test automation detected

**Build System:**
- `Makefile` exists for C build
- No make test target visible from structure
- `build_ai.sh` script for AI interface build

**For Future Setup:**
```bash
npm test              # Run MCP server tests
python -m pytest      # Run Python tests
make test            # Run C tests (if implemented)
```

## Documentation of Testable APIs

**JavaScript:**
- `sendCommand(cmd: object): Promise<object>` - Sends JSON-RPC style commands to emulator socket
- `isEmulatorRunning(): boolean` - Checks if socket exists
- `formatScreen(data: array): string` - Formats array to ASCII display
- Tool handlers for each MCP operation

**Python:**
- `Atari800AI.connect()` - Establishes connection with verification
- `Atari800AI._send(cmd: dict): dict` - Core protocol implementation
- High-level APIs: `run()`, `screen_ascii()`, `cpu()`, `peek()`, `poke()`, etc.
- Helper: `wait_for_emulator(socket_path, timeout)` - Retry pattern for connection

**C Emulator:**
- AI interface exposed via Unix socket at `/tmp/atari800_ai.sock`
- JSON command protocol for querying/controlling emulator state
- Commands: `ping`, `screen_ascii`, `cpu`, `gtia`, `pokey`, `antic`, `pia`, `run`, `key`, `joystick`, `peek`, `poke`, etc.

---

*Testing analysis: 2026-03-20*

#!/usr/bin/env python3
"""
Puppetmaster — Full-Stack KillZone Orchestrator

Puppetmaster launches and coordinates all three components of the KillZone stack:
1. ZoneServer (Node.js TCP game server)
2. Atari800 emulator (with AI socket interface)
3. KillZone client (Atari program running on the emulator)

It uses the emulator's AI socket to act as a player — joining the game,
moving around, and observing state — while also monitoring the server's
log output for debugging and troubleshooting.

Usage:
    # Full stack: launch server + emulator + join game + move around
    python3 puppetmaster.py --launch --demo killzone.xex

    # Full stack with custom server path
    python3 puppetmaster.py --launch --server-dir /path/to/zoneserver --demo killzone.xex

    # Just emulator (server already running)
    python3 puppetmaster.py --launch --no-server --demo killzone.xex

    # Connect to already-running emulator (no launch)
    python3 puppetmaster.py --demo killzone.xex

    # Screen capture mode: dump what the Atari sees
    python3 puppetmaster.py --launch --screen killzone.xex
"""

import os
import sys
import time
import signal
import threading
import subprocess
import argparse

from atari800_ai import Atari800AI, wait_for_emulator


# Default paths
DEFAULT_EMULATOR = "./src/atari800"
DEFAULT_SOCKET = "/tmp/atari800_ai.sock"
DEFAULT_SERVER_DIR = "/Users/dillera/code/killzone/zoneserver"


class ZoneServerManager:
    """Manages the KillZone zoneserver (Node.js) subprocess.

    Launches the server, captures stdout/stderr in a background thread,
    and provides methods to check status and read recent log lines.
    """

    def __init__(self, server_dir: str = DEFAULT_SERVER_DIR):
        self.server_dir = server_dir
        self._proc = None
        self._log_lines = []
        self._log_lock = threading.Lock()
        self._reader_thread = None

    def start(self, timeout: float = 10.0) -> None:
        """Start the zoneserver and wait until it's listening.

        Args:
            timeout: Seconds to wait for server startup.

        Raises SystemExit if server fails to start.
        """
        server_js = os.path.join(self.server_dir, "src", "server.js")
        if not os.path.exists(server_js):
            print(f"Error: Server not found at {server_js}", file=sys.stderr)
            sys.exit(1)

        # Check if npm dependencies are installed
        node_modules = os.path.join(self.server_dir, "node_modules")
        if not os.path.isdir(node_modules):
            print("Installing zoneserver dependencies...")
            subprocess.run(
                ["npm", "install"],
                cwd=self.server_dir,
                capture_output=True,
                check=True
            )

        print(f"Starting zoneserver from {self.server_dir}...")
        self._proc = subprocess.Popen(
            ["node", "src/server.js"],
            cwd=self.server_dir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True
        )

        # Start background thread to read server output
        self._reader_thread = threading.Thread(target=self._read_output, daemon=True)
        self._reader_thread.start()

        # Wait for server ready message
        start = time.time()
        while time.time() - start < timeout:
            with self._log_lock:
                full_log = "\n".join(self._log_lines)
            if "KillZone Server running" in full_log or "KillZone TCP Server running" in full_log:
                print("ZoneServer started successfully.")
                return
            time.sleep(0.2)

        print("Warning: Server may not be fully ready (timed out waiting for startup message).",
              file=sys.stderr)

    def _read_output(self) -> None:
        """Background thread: read server stdout line by line."""
        for line in self._proc.stdout:
            line = line.rstrip("\n")
            with self._log_lock:
                self._log_lines.append(line)
            # Print server output with prefix for easy identification
            print(f"  [server] {line}")

    def get_log(self) -> list:
        """Return a copy of all captured log lines."""
        with self._log_lock:
            return list(self._log_lines)

    def get_recent_log(self, n: int = 20) -> list:
        """Return the last N log lines."""
        with self._log_lock:
            return list(self._log_lines[-n:])

    def has_log_match(self, text: str) -> bool:
        """Check if any log line contains the given text (case-insensitive)."""
        text_lower = text.lower()
        with self._log_lock:
            return any(text_lower in line.lower() for line in self._log_lines)

    def is_running(self) -> bool:
        """Check if the server process is still running."""
        return self._proc is not None and self._proc.poll() is None

    def stop(self) -> None:
        """Stop the zoneserver gracefully (SIGTERM), then force-kill if needed."""
        if self._proc is not None and self._proc.poll() is None:
            print("Stopping zoneserver...")
            self._proc.terminate()
            try:
                self._proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._proc.kill()
            self._proc = None


class PuppetmasterController:
    """Controls the KillZone Atari client via the atari800 AI socket interface.

    Wraps Atari800AI to provide named movement/action methods and lifecycle
    management (launch, connect, disconnect).

    Key codes match Atari800AI constants:
        W=46, A=63, S=62, D=58, Space=33, Q=47, Y=43, N=35
    """

    def __init__(
        self,
        emulator_path: str = DEFAULT_EMULATOR,
        xex_path: str = None,
        socket_path: str = DEFAULT_SOCKET
    ):
        self.emulator_path = emulator_path
        self.xex_path = xex_path
        self.socket_path = socket_path
        self._client = None
        self._proc = None

    def connect(self) -> None:
        """Connect to an already-running atari800 instance."""
        try:
            client = Atari800AI(self.socket_path)
            client.connect()
            self._client = client
            print(f"Connected to emulator at {self.socket_path}")
        except (ConnectionError, FileNotFoundError):
            print(
                f"Error: Could not connect to emulator at {self.socket_path}",
                file=sys.stderr
            )
            sys.exit(1)

    def launch_and_connect(self, netsio_port: int = 9997, timeout: float = 15.0) -> None:
        """Launch atari800 as a subprocess, then wait for it to be ready."""
        cmd = [self.emulator_path, "-ai", "-netsio", str(netsio_port), "-xl", "-run", self.xex_path]
        print(f"Launching emulator: {' '.join(cmd)}")
        self._proc = subprocess.Popen(cmd)
        try:
            self._client = wait_for_emulator(self.socket_path, timeout=timeout)
            print(f"Connected to emulator at {self.socket_path}")
        except TimeoutError:
            print(
                f"Error: Emulator did not start within {timeout}s",
                file=sys.stderr
            )
            sys.exit(1)

    def disconnect(self) -> None:
        """Disconnect from the emulator and terminate the subprocess if running."""
        if self._client is not None:
            self._client.disconnect()
            self._client = None
        if self._proc is not None:
            self._proc.terminate()
            self._proc = None

    def __enter__(self) -> "PuppetmasterController":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> bool:
        self.disconnect()
        return False

    # === Key input ===

    def press_key(self, code: int, hold_frames: int = 5) -> None:
        """Press and release a key."""
        self._client.key(code)
        self._client.run(frames=hold_frames)
        self._client.key_release()

    # === Movement keys ===

    def move_up(self) -> None:
        self.press_key(Atari800AI.AKEY_W)

    def move_down(self) -> None:
        self.press_key(Atari800AI.AKEY_S)

    def move_left(self) -> None:
        self.press_key(Atari800AI.AKEY_A)

    def move_right(self) -> None:
        self.press_key(Atari800AI.AKEY_D)

    # === Action keys ===

    def attack(self) -> None:
        self.press_key(Atari800AI.AKEY_SPACE)

    def menu_q(self) -> None:
        self.press_key(Atari800AI.AKEY_Q)

    def menu_y(self) -> None:
        self.press_key(Atari800AI.AKEY_Y)

    def menu_n(self) -> None:
        self.press_key(Atari800AI.AKEY_N)

    # === Game startup ===

    def type_string(self, text: str, frame_delay: int = 5) -> None:
        """Type a string of characters."""
        self._client.type_string(text, frame_delay=frame_delay)

    def press_return(self) -> None:
        self.press_key(Atari800AI.AKEY_RETURN)

    def run_frames(self, frames: int = 1) -> None:
        """Run the emulator for N frames."""
        self._client.run(frames=frames)

    def screen_text(self) -> list:
        """Get the current screen as a list of 40-char strings (actual text from RAM)."""
        return self._client.screen_text()

    def screen_ascii(self) -> list:
        """Get pixel-brightness view of the screen (NOT text)."""
        return self._client.screen_ascii()

    def print_screen(self) -> None:
        """Print the current emulator screen text."""
        self._client.print_screen_text()

    def wait_for_screen_text(self, text: str, timeout: float = 30.0, poll_frames: int = 30) -> bool:
        """Poll screen text memory until the given text appears, or timeout."""
        text_lower = text.lower()
        start = time.time()
        while time.time() - start < timeout:
            self._client.run(frames=poll_frames)
            lines = self._client.screen_text()
            screen = "\n".join(lines).lower()
            if text_lower in screen:
                return True
        return False

    def read_full_screen(self) -> str:
        """Read and display the full emulator screen text, returning it as a string."""
        lines = self.screen_text()
        screen_text = "\n".join(lines)
        print("+----------------------------------------+")
        for line in lines:
            print(f"|{line}|")
        print("+----------------------------------------+")
        return screen_text

    def verify_client_connection(
        self,
        server: "ZoneServerManager",
        timeout: float = 30.0,
        poll_interval: float = 1.0
    ) -> bool:
        """Wait for the zoneserver to log that the KillZone client connected on port 3001.

        Polls the server log for 'TCP Client connected'. If it doesn't appear
        within the timeout, runs diagnostics and returns False.

        Args:
            server: The ZoneServerManager instance to check logs on.
            timeout: Seconds to wait for the connection log entry.
            poll_interval: Seconds between each check.

        Returns:
            True if the server confirmed the client connection, False otherwise.
        """
        print(f"\nWaiting for KillZone client to connect to zoneserver (timeout {timeout}s)...")
        start = time.time()
        while time.time() - start < timeout:
            # Run a few frames so the emulator/client can make progress
            self.run_frames(30)

            if server.has_log_match("TCP Client connected"):
                elapsed = time.time() - start
                print(f"  Server confirmed client TCP connection ({elapsed:.1f}s)")
                return True

            time.sleep(poll_interval)

        # --- Connection failed: run diagnostics ---
        print("\n*** CLIENT CONNECTION FAILED — running diagnostics ***\n", file=sys.stderr)
        self._diagnose_connection(server)
        return False

    def _diagnose_connection(self, server: "ZoneServerManager") -> None:
        """Print diagnostics when the KillZone client fails to connect to the server."""

        # 1. Is the zoneserver still running?
        if not server.is_running():
            print("  PROBLEM: ZoneServer process has exited!", file=sys.stderr)
            print("  Recent server output:", file=sys.stderr)
            for line in server.get_recent_log(10):
                print(f"    [server] {line}", file=sys.stderr)
            return

        # 2. Did the server start listening on port 3001?
        if not server.has_log_match("TCP Server running on port"):
            print("  PROBLEM: ZoneServer never logged 'TCP Server running on port'.", file=sys.stderr)
            print("  The server may have crashed during startup.", file=sys.stderr)
            print("  Recent server output:", file=sys.stderr)
            for line in server.get_recent_log(10):
                print(f"    [server] {line}", file=sys.stderr)
            return

        # 3. What's on the emulator screen right now?
        print("  Server is running and listening, but no client connected.", file=sys.stderr)
        print("  Current emulator screen:", file=sys.stderr)
        self.run_frames(30)
        lines = self.screen_text()
        for line in lines:
            print(f"    |{line}|", file=sys.stderr)

        # 4. Check for common screen clues
        screen_text = "\n".join(lines).lower()
        if "error" in screen_text or "fail" in screen_text:
            print("  CLUE: Screen contains 'error' or 'fail' — the client may have "
                  "hit a network or config issue.", file=sys.stderr)
        elif "menu" in screen_text or "press" in screen_text:
            print("  CLUE: Client appears to be at a menu. It may not have attempted "
                  "to connect yet, or the server address/port may be wrong.", file=sys.stderr)
        elif not any(c.strip() for c in lines):
            print("  CLUE: Screen is blank — the XEX may not have loaded, or the "
                  "emulator may still be booting.", file=sys.stderr)
        else:
            print("  No obvious clue on screen. The client may be using a different "
                  "server address or port than what the zoneserver is listening on.",
                  file=sys.stderr)

        # 5. Dump recent server logs for context
        print("\n  Recent server log:", file=sys.stderr)
        for line in server.get_recent_log(15):
            print(f"    [server] {line}", file=sys.stderr)

    def wait_for_game_startup(
        self,
        player_name: str = "puppet",
        server: "ZoneServerManager" = None,
        timeout: float = 30.0
    ) -> bool:
        """Smart KillZone startup: verify connection, then enter player name.

        Flow:
        1. Let the emulator boot and read the initial screen
        2. If a server manager is provided, verify the client connected to it
        3. Wait for the 'player name' prompt on screen
        4. Type the player name and press Return
        5. Verify the join was accepted in server logs

        Args:
            player_name: Name to enter at the join prompt.
            server: Optional ZoneServerManager to verify connection against.
            timeout: Seconds to wait for each step.

        Returns:
            True if the full startup succeeded, False if connection or join failed.
        """
        # --- Step 1: Let the emulator boot and show the initial screen ---
        print("\n=== KillZone Startup ===")
        print("Letting emulator boot (running 120 frames)...")
        self.run_frames(120)
        print("\nInitial screen:")
        self.read_full_screen()

        # --- Step 2: Verify client connected to server ---
        if server is not None:
            if not self.verify_client_connection(server, timeout=timeout):
                print("\nAborting: client never connected to zoneserver.", file=sys.stderr)
                return False
        else:
            print("No server manager — skipping connection verification.")

        # --- Step 3: Wait for 'player name' prompt ---
        print("\nWaiting for 'player name' prompt on screen...")
        if not self.wait_for_screen_text("player name", timeout=timeout):
            print("Timed out waiting for 'player name' prompt.", file=sys.stderr)
            print("Current screen:")
            self.read_full_screen()
            return False
        print("  'player name' prompt detected!")
        self.read_full_screen()

        # --- Step 4: Type the player name and press Return ---
        print(f"\nTyping player name: {player_name}")
        self.type_string(player_name)
        self._client.run(frames=10)
        print("Pressing Return to join...")
        self.press_return()
        self._client.run(frames=60)

        # --- Step 5: Verify join in server logs ---
        if server is not None:
            print("Verifying join in server logs...")
            join_confirmed = False
            for _ in range(10):
                time.sleep(0.3)
                if (server.has_log_match("TCP Join Request")
                        or server.has_log_match("TCP Join:")
                        or server.has_log_match("TCP Rejoin:")):
                    join_confirmed = True
                    break
            if join_confirmed:
                print("  Server confirmed player join!")
            else:
                print("  Warning: join not confirmed in server logs yet.", file=sys.stderr)
                print("  Recent server log:", file=sys.stderr)
                for line in server.get_recent_log(10):
                    print(f"    [server] {line}", file=sys.stderr)

        # Show post-join screen
        self.run_frames(60)
        print("\nPost-join screen:")
        self.read_full_screen()
        print("Game startup complete.")
        return True


def main():
    parser = argparse.ArgumentParser(
        description="Puppetmaster: Full-stack KillZone orchestrator — server + emulator + AI control"
    )
    parser.add_argument(
        "xex",
        help="Path to KillZone .xex file to run"
    )

    # Launch options
    launch_group = parser.add_argument_group("launch options")
    launch_group.add_argument(
        "--launch",
        action="store_true",
        help="Launch atari800 emulator before connecting"
    )
    launch_group.add_argument(
        "--emulator",
        default=DEFAULT_EMULATOR,
        help=f"Path to atari800 binary (default: {DEFAULT_EMULATOR})"
    )
    launch_group.add_argument(
        "--socket",
        default=DEFAULT_SOCKET,
        help=f"Path to AI socket (default: {DEFAULT_SOCKET})"
    )
    launch_group.add_argument(
        "--netsio-port",
        type=int,
        default=9997,
        help="NetSIO UDP port for FujiNet-PC bridge (default: 9997)"
    )

    # Server options
    server_group = parser.add_argument_group("server options")
    server_group.add_argument(
        "--no-server",
        action="store_true",
        help="Don't launch zoneserver (assume it's already running)"
    )
    server_group.add_argument(
        "--server-dir",
        default=DEFAULT_SERVER_DIR,
        help=f"Path to zoneserver directory (default: {DEFAULT_SERVER_DIR})"
    )

    # Game options
    game_group = parser.add_argument_group("game options")
    game_group.add_argument(
        "--name",
        default="puppet",
        help="Player name to enter at KillZone join prompt (default: puppet)"
    )
    game_group.add_argument(
        "--no-startup",
        action="store_true",
        help="Skip KillZone startup sequence (assume already in-game)"
    )

    # Action modes
    action_group = parser.add_argument_group("action modes")
    action_group.add_argument(
        "--connect-only",
        action="store_true",
        help="Connect to emulator, print confirmation, then exit"
    )
    action_group.add_argument(
        "--keys",
        action="store_true",
        help="Send one press of each key (W A S D Space Q Y N)"
    )
    action_group.add_argument(
        "--demo",
        action="store_true",
        help="Send demo movement sequence (up/down/left/right)"
    )
    action_group.add_argument(
        "--screen",
        action="store_true",
        help="Dump the current emulator screen and exit"
    )
    action_group.add_argument(
        "--server-log",
        action="store_true",
        help="Show recent zoneserver log lines and exit"
    )

    args = parser.parse_args()

    server = None
    controller = None

    def cleanup(signum=None, frame=None):
        """Clean shutdown of all components."""
        if controller:
            controller.disconnect()
        if server:
            server.stop()
        if signum:
            sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    try:
        # --- Step 1: Start zoneserver (if needed) ---
        if args.launch and not args.no_server:
            server = ZoneServerManager(server_dir=args.server_dir)
            server.start()

        # --- Step 2: Launch/connect emulator ---
        controller = PuppetmasterController(
            emulator_path=args.emulator,
            xex_path=args.xex,
            socket_path=args.socket
        )

        if args.launch:
            controller.launch_and_connect(netsio_port=args.netsio_port)
        else:
            controller.connect()

        print("Puppetmaster ready. Emulator connected.")

        # --- Quick-exit modes ---
        if args.connect_only:
            cleanup()
            sys.exit(0)

        if args.screen:
            controller.run_frames(60)
            controller.print_screen()
            cleanup()
            sys.exit(0)

        if args.server_log and server:
            for line in server.get_recent_log(30):
                print(f"  [server] {line}")
            cleanup()
            sys.exit(0)

        # --- Step 3: KillZone startup (join game) ---
        needs_startup = (args.keys or args.demo) and not args.no_startup
        if needs_startup:
            success = controller.wait_for_game_startup(
                player_name=args.name,
                server=server,
            )
            if not success:
                print("Startup failed — exiting.", file=sys.stderr)
                cleanup()
                sys.exit(1)

        # --- Step 4: Run action mode ---
        if args.keys:
            key_sequence = [
                ("W", "move up", controller.move_up),
                ("A", "move left", controller.move_left),
                ("S", "move down", controller.move_down),
                ("D", "move right", controller.move_right),
                ("Space", "attack", controller.attack),
                ("Q", "menu Q", controller.menu_q),
                ("Y", "menu Y", controller.menu_y),
                ("N", "menu N", controller.menu_n),
            ]
            for key_name, desc, method in key_sequence:
                print(f"Sending {key_name} ({desc})...")
                method()
                controller.run_frames(10)
            print("Keys test complete.")

        if args.demo:
            print("Running demo sequence...")
            for direction, method in [
                ("up", controller.move_up),
                ("down", controller.move_down),
                ("left", controller.move_left),
                ("right", controller.move_right),
            ]:
                print(f"  Moving {direction}...")
                method()
                controller.run_frames(30)

                # Show screen after each move
                lines = controller.screen_text()
                if lines:
                    print(f"  Screen after move {direction}:")
                    for line in lines:
                        print(f"    |{line}|")

            print("Demo complete.")

            # Show server perspective
            if server:
                print("\nServer log (last 10 lines):")
                for line in server.get_recent_log(10):
                    print(f"  [server] {line}")

    finally:
        cleanup()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
Puppetmaster — Emulator Interface Controller for KillZone

Puppetmaster provides a Python script and library to launch or connect to the
atari800 emulator and send keyboard inputs to control the KillZone game client.

It wraps the Atari800AI client library and provides:
- A PuppetmasterController class for programmatic control
- A CLI entry point with --launch, --emulator, --socket, and --demo flags

Usage as library:
    from puppetmaster import PuppetmasterController

    controller = PuppetmasterController(
        emulator_path="./src/atari800",
        xex_path="killzone.xex"
    )
    with controller:
        controller.launch_and_connect()
        controller.move_up()
        controller.attack()

Usage as CLI:
    python3 puppetmaster.py --launch killzone.xex
    python3 puppetmaster.py --launch --demo killzone.xex
    python3 puppetmaster.py killzone.xex  # connect to already-running emulator
"""

import sys
import subprocess
import argparse

from atari800_ai import Atari800AI, wait_for_emulator


# Default paths
DEFAULT_EMULATOR = "./src/atari800"
DEFAULT_SOCKET = "/tmp/atari800_ai.sock"


class PuppetmasterController:
    """Controls the KillZone Atari client via the atari800 AI socket interface.

    Wraps Atari800AI to provide named movement/action methods and lifecycle
    management (launch, connect, disconnect).

    Key codes match Atari800AI constants (D-08):
        W=46, A=63, S=62, D=58, Space=33, Q=47, Y=43, N=35
    """

    def __init__(
        self,
        emulator_path: str = DEFAULT_EMULATOR,
        xex_path: str = None,
        socket_path: str = DEFAULT_SOCKET
    ):
        """Initialize controller with paths.

        Args:
            emulator_path: Path to the atari800 binary.
            xex_path: Path to the KillZone .xex file.
            socket_path: Path to the AI Unix domain socket.
        """
        self.emulator_path = emulator_path
        self.xex_path = xex_path
        self.socket_path = socket_path
        self._client = None
        self._proc = None

    def connect(self) -> None:
        """Connect to an already-running atari800 instance.

        Raises SystemExit with code 1 if the connection fails (per D-06).
        Prints a confirmation message on success.
        """
        try:
            client = Atari800AI(self.socket_path)
            client.connect()
            self._client = client
            print(f"Connected to emulator at {self.socket_path}")
        except (ConnectionError, FileNotFoundError) as e:
            print(
                f"Error: Could not connect to emulator at {self.socket_path}",
                file=sys.stderr
            )
            sys.exit(1)

    def launch_and_connect(self, timeout: float = 15.0) -> None:
        """Launch atari800 as a subprocess, then wait for it to be ready.

        Spawns: atari800 -ai -xl -run <xex_path>  (per D-05)
        Polls the socket until the emulator is available or timeout is reached.

        Args:
            timeout: Seconds to wait for emulator startup (default 15.0).

        Raises SystemExit with code 1 if the emulator does not start in time.
        """
        self._proc = subprocess.Popen(
            [self.emulator_path, "-ai", "-xl", "-run", self.xex_path]
        )
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
        """Support use as a context manager."""
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> bool:
        """Disconnect on context manager exit. Does not suppress exceptions."""
        self.disconnect()
        return False

    # === Key input ===

    def press_key(self, code: int, hold_frames: int = 5) -> None:
        """Press and release a key using proven type_char timing (per D-09).

        Sequence: key(code) -> run(frames=hold_frames) -> key_release()

        Args:
            code: Atari key code (AKEY_* constant).
            hold_frames: Number of frames to hold the key before release (default 5).
        """
        self._client.key(code)
        self._client.run(frames=hold_frames)
        self._client.key_release()

    # === Movement keys ===

    def move_up(self) -> None:
        """Move player up (W key, AKEY_W = 46)."""
        self.press_key(Atari800AI.AKEY_W)

    def move_down(self) -> None:
        """Move player down (S key, AKEY_S = 62)."""
        self.press_key(Atari800AI.AKEY_S)

    def move_left(self) -> None:
        """Move player left (A key, AKEY_A = 63)."""
        self.press_key(Atari800AI.AKEY_A)

    def move_right(self) -> None:
        """Move player right (D key, AKEY_D = 58)."""
        self.press_key(Atari800AI.AKEY_D)

    # === Action keys ===

    def attack(self) -> None:
        """Attack (Space key, AKEY_SPACE = 33)."""
        self.press_key(Atari800AI.AKEY_SPACE)

    def menu_q(self) -> None:
        """Press Q for menu quit/back (AKEY_Q = 47)."""
        self.press_key(Atari800AI.AKEY_Q)

    def menu_y(self) -> None:
        """Press Y for menu yes (AKEY_Y = 43)."""
        self.press_key(Atari800AI.AKEY_Y)

    def menu_n(self) -> None:
        """Press N for menu no (AKEY_N = 35)."""
        self.press_key(Atari800AI.AKEY_N)


def main():
    """CLI entry point for Puppetmaster.

    Usage:
        python3 puppetmaster.py killzone.xex
        python3 puppetmaster.py --launch killzone.xex
        python3 puppetmaster.py --launch --emulator /usr/local/bin/atari800 killzone.xex
        python3 puppetmaster.py --launch --demo killzone.xex
    """
    parser = argparse.ArgumentParser(
        description="Puppetmaster: Control the KillZone Atari client via AI socket"
    )
    parser.add_argument(
        "xex",
        help="Path to KillZone .xex file to run"
    )
    parser.add_argument(
        "--launch",
        action="store_true",
        help="Launch atari800 emulator before connecting"
    )
    parser.add_argument(
        "--emulator",
        default=DEFAULT_EMULATOR,
        help="Path to atari800 binary (default: ./src/atari800)"
    )
    parser.add_argument(
        "--socket",
        default=DEFAULT_SOCKET,
        help="Path to AI socket (default: /tmp/atari800_ai.sock)"
    )
    parser.add_argument(
        "--demo",
        action="store_true",
        help="Send demo key sequence after connecting"
    )
    parser.add_argument(
        "--keys",
        action="store_true",
        help="Send one press of each key (W A S D Space Q Y N) with 10-frame pauses"
    )
    parser.add_argument(
        "--connect-only",
        action="store_true",
        help="Connect to emulator, print confirmation, then exit"
    )

    args = parser.parse_args()

    controller = PuppetmasterController(
        emulator_path=args.emulator,
        xex_path=args.xex,
        socket_path=args.socket
    )

    try:
        if args.launch:
            controller.launch_and_connect()
        else:
            controller.connect()

        print("Puppetmaster ready. Emulator connected.")

        if args.connect_only:
            sys.exit(0)

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
                controller._client.run(frames=10)
            print("Keys test complete.")

        if args.demo:
            print("Running demo sequence...")
            controller.move_up()
            controller._client.run(frames=10)
            controller.move_down()
            controller._client.run(frames=10)
            controller.move_left()
            controller._client.run(frames=10)
            controller.move_right()
            controller._client.run(frames=10)
            print("Demo complete.")
    finally:
        controller.disconnect()


if __name__ == "__main__":
    main()

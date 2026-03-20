"""
Tests for PuppetmasterController class.

Tests validate that key press methods use correct AKEY codes,
press_key sequence is correct (key -> run -> key_release),
connection errors result in SystemExit, and subprocess launch works.
"""

import sys
import subprocess
import pytest
from unittest.mock import MagicMock, patch, call


# We import after mocking to avoid actual socket connections
# Import the module under test
from puppetmaster import PuppetmasterController


def make_controller():
    """Create a PuppetmasterController with test paths."""
    return PuppetmasterController(
        emulator_path="./src/atari800",
        xex_path="killzone.xex",
        socket_path="/tmp/atari800_ai.sock"
    )


def make_controller_with_mock_client():
    """Create a controller with a mock _client already set."""
    controller = make_controller()
    mock_client = MagicMock()
    controller._client = mock_client
    return controller, mock_client


class TestPressKey:
    def test_press_key_sends_key_then_run_then_release(self):
        """press_key must call key(), then run(frames=N), then key_release() in order."""
        controller, mock_client = make_controller_with_mock_client()
        mock_client.run.return_value = {"status": "ok"}

        controller.press_key(46, hold_frames=5)

        # Verify call sequence and order
        mock_client.key.assert_called_once_with(46)
        mock_client.run.assert_called_once_with(frames=5)
        mock_client.key_release.assert_called_once()

        # Verify ordering via call_args_list on parent mock
        parent = MagicMock()
        parent.key = mock_client.key
        parent.run = mock_client.run
        parent.key_release = mock_client.key_release

        # Re-test with tracked call order
        controller2, mock_client2 = make_controller_with_mock_client()
        manager = MagicMock()
        manager.attach_mock(mock_client2.key, "key")
        manager.attach_mock(mock_client2.run, "run")
        manager.attach_mock(mock_client2.key_release, "key_release")

        controller2.press_key(46, hold_frames=5)

        expected_calls = [
            call.key(46),
            call.run(frames=5),
            call.key_release(),
        ]
        assert manager.mock_calls == expected_calls


class TestMovementKeys:
    def test_move_up_uses_akey_w(self):
        """move_up() must use AKEY_W = 46."""
        controller, mock_client = make_controller_with_mock_client()
        controller.move_up()
        mock_client.key.assert_called_once_with(46)

    def test_move_left_uses_akey_a(self):
        """move_left() must use AKEY_A = 63."""
        controller, mock_client = make_controller_with_mock_client()
        controller.move_left()
        mock_client.key.assert_called_once_with(63)

    def test_move_down_uses_akey_s(self):
        """move_down() must use AKEY_S = 62."""
        controller, mock_client = make_controller_with_mock_client()
        controller.move_down()
        mock_client.key.assert_called_once_with(62)

    def test_move_right_uses_akey_d(self):
        """move_right() must use AKEY_D = 58."""
        controller, mock_client = make_controller_with_mock_client()
        controller.move_right()
        mock_client.key.assert_called_once_with(58)


class TestActionKeys:
    def test_attack_uses_akey_space(self):
        """attack() must use AKEY_SPACE = 33."""
        controller, mock_client = make_controller_with_mock_client()
        controller.attack()
        mock_client.key.assert_called_once_with(33)

    def test_menu_q_uses_akey_q(self):
        """menu_q() must use AKEY_Q = 47."""
        controller, mock_client = make_controller_with_mock_client()
        controller.menu_q()
        mock_client.key.assert_called_once_with(47)

    def test_menu_y_uses_akey_y(self):
        """menu_y() must use AKEY_Y = 43."""
        controller, mock_client = make_controller_with_mock_client()
        controller.menu_y()
        mock_client.key.assert_called_once_with(43)

    def test_menu_n_uses_akey_n(self):
        """menu_n() must use AKEY_N = 35."""
        controller, mock_client = make_controller_with_mock_client()
        controller.menu_n()
        mock_client.key.assert_called_once_with(35)


class TestConnect:
    def test_connect_exits_on_connection_error(self):
        """connect() must call sys.exit(1) when ConnectionError is raised."""
        controller = make_controller()

        with patch("puppetmaster.Atari800AI") as MockAtari800AI:
            mock_instance = MagicMock()
            MockAtari800AI.return_value = mock_instance
            mock_instance.connect.side_effect = ConnectionError("refused")

            with pytest.raises(SystemExit) as exc_info:
                controller.connect()

            assert exc_info.value.code == 1

    def test_connect_exits_on_file_not_found_error(self):
        """connect() must call sys.exit(1) when FileNotFoundError is raised."""
        controller = make_controller()

        with patch("puppetmaster.Atari800AI") as MockAtari800AI:
            mock_instance = MagicMock()
            MockAtari800AI.return_value = mock_instance
            mock_instance.connect.side_effect = FileNotFoundError("no socket")

            with pytest.raises(SystemExit) as exc_info:
                controller.connect()

            assert exc_info.value.code == 1


class TestLaunch:
    def test_launch_spawns_subprocess(self):
        """launch_and_connect() must spawn subprocess with correct args."""
        controller = PuppetmasterController(
            emulator_path="./src/atari800",
            xex_path="killzone.xex",
            socket_path="/tmp/atari800_ai.sock"
        )

        mock_proc = MagicMock()
        mock_client = MagicMock()

        with patch("puppetmaster.subprocess.Popen") as mock_popen, \
             patch("puppetmaster.wait_for_emulator") as mock_wait:
            mock_popen.return_value = mock_proc
            mock_wait.return_value = mock_client

            controller.launch_and_connect(timeout=15.0)

            mock_popen.assert_called_once_with(
                ["./src/atari800", "-ai", "-xl", "-run", "killzone.xex"]
            )
            mock_wait.assert_called_once_with("/tmp/atari800_ai.sock", timeout=15.0)

"""Resource-free file dialog tests: python -m unittest unittests.test_nativefiledialog."""

import dataclasses
import importlib.util
import os
from pathlib import Path
import pickle
import sys
import tempfile
import threading
import types
import unittest
from unittest.mock import Mock, patch


module_name = "_test_renpy_nativefiledialog"
spec = importlib.util.spec_from_file_location(module_name, Path(__file__).parents[1] / "renpy" / "nativefiledialog.py")
nfd = importlib.util.module_from_spec(spec)
sys.modules[module_name] = nfd
fake_renpy = types.SimpleNamespace(
    game=types.SimpleNamespace(
        context=lambda: types.SimpleNamespace(init_phase=False), interface=types.SimpleNamespace(started=True)
    ),
    exports=types.SimpleNamespace(predicting=lambda: False, restart_interaction=Mock()),
    mobile=False,
    emscripten=False,
    macintosh=False,
    linux=True,
    pygame=types.SimpleNamespace(event=types.SimpleNamespace(clear=Mock())),
    display=types.SimpleNamespace(core=types.SimpleNamespace(input_events={1, 2})),
)
with patch.dict(sys.modules, {"renpy": fake_renpy}):
    spec.loader.exec_module(nfd)


class NativeFileDialogTests(unittest.TestCase):
    def tearDown(self):
        nfd._active = None
        nfd._retired.clear()

    def test_missing_parents_preserve_filename_without_creating_directories(self):
        with tempfile.TemporaryDirectory() as root:
            proposed = os.path.join(root, "missing", "child", "猫 100%.png")
            self.assertEqual(nfd._normalise_path("save", proposed), os.path.join(root, "猫 100%.png"))
            self.assertEqual(nfd._normalise_path("folder", os.path.dirname(proposed)), root)
            self.assertFalse(os.path.exists(os.path.join(root, "missing")))
            self.assertEqual(
                nfd._normalise_path("save", os.path.join(root, "missing", "child") + os.sep), root + os.sep
            )

    def test_existing_directory_and_root(self):
        with tempfile.TemporaryDirectory() as root:
            self.assertEqual(nfd._normalise_path("open", root), root + os.sep)
        root = os.path.abspath(os.sep)
        self.assertEqual(
            nfd._normalise_path("save", os.path.join(root, "missing", "file.png")), os.path.join(root, "file.png")
        )
        self.assertIsNone(nfd._normalise_path("save", None))

    def test_filters(self):
        filters = [("Images", ["*.png", "*.jpg", "*.jpeg", "*.bmp"])]
        self.assertEqual(nfd._normalise_filters("save", filters), (("Images", ("*.png", "*.jpg", "*.jpeg", "*.bmp")),))
        self.assertEqual(nfd._normalise_filters("folder", object()), ())
        for value in [
            [("PNG", "*.png")],
            [("PNG", [])],
            [("bad\0", ["*"])],
            [("bad|name", ["*"])],
            [("bad", ["[ab]*"])],
        ]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                nfd._normalise_filters("open", value)

    def test_file_uri_decoding_is_exactly_once(self):
        for uri, expected in [
            ("file:///tmp/%E7%8C%AB%20a%25100.png", "/tmp/猫 a%100.png"),
            ("file://localhost/tmp/a%2520b", "/tmp/a%20b"),
            ("file:///tmp/a%23b", "/tmp/a#b"),
        ]:
            self.assertEqual(nfd._file_uri_path(uri), expected)
        for uri in [
            "https://example.com/a",
            "file://remote/tmp/a",
            "file:relative",
            "file:///tmp/%00a",
            "file:///tmp/%",
            "file:///tmp/%GG",
            "file:///tmp/a?b",
            "file:///tmp/a#b",
            "\nfile:///tmp/a",
            "file:///tmp/a b",
            "file:///tmp/a\\b",
        ]:
            with self.subTest(uri=uri), self.assertRaises(ValueError):
                nfd._file_uri_path(uri)

    def test_request_is_immutable_and_save_restores_expired_handle(self):
        for status in ("pending", "selected", "cancelled", "error", "unavailable"):
            request = nfd.FileDialogRequest(nfd.FileDialogResult(status != "pending", status))
            self.assertIs(request.poll(), request.poll())
            with self.assertRaises(dataclasses.FrozenInstanceError):
                request.poll().status = "other"
            restored = pickle.loads(pickle.dumps(request))
            self.assertTrue(restored.poll().done)
            self.assertEqual(restored.poll().status, "cancelled")

    def test_reject_init_prediction_and_worker_calls(self):
        with patch.object(fake_renpy.exports, "predicting", return_value=True), self.assertRaises(RuntimeError):
            nfd.show_file_dialog("save")
        with (
            patch.object(fake_renpy.game, "context", return_value=types.SimpleNamespace(init_phase=True)),
            self.assertRaises(RuntimeError),
        ):
            nfd.show_file_dialog("save")
        with patch.object(threading, "current_thread", return_value=object()), self.assertRaises(RuntimeError):
            nfd.show_file_dialog("save")
        with patch.object(fake_renpy.game.interface, "started", False), self.assertRaises(RuntimeError):
            nfd.show_file_dialog("save")

    def test_busy_and_unsupported_are_terminal(self):
        nfd._active = object()
        self.assertEqual(nfd.show_file_dialog("save").poll().status, "error")
        nfd._active = None
        with patch.object(fake_renpy, "mobile", True):
            self.assertEqual(nfd.show_file_dialog("save").poll().status, "unavailable")

    def test_cancel_reset_and_late_result(self):
        native = Mock()
        request = nfd.FileDialogRequest()
        operation = nfd._Operation(request, native, "save", None, (), "Save")
        operation.started = True
        nfd._active = operation
        request.cancel()
        request.cancel()
        self.assertTrue(operation.cancelled.is_set())
        nfd.reset("reload")
        native.release_parent.assert_not_called()
        self.assertEqual(request.poll().status, "cancelled")
        self.assertEqual(request.poll().error, "reload")
        operation.results.put(nfd.FileDialogResult(True, "selected", "/tmp/stale"))
        nfd.update()
        self.assertFalse(nfd._retired)
        self.assertIsNone(request.poll().path)

    def test_completion_clears_input_and_restarts_interaction(self):
        native = Mock()
        request = nfd.FileDialogRequest()
        operation = nfd._Operation(request, native, "save", None, (), "Save")
        operation.started = True
        operation.results.put(nfd.FileDialogResult(True, "selected", "/tmp/猫.png", "portal"))
        nfd._active = operation
        nfd.update()
        self.assertFalse(nfd.active())
        self.assertEqual(request.poll().path, "/tmp/猫.png")
        native.release_parent.assert_called_once()
        fake_renpy.pygame.event.clear.assert_called()
        fake_renpy.exports.restart_interaction.assert_called()

    def test_fallback_never_uses_shell_and_preserves_spaces(self):
        process = Mock(returncode=0)
        process.communicate.return_value = (b"/tmp/ space .png\n", b"")
        with (
            patch.dict(os.environ, {"DISPLAY": ":0"}),
            patch.object(nfd.shutil, "which", return_value="/usr/bin/kdialog"),
            patch.object(nfd.subprocess, "Popen", return_value=process) as popen,
        ):
            result = nfd._fallback("save", "/tmp/a'\".png", (("PNG", ("*.png",)),), "Save ' \"", threading.Event())
        self.assertEqual(result.status, "selected")
        self.assertEqual(result.path, "/tmp/ space .png")
        self.assertNotIn("shell", popen.call_args.kwargs)
        self.assertIn("/tmp/a'\".png", popen.call_args.args[0])

    def test_fallback_exit_states(self):
        for code, out, err, status in [
            (1, b"", b"", "cancelled"),
            (1, b"", b"backend broke", "error"),
            (2, b"", b"", "error"),
            (0, b"", b"", "error"),
        ]:
            process = Mock(returncode=code)
            process.communicate.return_value = (out, err)
            with (
                patch.dict(os.environ, {"DISPLAY": ":0"}),
                patch.object(nfd.shutil, "which", return_value="/usr/bin/zenity"),
                patch.object(nfd.subprocess, "Popen", return_value=process),
            ):
                self.assertEqual(nfd._fallback("open", None, (), "Open", threading.Event()).status, status)

    def test_fallback_start_failure_is_error(self):
        with (
            patch.dict(os.environ, {"DISPLAY": ":0"}),
            patch.object(nfd.shutil, "which", return_value="/usr/bin/kdialog"),
            patch.object(nfd.subprocess, "Popen", side_effect=OSError("Cannot launch chooser")),
        ):
            result = nfd._fallback("open", None, (), "Open", threading.Event())
        self.assertEqual((result.status, result.backend), ("error", "kdialog"))
        self.assertIn("Cannot launch", result.error)

    def test_portal_only_falls_back_when_unavailable(self):
        for status in ("selected", "cancelled", "error", "unavailable"):
            native = Mock()
            native.result.return_value = (status, "file:///tmp/selected.png", "portal", None, False)
            operation = nfd._Operation(nfd.FileDialogRequest(), native, "open", None, (), "Open")
            with patch.object(nfd, "_wake"), patch.object(nfd, "_fallback") as fallback:
                fallback.return_value = nfd.FileDialogResult(True, "selected", "/tmp/fallback.png", "zenity")
                operation.run()
                result = operation.results.get_nowait()
                self.assertEqual(fallback.call_count, int(status == "unavailable"))
                self.assertEqual(result.status, "selected" if status == "unavailable" else status)

    def test_fallback_kills_unresponsive_cancelled_process(self):
        process = Mock()
        cancelled = threading.Event()

        def communicate(timeout=None):
            if timeout == 0.05:
                cancelled.set()
                raise nfd.subprocess.TimeoutExpired("dialog", timeout)
            if timeout == 2:
                raise nfd.subprocess.TimeoutExpired("dialog", timeout)
            return b"", b""

        process.communicate.side_effect = communicate
        with (
            patch.dict(os.environ, {"DISPLAY": ":0"}),
            patch.object(nfd.shutil, "which", return_value="/usr/bin/kdialog"),
            patch.object(nfd.subprocess, "Popen", return_value=process),
        ):
            self.assertEqual(nfd._fallback("open", None, (), "Open", cancelled).status, "cancelled")
        process.terminate.assert_called_once()
        process.kill.assert_called_once()


if __name__ == "__main__":
    unittest.main()

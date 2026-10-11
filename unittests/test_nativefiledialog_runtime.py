"""Run with a rebuilt SDK Python. Windows tests open and cancel real OS panels."""

import os
from pathlib import Path
import tempfile
import threading
import time
import types
import unittest
from unittest.mock import patch

import renpy

try:
    if not hasattr(renpy, "display"):
        renpy.import_all()
    from renpy import _nativefiledialog
except ImportError as exc:
    raise unittest.SkipTest("A rebuilt desktop runtime is required: " + str(exc))

from renpy import nativefiledialog as nfd
from renpy import pygame


class NativeRuntimeTests(unittest.TestCase):
    def test_module_registration_and_legacy_api(self):
        self.assertIs(renpy.exports.show_file_dialog, nfd.show_file_dialog)
        for name in ("saveFileDialog", "openFileDialog", "selectFolderDialog"):
            self.assertTrue(callable(getattr(renpy.tfd, name)))

    @unittest.skipUnless(renpy.windows, "Real COM panels require Windows")
    def test_real_com_dialogs_cancel_and_release(self):
        pygame.display.init()
        self.addCleanup(pygame.display.quit)
        pygame.display.set_mode((128, 128), pygame.WINDOW_HIDDEN)
        window = pygame.display.get_window()
        window.show()
        for kind in ("save", "open", "folder"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as root:
                target = os.path.join(root, "猫 100%.png") if kind != "folder" else root
                native = _nativefiledialog.NativeRequest(
                    kind, target, "RenPy API test ' \"", [("Images", ("*.png", "*.jpg", "*.bmp"))], window
                )
                self.assertTrue(native.prepare())
                worker = threading.Thread(target=native.run, daemon=True)
                worker.start()
                deadline = time.monotonic() + 10
                cancel_at = time.monotonic() + 0.5
                while worker.is_alive() and time.monotonic() < deadline:
                    native.pump()
                    pygame.event.pump()
                    if time.monotonic() >= cancel_at:
                        native.cancel()
                    time.sleep(0.01)
                worker.join(0.1)
                self.assertFalse(
                    worker.is_alive(),
                    "The owning STA thread did not close the panel (stage {}, callback {}, worker {}, main {}).".format(
                        native.pending_stage(), native.cancel_thread(), worker.native_id, threading.get_native_id()
                    ),
                )
                status, path, backend, error, attached = native.result()
                self.assertEqual((status, path, backend, error), ("cancelled", None, "windows-com", None))
                self.assertTrue(attached)
                native.release_parent()
                if kind == "save":
                    self.assertFalse(Path(target).exists(), "Opening a save panel test-wrote the file.")

    @unittest.skipUnless(renpy.windows, "Main-thread integration cancellation uses Windows COM")
    def test_public_request_completion_and_late_reset(self):
        pygame.display.init()
        self.addCleanup(pygame.display.quit)
        pygame.display.set_mode((128, 128), pygame.WINDOW_HIDDEN)
        with (
            patch.object(renpy.game, "context", return_value=types.SimpleNamespace(init_phase=False)),
            patch.object(renpy.game, "interface", types.SimpleNamespace(started=True)),
            patch.object(renpy.exports, "predicting", return_value=False),
            patch.object(renpy.exports, "restart_interaction"),
        ):
            for reset in (False, True):
                request = nfd.show_file_dialog("save", os.path.join(tempfile.gettempdir(), "猫.png"))
                self.assertEqual(nfd.show_file_dialog("open").poll().status, "error")
                time.sleep(0.3)
                if reset:
                    nfd.reset("runtime test reset")
                else:
                    request.cancel()
                deadline = time.monotonic() + 5
                while (nfd.active() or nfd._retired) and time.monotonic() < deadline:
                    pygame.event.pump()
                    nfd.update()
                    time.sleep(0.01)
                self.assertFalse(nfd.active())
                self.assertFalse(nfd._retired)
                self.assertEqual(request.poll().status, "cancelled")
                self.assertIsNone(request.poll().path)

    def test_dialogue_auto_forward_and_pause_are_suspended(self):
        from renpy.display.behavior import SayBehavior, PauseBehavior

        event = pygame.event.Event(renpy.display.core.TIMEEVENT)
        with patch.object(nfd, "active", return_value=True):
            say = SayBehavior.__new__(SayBehavior)
            self.assertIsNone(say.event(event, 0, 0, 1000))
            pause = PauseBehavior.__new__(PauseBehavior)
            with patch.object(renpy.game, "interface") as interface:
                self.assertIsNone(pause.event(event, 0, 0, 1000))
                interface.timeout.assert_called_with(0.05)


if __name__ == "__main__":
    unittest.main()

"""Run with a rebuilt SDK Python: -m unittest unittests.test_startup_window."""

import os
import unittest
from unittest.mock import Mock, patch

import renpy

if not hasattr(renpy, "display"):
    renpy.import_all()

from renpy import pygame
from renpy.display.core import Interface
from renpy.gl2.gl2draw import GL2Draw


class StartupWindowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.environment = patch.dict(os.environ, {"SDL_VIDEODRIVER": "dummy"})
        cls.environment.start()
        cls.addClassCleanup(cls.environment.stop)
        pygame.display.init()
        cls.addClassCleanup(pygame.display.quit)

    def setUp(self):
        pygame.display.set_mode((64, 64), pygame.WINDOW_HIDDEN)
        self.addCleanup(pygame.display.destroy)
        self.window = pygame.display.get_window()
        self.interface = Interface.__new__(Interface)
        self.interface.first_frame = True
        self.interface.frame_times = []
        self.interface.force_redraw = False
        self.interface.dpi_scale = 1.0
        self.interface.display_reset = False
        self.interface.started = True
        self.interface.surftree = None
        self.interface.screenshot = None
        self.interface.screenshot_surface = None
        self.renderer = Mock()
        self.renderer.draw_screen.return_value = True

        for p in (
            patch.object(renpy, "mobile", False),
            patch.object(renpy, "android", False),
            patch.object(renpy, "emscripten", False),
            patch.object(renpy.display, "interface", self.interface),
            patch.object(renpy.game, "interface", self.interface),
            patch.object(renpy.display, "draw", self.renderer),
            patch.object(renpy.display.screen, "per_frame"),
            patch.object(renpy.display.render, "render_screen", return_value=object()),
            patch.object(renpy.display.render, "mark_sweep"),
            patch.object(renpy.display.focus, "take_focuses"),
        ):
            p.start()
            self.addCleanup(p.stop)

    def assert_hidden(self):
        self.assertTrue(self.window.get_window_flags() & pygame.WINDOW_HIDDEN)

    def test_hidden_window_can_be_updated_before_show(self):
        self.assert_hidden()
        surface = self.window.get_surface()
        surface.fill((32, 160, 224))
        self.window.flip()
        self.assert_hidden()
        self.window.show()
        self.assertFalse(self.window.get_window_flags() & pygame.WINDOW_HIDDEN)
        self.assertEqual(tuple(surface.get_at((32, 32)))[:3], (32, 160, 224))

    def test_first_successful_presentation_reveals_window(self):
        def present(surftree):
            self.assert_hidden()
            return True

        self.renderer.draw_screen.side_effect = present
        self.interface.draw_screen(None, False, True)
        self.assertFalse(self.window.get_window_flags() & pygame.WINDOW_HIDDEN)
        self.assertFalse(self.interface.first_frame)
        self.assertTrue(self.interface.force_redraw)

    def test_skipped_and_failed_frames_do_not_reveal_window(self):
        self.interface.draw_screen(None, False, False)
        self.renderer.draw_screen.assert_not_called()
        self.assert_hidden()
        self.assertTrue(self.interface.first_frame)

        self.renderer.draw_screen.return_value = False
        self.interface.draw_screen(None, False, True)
        self.assert_hidden()
        self.assertTrue(self.interface.first_frame)

        self.renderer.draw_screen.return_value = True
        self.interface.draw_screen(None, False, True)
        self.assertFalse(self.interface.first_frame)
        self.assertFalse(self.window.get_window_flags() & pygame.WINDOW_HIDDEN)

    def test_first_frame_callback_runs_once(self):
        self.interface.after_first_frame = Mock()
        self.interface.draw_screen(None, False, True)
        self.interface.draw_screen(None, False, True)
        self.interface.after_first_frame.assert_called_once_with()

    def test_gl_swap_failure_is_not_a_presented_frame(self):
        renderer = GL2Draw("gl2")
        with patch.object(pygame.display, "flip", side_effect=pygame.error("test swap failure")):
            with patch.object(renpy.display.log, "write"):
                self.assertFalse(renderer.flip())
        self.assertTrue(self.interface.display_reset)
        with patch.object(pygame.display, "flip"):
            self.assertTrue(renderer.flip())

    def test_screenshots_before_first_render_are_unavailable(self):
        self.renderer.screenshot.side_effect = AssertionError("No render tree exists yet")
        self.interface.take_screenshot((32, 32))
        self.interface.take_screenshot((32, 32), background=True)
        self.assertIsNone(self.interface.screenshot)
        self.assertIsNone(self.interface.screenshot_to_bytes())
        self.assertFalse(self.interface.save_screenshot("unused.png"))
        self.renderer.screenshot.assert_not_called()
        self.assert_hidden()

    def test_screenshot_after_first_render_still_works(self):
        self.renderer.screenshot.return_value = self.window.get_surface()
        self.interface.draw_screen(None, False, True)
        self.interface.take_screenshot((32, 32))
        self.assertTrue(self.interface.screenshot.startswith(b"\x89PNG\r\n\x1a\n"))
        self.renderer.screenshot.assert_called_once_with(self.interface.surftree)


if __name__ == "__main__":
    unittest.main()

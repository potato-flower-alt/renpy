"""Shader queue regressions without requiring a GL context."""
import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch


class ShaderPredictionTests(unittest.TestCase):
    def setUp(self):
        self.renpy = SimpleNamespace(config=SimpleNamespace(
            predict_shaders=True, debug_prediction=False, shader_part_filter=None, default_shader="renpy.geometry"),
            display=SimpleNamespace(draw=None))
        spec = importlib.util.spec_from_file_location("test_shadercache", Path(__file__).parents[1] / "renpy/gl2/gl2shadercache.py")
        self.module = importlib.util.module_from_spec(spec)
        with patch.dict(sys.modules, {"renpy": self.renpy}):
            spec.loader.exec_module(self.module)
        self.cache = self.module.ShaderCache("unused", False)
        self.renpy.display.draw = SimpleNamespace(shader_cache=self.cache)
        self.compiled = []

        def compile_parts(parts):
            normalized = self.cache._normalize_partnames(parts)
            self.compiled.append(normalized)
            self.cache.cache[normalized] = object()

        self.cache._get_filtered = compile_parts

    def test_deduplication_removal_and_single_compile(self):
        self.module.predict_shader(("renpy.texture", "mask", "-renpy.texture"))
        self.module.predict_shader(("mask",))
        self.module.predict_shader(("alpha", "mask"))
        self.assertEqual(len(self.cache.predicted), 2)
        self.assertTrue(self.module.preload_predicted_shader())
        self.assertEqual(self.compiled, [("mask", "renpy.geometry")])
        self.assertEqual(len(self.cache.predicted), 1)
        self.module.predict_shader(("mask",))
        self.assertEqual(len(self.cache.predicted), 1)

    def test_disable_clears_queue_and_clear_allows_context_reload(self):
        self.module.predict_shader("mask")
        self.renpy.config.predict_shaders = False
        self.assertFalse(self.module.has_predicted_shaders())
        self.renpy.config.predict_shaders = True
        self.module.predict_shader("mask")
        self.module.preload_predicted_shader()
        self.cache.clear()
        self.module.predict_shader("mask")
        self.assertTrue(self.module.has_predicted_shaders())

    def test_prediction_errors_follow_debug_setting(self):
        def fail(parts):
            raise RuntimeError("compile failed")
        self.cache._get_filtered = fail
        self.module.predict_shader("mask")
        self.assertTrue(self.module.preload_predicted_shader())
        self.renpy.config.debug_prediction = True
        self.module.predict_shader("mask")
        with self.assertRaises(RuntimeError):
            self.module.preload_predicted_shader()

    def test_filter_and_ftl_are_preserved(self):
        self.renpy.config.shader_part_filter = lambda parts: ("renpy.ftl",)
        self.module.predict_shader(("texture",))
        self.module.preload_predicted_shader()
        self.assertEqual(self.compiled, [("renpy.ftl",)])

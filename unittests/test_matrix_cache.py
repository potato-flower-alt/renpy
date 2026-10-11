"""Run with the built engine's Python to exercise native Matrix caches."""
import pickle
import unittest

try:
    from renpy.display.matrix import Matrix, fields
except ImportError:
    Matrix = None


@unittest.skipIf(Matrix is None, "native Matrix extension not built")
class MatrixCacheTests(unittest.TestCase):
    def assert_fresh(self, matrix):
        fresh = Matrix(None)
        fresh.__setstate__(matrix.__getstate__())
        for operation in ("inverse", "transpose"):
            actual, expected = getattr(matrix, operation)(), getattr(fresh, operation)()
            for field in fields:
                self.assertAlmostEqual(getattr(actual, field), getattr(expected, field), places=5)

    def prime(self, matrix):
        matrix.inverse()
        matrix.transpose()

    def test_public_fields_invalidate_both_caches(self):
        for field in fields:
            matrix = Matrix([2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1])
            self.prime(matrix)
            setattr(matrix, field, getattr(matrix, field) + 0.125)
            self.assert_fresh(matrix)

    def test_take_reinitialization_and_restore(self):
        matrix = Matrix.offset(1, 2, 3)
        self.prime(matrix)
        matrix.take(Matrix.offset(4, 5, 6))
        self.assert_fresh(matrix)
        self.prime(matrix)
        matrix.__init__([3, 0, 0, 2])
        self.assert_fresh(matrix)
        self.prime(matrix)
        matrix.__setstate__(Matrix.offset(7, 8, 9).__getstate__())
        self.assert_fresh(matrix)
        self.assert_fresh(pickle.loads(pickle.dumps(matrix)))

from __future__ import annotations

import os
import subprocess
import unittest
from pathlib import Path


class PointLocationCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        binary_text = os.environ.get("POINT_LOCATION_BIN")
        if binary_text is None:
            raise AssertionError("POINT_LOCATION_BIN must name the C example executable")
        cls.binary = Path(binary_text).resolve()

    def test_machine_mode_rejects_malformed_input(self) -> None:
        completed = subprocess.run(
            [str(self.binary), "--machine"],
            input="2 nope\n",
            text=True,
            capture_output=True,
        )

        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(completed.stdout, "")
        self.assertEqual(
            completed.stderr,
            "invalid input: expected whitespace-separated 'x y' pairs\n",
        )

    def test_rejects_unsupported_arguments(self) -> None:
        completed = subprocess.run(
            [str(self.binary), "--unsupported"],
            text=True,
            capture_output=True,
        )

        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(completed.stdout, "")
        self.assertEqual(
            completed.stderr,
            f"usage: {self.binary} [--machine]\n",
        )


if __name__ == "__main__":
    unittest.main()

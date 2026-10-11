"""Asset-free regression tests for sampled bot movement; no pytest required."""
import runpy
import unittest
from pathlib import Path

check_movement = runpy.run_path(
    str(Path(__file__).with_name("check_bot_navigation_log.py"))
)["check_movement"]


def sample(x: float, behavior: str = "fight") -> str:
    return f"bots: bot 17 at ({x:.1f} 0.0 0.0) yaw 0 {behavior} (conf 0.72)\n"


class MovementLogTests(unittest.TestCase):
    def test_stationary_fighting_bot_fails(self):
        with self.assertRaisesRegex(SystemExit, "bot 17 stayed"):
            check_movement(sample(1) * 3)

    def test_slow_drift_does_not_hide_stall(self):
        with self.assertRaises(SystemExit):
            check_movement(sample(1) + sample(1.1) + sample(1.2))

    def test_moving_bot_passes(self):
        check_movement(sample(1) + sample(2) + sample(3))

    def test_vehicle_resets_stationary_window(self):
        check_movement(sample(1) * 2 + sample(1, "vehicle") * 4 + sample(1) * 2)


if __name__ == "__main__":
    unittest.main()

"""Asset-free regression tests for sampled bot movement; no pytest required."""
import runpy
import unittest
from pathlib import Path

checker = runpy.run_path(str(Path(__file__).with_name("check_bot_navigation_log.py")))
check_movement = checker["check_movement"]
check_movement_coverage = checker["check_movement_coverage"]


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

    def test_vertical_jumping_does_not_hide_horizontal_stall(self):
        text = sample(1) + sample(1).replace("1.0 0.0 0.0", "1.0 0.0 1.0") + sample(1)
        with self.assertRaisesRegex(SystemExit, "bot 17 stayed"):
            check_movement(text)

    def test_respawn_resets_stationary_window(self):
        text = sample(1) * 2 + "bots: bot 17 team 0 cleared item claims (new life)\n" + sample(1) * 2
        check_movement(text)

    def test_crash_cannot_pass_after_movement_samples(self):
        with self.assertRaisesRegex(SystemExit, "crash"):
            check_movement(sample(1) + sample(2) + sample(3) + "Assertion failed: player unit\n")

    def test_insufficient_samples_are_not_a_pass(self):
        with self.assertRaisesRegex(SystemExit, "insufficient"):
            check_movement_coverage(sample(1) + sample(2), 1)

    def test_many_samples_of_one_bot_do_not_cover_two_bots(self):
        with self.assertRaisesRegex(SystemExit, "1/2"):
            check_movement_coverage(sample(1) + sample(2) + sample(3), 2)

    def test_malformed_number_is_a_clear_failure(self):
        with self.assertRaisesRegex(SystemExit, "invalid number"):
            check_movement(sample(1).replace("1.0 0.0 0.0", "1..0 0.0 0.0"))

    def test_vehicle_resets_stationary_window(self):
        check_movement(sample(1) * 2 + sample(1, "vehicle") * 4 + sample(1) * 2)


if __name__ == "__main__":
    unittest.main()

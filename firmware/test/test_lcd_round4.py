"""Wiring checks for sketch paths outside the native firmware test harness."""
from pathlib import Path
import unittest

LCD = Path(__file__).resolve().parents[1] / "lcd43b"


class RoundFour(unittest.TestCase):
    def test_staging_runs_on_worker(self):
        refresh = (LCD / "app_refresh.h").read_text()
        worker = refresh.split("void refreshWorker(void*)", 1)[1].split("bool startRefresh", 1)[0]
        self.assertIn("lcd::stageUpdatePass(", worker)
        self.assertNotIn("stageUpdatePass(", (LCD / "app_update.h").read_text())

    def test_accepted_download_does_not_block_frame_saves(self):
        refresh = (LCD / "app_refresh.h").read_text()
        allowed = refresh.split("bool litFrameSaveAllowed", 1)[1].split("}", 1)[0]
        self.assertNotIn("acceptedUpdate", allowed)
        self.assertRegex(refresh, r"memoryPressure \|\| !refreshBusy \|\| refreshStagingUpdate")
        sketch = (LCD / "lcd43b.ino").read_text()
        self.assertRegex(sketch, r"installQueued == lcd::UpdateSource::None && settingsPage.expired")

    def test_art_progress_resumes_next_pass(self):
        art = (LCD / "local_provider.cpp").read_text()
        self.assertRegex(art, r"(?s)outcome == FetchOutcome::Progress\) \{[^}]*madeProgress = true")
        self.assertIn("state.finishPass(day, uint32_t(time(nullptr)), artBatch.placed == ArtWant, madeProgress)", art)

    def test_partial_store_uses_scan_scope(self):
        sketch = (LCD / "lcd43b.ino").read_text()
        self.assertRegex(sketch, r"(?s)setPartialStoreWriteHook\(.*?scope.emplace\(lcd::ScanActivity::PartialStore\)")

    def test_standalone_sweep_leaves_frame_files(self):
        refresh = (LCD / "app_refresh.h").read_text()
        sweep = refresh.split("void sweepDownloads(", 1)[1].split("void refreshWorker", 1)[0]
        self.assertRegex(sweep, r"const bool sweepFrames = manifest && frameSweepDay\.due\(now\);")
        worker = refresh.split("void refreshWorker(void*)", 1)[1].split("bool startRefresh", 1)[0]
        self.assertIn("refreshStaging.manifestOk && !standalone ? &refreshStaging.manifest : nullptr", worker)


if __name__ == "__main__":
    unittest.main()

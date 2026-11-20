# SPDX-License-Identifier: GPL-3.0-or-later
import json
from pathlib import Path
import subprocess
import sys


SCORER = Path(__file__).resolve().parents[2] / "tools/wifi-bench/score_log.py"


def score(tmp_path, log):
    path = tmp_path / "serial.log"
    path.write_text(log, encoding="utf-8")
    result = subprocess.run([sys.executable, str(SCORER), str(path)],
                            capture_output=True, text=True, check=False)
    assert result.stdout, result.stderr
    return result.returncode, json.loads(result.stdout)


def test_scores_clean_run(tmp_path):
    code, report = score(tmp_path, """\
23:59:55 ventanal lcd43b boot fw=1.1.0 app=ventanal slot=0
23:59:56 standalone frame slot=weather id=w1 bytes=100 deflate_ms=2
23:59:57 standalone frame slot=solar id=s1 bytes=100 deflate_ms=2
23:59:58 cache queued slot=art bytes=100 free_psram=1000
00:00:00 refresh fetch=0 ok=1 offline=0 failed=0
00:00:01 cache queued slot=spare0 bytes=100 free_psram=1000
00:00:02 cache queued slot=spare1 bytes=100 free_psram=1000
00:00:03 cache persist slot=art id=a1 bytes=100 ok=1 ms=10
00:00:04 cache persist slot=weather id=w2 bytes=100 ok=0 ms=10
00:00:06 refresh fetch=1 ok=1 offline=0 failed=0
00:00:08 unrelated serial output
00:00:09 refresh fetch=2 ok=1 offline=0 failed=0
""")
    assert code == 0
    assert report == {
        "passes": 3,
        "completed": {"weather": 1, "solar": 1, "art": 3, "frames": 1},
        "failed_labels": 0,
        "reboots": 0,
        "watchdogs": 0,
        "resumes": 0,
        "resume_bytes_skipped": 0,
        "max_pass_s": 6.0,
    }
    assert isinstance(report["max_pass_s"], float)


def test_failed_label_fails(tmp_path):
    code, report = score(tmp_path, """\
12:00:00 ventanal lcd43b boot fw=1.1.0 app=ventanal slot=0
12:00:09 label=DOWNLOAD FAILED
12:00:10 refresh fetch=0 ok=1 offline=0 failed=1
""")
    assert code == 1
    assert report["failed_labels"] == 1
    assert report["passes"] == 1
    assert report["max_pass_s"] == 10.0


def test_counts_resume(tmp_path):
    code, report = score(tmp_path, """\
12:00:00 resume target=weather from=1234 total=2500
12:00:01 resume done target=weather bytes=2500 ms=500
12:00:02 resume target=solar from=456 total=1000
12:00:03 resume done target=solar bytes=1000 ms=400
""")
    assert code == 0
    assert report["resumes"] == 2
    assert report["resume_bytes_skipped"] == 1690


def test_resume_ignored_range_and_incomplete(tmp_path):
    _, report = score(tmp_path, """\
12:00:00 resume target=frame-art from=100 total=1000
12:00:01 resume done target=frame-art bytes=1000 ms=10
12:00:02 frame slot=art outcome=complete reason=ok got=900 have=1000 total=1000 ms=10
12:00:03 resume target=frame-weather from=200 total=1000
12:00:04 resume done target=frame-weather bytes=1000 ms=10
12:00:05 frame slot=weather outcome=complete reason=ok got=1000 have=1000 total=1000 ms=10
12:00:06 resume target=frame-solar from=300 total=1000
12:00:07 frame slot=solar outcome=no-progress reason=budget got=0 have=300 total=1000 ms=10
""")
    assert report["resumes"] == 1
    assert report["resume_bytes_skipped"] == 100


def test_reboot_and_watchdog_detected(tmp_path):
    code, report = score(tmp_path, """\
12:00:00 ventanal lcd43b boot fw=1.1.0 app=ventanal slot=0
12:00:10 E (123) task_wdt: Task watchdog got triggered
12:00:11 Task watchdog diagnostic
12:00:12 ventanal epaper boot fw=1.1.0
""")
    assert code == 1
    assert report["reboots"] == 1
    assert report["watchdogs"] == 2

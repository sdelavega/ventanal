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
12:00:00 resume target=frame-art from=196608 total=309257
12:00:01 frame slot=art outcome=progress reason=budget got=281780 have=262144 total=309257
12:00:02 resume target=frame-art from=262144 total=309257
12:00:03 resume done target=frame-art bytes=309257 ms=25763
12:00:04 frame slot=art outcome=complete reason=ok got=47113 have=0 total=309257
12:00:05 resume target=frame-spare2 from=98304 total=284118
12:00:06 resume restart target=frame-spare2 reason=whole-body from=98304
12:00:07 resume done target=frame-spare2 bytes=284118 ms=27611
12:00:08 frame slot=spare2 outcome=complete reason=ok got=284118 have=0 total=284118
12:00:09 resume target=ota-1.2.1 from=983040 total=2259776
12:00:10 resume done target=ota-1.2.1 bytes=2259776 ms=25763
12:00:11 resume target=ota-1.2.2 from=983040 total=2259776
12:00:12 resume restart target=ota-1.2.2 reason=whole-body from=983040
12:00:13 resume done target=ota-1.2.2 bytes=2259776 ms=25763
""")
    assert report["resumes"] == 2
    assert report["resume_bytes_skipped"] == 262144 + 983040


def test_restart_invalidates_pending_frame_even_after_done(tmp_path):
    _, report = score(tmp_path, """\
12:00:00 resume target=frame-art from=196608 total=309257
12:00:01 resume done target=frame-art bytes=309257 ms=25763
12:00:02 resume restart target=frame-art reason=whole-body from=196608
12:00:03 frame slot=art outcome=complete reason=ok got=112649 have=0 total=309257
""")
    assert report["resumes"] == 0
    assert report["resume_bytes_skipped"] == 0


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

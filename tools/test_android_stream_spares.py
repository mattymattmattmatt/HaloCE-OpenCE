"""Check that an OpenGL ES frame streaming more than its buffer holds goes
on in spare buffers of its frame of the ring.

Compiles the real stream_reserve, stream_spare_get, stream_upload and
index_upload with a model of buffer storage: no buffer a draw of the frame
read is orphaned (the unsynchronized writes would count on the driver
renaming its storage under the queued draws), none a queued frame may read
is written, and no write goes past a buffer's end.
"""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

from harness import function, mutated

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def spares(tmp_path_factory):
    compiler = shutil.which(os.environ.get("CC", "clang"))
    if not compiler:
        pytest.skip("clang required")
    renderer = (ROOT / "port/linux/src/d3d8_gl.c").read_text()
    pool = renderer[renderer.index("#define STREAM_SPARE_BUFFERS"):
                    renderer.index("static void stream_reserve(unsigned long size)")]
    pool = pool[:pool.rindex("#endif")]
    code = "\n".join([pool, function(renderer, "stream_reserve"),
                      "static unsigned long stream_upload(const void *data, unsigned long size);",
                      function(renderer, "stream_upload"), function(renderer, "index_upload")])
    present = function(renderer, "D3DDevice_Present")
    rotation = present[present.index("device.stream_buffer = device.stream_buffers[device.buffer_ring];"):]
    rotation = rotation[:rotation.index("#endif") + len("#endif")]
    directory = tmp_path_factory.mktemp("stream-spares")
    target = ["--target=i686-pc-windows-msvc", "-fuse-ld=lld"] if os.name == "nt" else ["-m32"]
    binaries = {}
    for variant in ("spares", "orphaning"):
        work = directory / variant
        work.mkdir()
        selected = code if variant == "spares" else mutated(
            code, "if (stream_spares_used[indices] == STREAM_SPARE_BUFFERS)", "if (1)")
        (work / "spares.inc").write_text(selected)
        (work / "rotation.inc").write_text(rotation)
        executable = work / ("test.exe" if os.name == "nt" else "test")
        result = subprocess.run(
            [compiler, *target, "-std=gnu89", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-O2", "-I", str(work), str(ROOT / "port/android/tests/stream_spares_test.c"), "-o", str(executable)],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        binaries[variant] = executable
    return binaries


@pytest.mark.parametrize("volume", ["4", "40", "100"])
def test_stream_spares(spares, volume):
    result = subprocess.run([str(spares["spares"]), volume], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "orphans_read=0 writes_queued=0 writes_outside=0" in result.stderr, result.stderr


def test_orphaning_control(spares):
    """Negative control: orphaning the frame's buffer when full is caught."""
    result = subprocess.run([str(spares["orphaning"]), "40"], capture_output=True, text=True, timeout=30)
    assert result.returncode == 1, result.stdout + result.stderr
    assert "orphans_read=0 " not in result.stderr, result.stderr

"""Tests of how configure.py generates the PS5 build (tools/ps5_build.py):
it downloads nothing unless asked for, a checkout that is not there makes the
ps5 target stop with the reason instead of stopping configure.py, a
checkout with what the generator checks generates the title, and one that is
not at its pin is a warning."""
import io
import shutil
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools import ps5_build  # noqa: E402
from tools.ninja_syntax import Writer  # noqa: E402


@pytest.fixture
def tree(tmp_path, monkeypatch):
    """a folder that configure.py runs in, with the build's settings, the
    host's folder and the system's OpenGL headers; fetches are recorded"""
    (tmp_path / "port" / "linux" / "host").mkdir(parents=True)
    shutil.copy(ROOT / "port" / "linux" / "port.json", tmp_path / "port" / "linux" / "port.json")
    (tmp_path / "port" / "ps5" / "host").mkdir(parents=True)
    for name in ("host_main.c", "host_syscall.c"):
        (tmp_path / "port" / "ps5" / "host" / name).write_text("")
    headers = tmp_path / "gl"
    for name in ps5_build.GL_HEADER_NAMES:
        (headers / name).parent.mkdir(parents=True, exist_ok=True)
        (headers / name).write_text("")
    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(ps5_build, "GL_HEADERS", headers)
    fetches = []
    monkeypatch.setattr(ps5_build, "fetch_third_party", lambda third_party: fetches.append(third_party))

    def fetch_glslang():
        fetches.append("glslang")
        return tmp_path / "glslang"

    monkeypatch.setattr(ps5_build, "fetch_glslang", fetch_glslang)

    def resolve_pacbrew(vulkan):
        fetches.append("pacbrew")
        return {"cflags": ["-I/pacbrew/include", "-I/pacbrew/include/SDL2", "-D_REENTRANT"],
                "libs": ["-L/pacbrew/lib", "-lSDL2", "-liconv"]}

    monkeypatch.setattr(ps5_build, "resolve_pacbrew", resolve_pacbrew)
    return fetches


@pytest.fixture
def repository(tree, monkeypatch):
    """the generator run from the repository, whose sources the guest image's
    rules list (nothing is written there: the fetches are recorded)"""
    monkeypatch.chdir(ROOT)
    return tree


def fake_checkout(root: Path, sdk_revision: str = ps5_build.PS5_PAYLOAD_SDK_REVISION) -> Path:
    """a PS5_Vulkan checkout, and PS5_Mesa beside it, holding the files the
    generator checks; an llvm-config that answers its clang's folder"""
    vulkan = root / "PS5_Vulkan"
    files = [*ps5_build.VULKAN_FILES, *(source for _, source in ps5_build.AGC_STUBS),
             ".deps/native/radv-release/lib/libvulkan_radeon.ps5.a", ".deps/native/zlib/root/usr/lib/libz.a",
             ".deps/native/ps5-payload-sdk/toolchain/prospero.cmake"]
    for name in files:
        (vulkan / name).parent.mkdir(parents=True, exist_ok=True)
        (vulkan / name).write_text("")
    (vulkan / "tools" / "setup-pacbrew-dependencies.sh").write_text(
        f"version={ps5_build.PACBREW_VERSION}\nhash={ps5_build.PACBREW_SHA256}\n")
    (vulkan / ".deps" / "native" / "ps5-payload-sdk" / ".ps5-sdk-revision").write_text(sdk_revision + "\n")
    mesa = root / "PS5_Mesa" / "include" / "vulkan" / "vulkan.h"
    mesa.parent.mkdir(parents=True)
    mesa.write_text("")
    llvm = root / "llvm"
    (llvm / "bin").mkdir(parents=True)
    (llvm / "bin" / "clang").write_text("")
    (llvm / "llvm-config").write_text(f"#!/bin/sh\necho {llvm / 'bin'}\n")
    (llvm / "llvm-config").chmod(0o755)
    (root / "hostdeps.sh").write_text("")
    return vulkan


def settings(root: Path, vulkan: Path, **more):
    return SimpleNamespace(ps5=True, ps5_vulkan=str(vulkan), ps5_llvm_config=str(root / "llvm" / "llvm-config"),
                           ps5_hostdeps=str(root / "hostdeps.sh"), ps5_guest_cc="clang", **more)


def generate(capsys, sln):
    output = io.StringIO()
    ps5_build.generate_ps5_build(Writer(output), sln)
    return output.getvalue(), capsys.readouterr().err


def builds(text: str):
    return [line for line in text.splitlines() if line.startswith("build ")]


def test_not_asked_for_downloads_nothing(tree, capsys):
    text, err = generate(capsys, SimpleNamespace())
    assert tree == []
    assert builds(text) == ["build ps5: ps5_unavailable"] and "--ps5" in text
    # (quietly: every configure.py gets here)
    assert err == ""


def test_fetched_sources_are_asked_for(tree, tmp_path):
    third_party = tmp_path / "third_party"
    assert not ps5_build.requested(SimpleNamespace(), third_party)
    (third_party / f"musl-{ps5_build.MUSL_VERSION}").mkdir(parents=True)
    (third_party / "SDL3").mkdir()
    assert ps5_build.requested(SimpleNamespace(), third_party)
    assert ps5_build.requested(SimpleNamespace(ps5_vulkan="/somewhere"), tmp_path / "none")


def test_checkout_that_is_not_there_is_this_target_unavailable(tree, capsys):
    text, err = generate(capsys, SimpleNamespace(ps5=True, ps5_vulkan="/nonexistent/PS5_Vulkan"))
    assert "(ps5) unavailable" in err and "/nonexistent/PS5_Vulkan" in err
    # (only the stopping target: nothing of the build written halfway)
    assert builds(text) == ["build ps5: ps5_unavailable"], text
    assert tree == []


def test_checkout_generates_the_title(repository, tmp_path, capsys):
    vulkan = fake_checkout(tmp_path)
    text, err = generate(capsys, settings(tmp_path, vulkan))
    assert repository == [ps5_build.THIRD_PARTY, "glslang", "pacbrew"]
    assert "unavailable" not in text, err
    title = ROOT / "build" / "ps5" / "title" / ps5_build.TITLE_ID
    for name in ("build/ps5/halo_guest_vk.elf", "host_main.c.o", "host_syscall.c.o", "host_arch.c.o", "posix_files.c.o",
                 "host_import_stubs.c.o", "--wrap=mmap",
                 "rule ps5_link", "build/ps5/host/eboot.elf", "libglslang.a", "-lSDL2",
                 f"{title}/eboot.bin", f"{title}/sce_sys/param.json", f"{title}/sce_sys/icon0.png",
                 f"{title}/sce_module/libc.prx", f"{title}/halo_guest.elf", f"{title}/NOTICE.txt"):
        assert name in text, name
    assert any(line.startswith("build ps5: phony") for line in builds(text))
    # (the guest of the title is the Vulkan image, with the window movable,
    # the platform layer's helpers of the Vulkan renderer, and its imports)
    assert "-DHALO_MOVABLE_WINDOW=1" in text and "-DHALO_PS5=1" in text
    assert "port/android/host_imports_vk.list" in text


def test_checkout_off_its_pin_is_a_warning(repository, tmp_path, capsys):
    vulkan = fake_checkout(tmp_path, sdk_revision="0" * 40)
    subprocess.run(["git", "init", "-q", str(vulkan)], check=True)
    subprocess.run(["git", "-C", str(vulkan), "-c", "user.name=test", "-c", "user.email=test@example.com",
                    "commit", "-q", "--allow-empty", "-m", "not the pin"], check=True)
    text, err = generate(capsys, settings(tmp_path, vulkan))
    assert "warning: PS5_Vulkan" in err and ps5_build.PS5_VULKAN_COMMIT in err
    assert "warning: the SDK fork" in err
    assert any(line.startswith("build ps5: phony") for line in builds(text))


def test_notice_names_every_pin():
    text = ps5_build.notice_text("0123456789abcdef0123456789abcdef01234567")
    for pin in (ps5_build.PS5_VULKAN_COMMIT, ps5_build.PS5_PAYLOAD_SDK_REVISION, ps5_build.PS5_MESA_COMMIT,
                ps5_build.PACBREW_VERSION, ps5_build.PACBREW_SHA256, ps5_build.GLSLANG_TAG,
                ps5_build.PS5_SDK_RELEASE, ps5_build.PS5_SDK_RELEASE_SHA256, ps5_build.REPOSITORY_URL,
                "0123456789abcdef0123456789abcdef01234567", "GPL-3.0-or-later"):
        assert pin in text


def test_release_licenses_are_named_once():
    # every licence text the release carries has a name of its own, and the
    # ones in this repository are there
    names = [name for _, _, name, _, _ in ps5_build.RELEASE_LICENSES]
    assert len(names) == len(set(names)) and "COPYING" in names
    for root, path, _, _, _ in ps5_build.RELEASE_LICENSES:
        if root == "repo":
            assert (ROOT / path).is_file(), path


def test_license_excerpt_is_the_head_comment():
    text = "/*\n * Licence line one\n *\n * line two\n */\nint code;\n/* not this */\n"
    assert ps5_build.license_excerpt(text) == "Licence line one\n\nline two\n"


def test_content_version_is_the_build_number():
    assert ps5_build.content_version(110) == "01.000.110"
    assert ps5_build.content_version(1001) == "01.001.001"


def test_a_numbered_build_wants_its_content_version(tmp_path):
    param = tmp_path / "param.json"
    param.write_text('{"titleId": "PPSA99501", "contentVersion": "01.000.111"}')
    ps5_build.check_content_version(param, "111")
    ps5_build.check_content_version(param, "0")
    ps5_build.check_content_version(param, "")
    with pytest.raises(RuntimeError, match="01.000.112"):
        ps5_build.check_content_version(param, "112")


def test_the_committed_param_json_is_well_formed():
    import json
    import re

    param = json.loads((ps5_build.PORT_DIR / "sce_sys" / "param.json").read_text())
    assert param["titleId"] == "PPSA99501"
    assert re.fullmatch(r"\d{2}\.\d{3}\.\d{3}", param["contentVersion"])


"""Checks the shared shader keys at the repository's root, which the Switch's
updater downloads from main (port/switch/host/host_update.c): keys.sha1 must
be keys.dkk's SHA-1 (a console whose download did not match would fetch it
again at every start), and keys.dkk a whole key file of this build, by the
header and record size port/switch/guest/dk_shaders.h gives. Skipped while
there is no keys.dkk."""
import hashlib
import re
import struct
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
KEYS = ROOT / "keys.dkk"
SHA1 = ROOT / "keys.sha1"
HEADER = ROOT / "port" / "switch" / "guest" / "dk_shaders.h"


def define(name):
    match = re.search(r"#define %s (0x[0-9a-fA-F]+|\d+)" % name, HEADER.read_text())
    assert match, "%s is not in %s" % (name, HEADER)
    return int(match.group(1).rstrip("UL"), 0)


pytestmark = pytest.mark.skipif(not KEYS.exists(), reason="no keys.dkk at the repository's root")


def test_sha1_matches():
    assert SHA1.exists(), "keys.dkk has no keys.sha1 beside it (sha1sum keys.dkk > keys.sha1)"
    written = SHA1.read_text().split()[0].lower() if SHA1.read_text().split() else ""
    assert re.fullmatch(r"[0-9a-f]{40}", written), "keys.sha1 does not start with a SHA-1"
    assert written == hashlib.sha1(KEYS.read_bytes()).hexdigest(), \
        "keys.sha1 is not keys.dkk's SHA-1 (sha1sum keys.dkk > keys.sha1)"


def test_key_file_of_this_build():
    data = KEYS.read_bytes()
    record = define("DK_KEY_RECORD_SIZE")
    assert len(data) >= 12, "keys.dkk has no header"
    magic, version, size = struct.unpack("<III", data[:12])
    assert magic == define("DK_KEY_MAGIC"), "keys.dkk is not a key file"
    assert version == define("DK_KEY_FORMAT"), "keys.dkk is format %d, this build reads %d" % (
        version, define("DK_KEY_FORMAT"))
    assert size == record, "keys.dkk's records are %d bytes, this build's %d" % (size, record)
    assert (len(data) - 12) % record == 0, "keys.dkk ends inside a record"
    for offset in range(12, len(data), record):
        assert data[offset] in (0, 1), "the record at %d has no stage" % offset

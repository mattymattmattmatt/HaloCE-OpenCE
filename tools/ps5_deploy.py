#!/usr/bin/env python3
"""Install, start, follow and fetch the PS5 title (port/ps5/README.md) from the PC.

The console (port/ps5/PS5.md, "Decisions") runs an FTP server (ftpsrv, port
2121), a kernel log server (klogsrv, port 3232) and, once it is loaded after
each boot, PS5_Vulkan's resident ps5vkctl payload (port 9111), which starts
and closes titles: one command a connection, answered with one line. The
title is installed in /data/homebrew/<TITLE_ID>/, where ShadowMountPlus
lists it. The protocols are PS5_Vulkan's (its tools/ps5_console.py and
docs/DEPLOYMENT.md), written again here.

Usage: python3 tools/ps5_deploy.py [options] ACTION...   (in the order given)
       python3 tools/ps5_deploy.py --run [--hold] [--timeout S] [--minutes N]

Actions:
  fetch          the title's log (halo.log in /data/halo, or in the title's
                 folder; both when both are there), the game's own log
                 (/data/halo/debug.txt, which the game appends to) and the
                 *.txt files of the title's folder, into
                 build/ps5/logs/<UTC time>/; and the screenshots
                 (/data/halo/screenshots/*.ppm, HALO_PS5_SCREENSHOT),
                 converted to PNG there and deleted from the console
  upload         the built title folder (build/ps5/title/<ID>), eboot.bin and
                 sce_sys/param.json last, each file's size checked after it;
                 refused while the title runs, unless --force. --hold puts
                 hold.txt in the title's folder (the host then waits 120 s
                 before it ends, so it can be killed); without it a hold.txt
                 there is removed. --env NAME=VALUE (again for each) writes
                 environment.txt there, the host's environment; without it an
                 environment.txt there is removed
  status, launch, kill, restart
                 one ps5vkctl command each; a launch of the title while it
                 runs is refused, and kill and restart name only this title
  follow         the kernel log: the backlog the server replays is skipped,
                 each line it doubles is shown once, and the host's lines
                 ("halo: "), RADV's ("[radv] ") and any fatal signal are printed
                 until the host's last line, a fatal signal, the title no
                 longer running (ps5vkctl's status, asked every 2 s once the
                 host's lines have begun) or --timeout (180 s); the whole
                 capture is saved
  load-ps5vkctl  PS5_Vulkan's build/ps5vkctl/ps5vkctl.elf sent to the user's
                 payload manager (itsPLK's ps5-payload-manager, port 8084):
                 an upload, then a load; then ping. Only when ps5vkctl does
                 not answer, and the manager is up

--run is kill if the title runs, upload, launch with follow started before
it, then fetch, into a folder of its own made at the launch: nothing of an
earlier or later run is written into it (debug.txt is fetched from where
it stood at the launch). What an earlier run left on the console and no
run fetched (screenshots, a halo.log no folder has) goes first into a
folder of its own, <UTC time>-leftover. Its exit status is 0 only if the
host's last line was seen, or with --minutes if the title ran that long.

--minutes N (with --run) is a long run: it follows for N minutes from the
launch (no --timeout), fetching the screenshots into the run's folder every
5 minutes as it goes, then kills the title and fetches.
"""

import argparse
import datetime
import ftplib
import http.client
import io
import socket
import sys
import threading
import time
import urllib.parse
from pathlib import Path
from typing import Callable, List, Optional

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_HOST = "192.168.2.157"
DEFAULT_TITLE = "PPSA99501"
DATA_FOLDER = "/data/halo-gore"
DEFAULT_PS5VKCTL = Path.home() / "projects" / "ps5-deps" / "PS5_Vulkan" / "build" / "ps5vkctl" / "ps5vkctl.elf"
# the files that go last, in this order, so the title never shows half uploaded
LAST = ("eboot.bin", "sce_sys/param.json")
# the SELF files of a title (eboot.bin, fake-signed with ps5-native-tool's
# magic, and libc.prx, a PS5 SELF): the server decrypts them when they are
# read, so what it says of their size is not the signed file's
# (PS5_FINDINGS.md)
SELF_MAGICS = (bytes.fromhex("4f153d1d"), bytes.fromhex("5414f5ee"))
# klogsrv replays its buffer on connect: what arrives until it is quiet this
# long (and at most this long in all) is that backlog
BACKLOG_QUIET_SECONDS = 0.75
BACKLOG_LIMIT_SECONDS = 5.0
# the host's last lines (port/ps5/host/host_main.c): the end, or the hold
# before it
HOST_LAST_LINES = ("halo: the host ends", "halo: holding for ")
FATAL_SIGNAL = "# A user thread receives a fatal signal"
# RADV's standard error, which the host sends to the kernel log with this
# prefix (port/ps5/host/host_main.c)
RADV_PREFIX = "[radv] "
SCREENSHOTS = f"{DATA_FOLDER}/screenshots"
# the game's own log, which it appends to at every start (source/cseries/errors.c)
GAME_LOG = "debug.txt"
# how often the follower asks ps5vkctl whether the title still runs, once
# the host's lines have begun; and how often a long run fetches the
# screenshots
STATUS_SECONDS = 2.0
SCREENSHOT_FETCH_SECONDS = 300.0


class DeployError(Exception):
    pass


def say(text: str) -> None:
    print(text, flush=True)


# ---------- ps5vkctl

def ps5vkctl(args: argparse.Namespace, command: str, timeout: float = 60.0) -> str:
    """one command, one connection, one line back"""
    try:
        with socket.create_connection((args.host, args.ctl_port), timeout=timeout) as connection:
            with connection.makefile("rwb", buffering=0) as stream:
                stream.write(f"{command}\n".encode("ascii"))
                reply = stream.readline()
    except OSError as error:
        raise DeployError(f"ps5vkctl at {args.host}:{args.ctl_port} did not answer ({error}); load it once the "
                          "payload manager is up: tools/ps5_deploy.py load-ps5vkctl") from error
    return reply.decode("ascii", "replace").strip()


def title_running(args: argparse.Namespace) -> bool:
    reply = ps5vkctl(args, "status")
    return reply.startswith("ok running") and f"title={args.title}" in reply.split()


def action_status(args: argparse.Namespace) -> int:
    say(f"ps5vkctl status: {ps5vkctl(args, 'status')}")
    return 0


def action_launch(args: argparse.Namespace) -> int:
    if title_running(args):
        raise DeployError(f"{args.title} is running already: kill it first (or --run, which does)")
    reply = ps5vkctl(args, f"launch {args.title}")
    say(f"ps5vkctl launch {args.title}: {reply}")
    return 0 if reply.startswith("ok launched") else 1


def action_kill(args: argparse.Namespace) -> int:
    reply = ps5vkctl(args, f"kill {args.title}")
    say(f"ps5vkctl kill {args.title}: {reply}")
    return 0 if reply.startswith("ok") else 1


def action_restart(args: argparse.Namespace) -> int:
    reply = ps5vkctl(args, f"restart {args.title}")
    say(f"ps5vkctl restart {args.title}: {reply}")
    return 0 if reply.startswith("ok") else 1


def action_load_ps5vkctl(args: argparse.Namespace) -> int:
    """the payload manager's two requests (ps5-payload-manager 0.5.2's
    http_server.c): POST /manage:upload?filename=<name> with the ELF as the
    body, which stores it in its payload folder, then GET /loadpayload:<name>,
    which hands it to the console's ELF loader (port 9021, local only)"""
    payload = Path(args.ps5vkctl)
    if not payload.is_file():
        raise DeployError(f"no {payload}: build it with PS5_PAYLOAD_SDK=<a payload SDK> bash "
                          "<PS5_Vulkan>/tools/build-ps5vkctl.sh")
    name = payload.name
    requests = [("POST", f"/manage:upload?filename={urllib.parse.quote(name)}", payload.read_bytes()),
                ("GET", f"/loadpayload:{urllib.parse.quote(name)}", None)]
    for method, path, body in requests:
        say(f"payload manager {args.host}:{args.manager_port}: {method} {path}"
            + (f" ({len(body)} bytes)" if body else ""))
        try:
            connection = http.client.HTTPConnection(args.host, args.manager_port, timeout=60)
            headers = {"Content-Type": "application/octet-stream"} if body else {}
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            text = response.read().decode("utf-8", "replace").strip()
            connection.close()
        except OSError as error:
            raise DeployError(f"the payload manager at {args.host}:{args.manager_port} did not answer "
                              f"({error})") from error
        say(f"  {response.status} {response.reason}: {text}")
        if response.status != 200:
            return 1
    for _ in range(20):
        time.sleep(0.5)
        try:
            say(f"ps5vkctl ping: {ps5vkctl(args, 'ping', timeout=5)}")
            return 0
        except DeployError:
            continue
    raise DeployError(f"ps5vkctl did not answer on {args.host}:{args.ctl_port} after the load")


# ---------- FTP

def ftp_open(args: argparse.Namespace) -> ftplib.FTP:
    try:
        ftp = ftplib.FTP()
        ftp.connect(args.host, args.ftp_port, timeout=30)
        ftp.login("anonymous", "anonymous@")
    except (OSError, ftplib.Error) as error:
        raise DeployError(f"the console's FTP server at {args.host}:{args.ftp_port} did not answer ({error})") \
            from error
    return ftp


def ftp_close(ftp: ftplib.FTP) -> None:
    try:
        ftp.quit()
    except (OSError, EOFError, ftplib.Error):
        pass


def ftp_delete(ftp: ftplib.FTP, name: str) -> bool:
    """DELE, for a server that answers it with 226 (ftpsrv), which ftplib's
    delete() refuses; False if there was no such file"""
    try:
        ftp.sendcmd(f"DELE {name}")
        return True
    except ftplib.error_perm as error:
        if str(error).startswith("550"):
            return False
        raise


def ftp_files(ftp: ftplib.FTP, folder: str) -> Optional[List[str]]:
    """the names of the files in a folder (None if there is no folder);
    ftpsrv ignores MLSD's argument, so the listing changes folder first"""
    try:
        ftp.cwd(folder)
    except ftplib.error_perm:
        return None
    return sorted(name for name, facts in ftp.mlsd() if facts.get("type") == "file")


def title_folder(args: argparse.Namespace) -> str:
    return f"/data/homebrew/{args.title}"


def new_log_folder(args: argparse.Namespace, label: str = "") -> Path:
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S")
    stamp += f"-{label}" if label else ""
    folder = Path(args.logs) / stamp
    suffix = 1
    while folder.exists():
        suffix += 1
        folder = Path(args.logs) / f"{stamp}-{suffix}"
    folder.mkdir(parents=True)
    return folder


def ftp_size(ftp: ftplib.FTP, folder: str, name: str) -> Optional[int]:
    """a file's size, None if there is no such file"""
    try:
        ftp.cwd(folder)
        return ftp.size(name)
    except ftplib.error_perm:
        return None


def ftp_read(ftp: ftplib.FTP, folder: str, name: str, offset: int = 0) -> bytes:
    """a file from offset on: REST when the server takes it, else the whole
    file read and its start dropped"""
    data = io.BytesIO()
    ftp.cwd(folder)
    try:
        ftp.retrbinary(f"RETR {name}", data.write, rest=offset if offset else None)
        return data.getvalue()
    except (ftplib.error_perm, ftplib.error_reply):
        if not offset:
            raise
    data = io.BytesIO()
    ftp.retrbinary(f"RETR {name}", data.write)
    return data.getvalue()[offset:]


def game_log_size(args: argparse.Namespace) -> int:
    """debug.txt's size now (0 if there is none): where a run's part of it
    starts"""
    ftp = ftp_open(args)
    try:
        return ftp_size(ftp, DATA_FOLDER, GAME_LOG) or 0
    finally:
        ftp_close(ftp)


def action_fetch(args: argparse.Namespace, folder: Optional[Path] = None, game_log_from: int = 0) -> int:
    """the logs and screenshots into folder (a new one if None); debug.txt
    from game_log_from on, the part a run added"""
    folder = folder or new_log_folder(args)
    ftp = ftp_open(args)
    fetched = []
    try:
        wanted = []
        title_files = ftp_files(ftp, title_folder(args)) or []
        if "halo.log" in title_files:
            wanted.append((title_folder(args), "halo.log", "app0-halo.log"))
        # (a run's reports; not the title's own files, such as NOTICE.txt and
        # brokers.txt, nor the deploy's hold.txt and environment.txt)
        built = {path.name for path in Path(args.title_dir).glob("*.txt")}
        wanted += [(title_folder(args), name, name) for name in title_files
                   if name.endswith(".txt") and name not in built | {"NOTICE.txt", "hold.txt", "environment.txt"}]
        data_files = ftp_files(ftp, DATA_FOLDER) or []
        if "halo.log" in data_files:
            wanted.append((DATA_FOLDER, "halo.log", "halo.log"))
        for remote_folder, name, local in wanted:
            (folder / local).write_bytes(ftp_read(ftp, remote_folder, name))
            fetched.append(f"{remote_folder}/{name}")
        if GAME_LOG in data_files:
            part = ftp_read(ftp, DATA_FOLDER, GAME_LOG, game_log_from)
            (folder / GAME_LOG).write_bytes(part)
            fetched.append(f"{DATA_FOLDER}/{GAME_LOG}" + (f" (from byte {game_log_from}: {len(part)} bytes)"
                                                         if game_log_from else ""))
        shots = fetch_screenshots(ftp, folder)
    finally:
        ftp_close(ftp)
    say(f"fetched {', '.join(fetched) if fetched else 'nothing (no log on the console)'} into {folder}")
    if shots:
        say(f"fetched {len(shots)} screenshots: {', '.join(shots)}")
    return 0


def fetch_leftovers(args: argparse.Namespace) -> None:
    """what an earlier run left on the console and no run's folder has: the
    screenshots, and halo.log when the newest folder's copy differs (a run
    stopped from the console, or killed before its fetch); into a folder
    <UTC time>-leftover, made only when there is something"""
    ftp = ftp_open(args)
    try:
        shots = [name for name in ftp_files(ftp, SCREENSHOTS) or [] if name.endswith(".ppm")]
        log = ftp_read(ftp, DATA_FOLDER, "halo.log") if "halo.log" in (ftp_files(ftp, DATA_FOLDER) or []) else b""
        folders = sorted(path for path in Path(args.logs).glob("2*") if path.is_dir()) \
            if Path(args.logs).is_dir() else []
        newest = next((path / "halo.log" for path in reversed(folders) if (path / "halo.log").is_file()), None)
        log_fetched = not log or (newest is not None and newest.read_bytes() == log)
        if not shots and log_fetched:
            return
        folder = new_log_folder(args, "leftover")
        if not log_fetched:
            (folder / "halo.log").write_bytes(log)
        saved = fetch_screenshots(ftp, folder)
    finally:
        ftp_close(ftp)
    say(f"leftovers of an earlier run into {folder}: " + ", ".join(
        (["halo.log"] if not log_fetched else []) + ([f"{len(saved)} screenshots"] if saved else [])))


def fetch_screenshots(ftp: ftplib.FTP, folder: Path) -> List[str]:
    """the host's screenshots (binary PPM, port/ps5/host/host_vk_screenshot_ps5.c),
    each saved as a PNG of the same name and then deleted from the console;
    the PNGs' names"""
    from PIL import Image  # (only when there are screenshots to convert)

    names = [name for name in ftp_files(ftp, SCREENSHOTS) or [] if name.endswith(".ppm")]
    saved = []
    for name in names:
        data = io.BytesIO()
        ftp.cwd(SCREENSHOTS)
        ftp.retrbinary(f"RETR {name}", data.write)
        data.seek(0)
        local = folder / (Path(name).stem + ".png")
        try:
            with Image.open(data) as image:
                image.save(local)
        except (OSError, ValueError) as error:
            say(f"{SCREENSHOTS}/{name}: not a picture ({error}); kept as it is")
            local = folder / name
            local.write_bytes(data.getvalue())
        saved.append(local.name)
        ftp_delete(ftp, name)
    return saved


def action_upload(args: argparse.Namespace) -> int:
    source = Path(args.title_dir)
    if not (source / "eboot.bin").is_file() or not (source / "sce_sys" / "param.json").is_file():
        raise DeployError(f"{source} is not a built title: ninja ps5 first")
    if not args.force and title_running(args):
        raise DeployError(f"{args.title} is running: kill it first, or --force")
    files = sorted(path.relative_to(source).as_posix() for path in source.rglob("*") if path.is_file())
    files = [name for name in files if name not in LAST] + [name for name in LAST if name in files]
    unsigned = ROOT / "build" / "ps5" / "host" / "eboot.title.elf"
    started = time.monotonic()
    total = 0
    ftp = ftp_open(args)
    try:
        for folder in sorted({str(Path(name).parent) for name in files if "/" in name}):
            path = title_folder(args)
            for part in [""] + folder.split("/"):
                path = f"{path}/{part}" if part else path
                try:
                    ftp.mkd(path)
                except ftplib.error_perm as error:
                    if not str(error).startswith("550"):
                        raise
        for name in files:
            local = source / name
            remote_folder, _, remote_name = f"{title_folder(args)}/{name}".rpartition("/")
            ftp.cwd(remote_folder)
            size = local.stat().st_size
            say(f"{local.relative_to(ROOT) if local.is_relative_to(ROOT) else local} -> {remote_folder}/{remote_name} "
                f"({size / 1048576.0:.1f} MB)")
            # (straight over the old file, as tools/switch_deploy.py does, and
            # for its reasons; a transfer cut short shows in the size)
            with local.open("rb") as handle:
                ftp.storbinary(f"STOR {remote_name}", handle)
            total += size
            actual = ftp.size(remote_name)
            with local.open("rb") as handle:
                is_self = handle.read(4) in SELF_MAGICS
            if actual == size:
                continue
            if is_self and name == "eboot.bin" and unsigned.is_file() and actual == unsigned.stat().st_size:
                say(f"  (the server gives the decrypted size, {actual} bytes: the unsigned ELF's)")
            elif is_self:
                say(f"  (a SELF: the server gives {actual} bytes, its decrypted size, for {size})")
            else:
                raise DeployError(f"{remote_name} is {actual} bytes on the console, {size} expected: "
                                  "the transfer was cut short")
        ftp.cwd(title_folder(args))
        if args.hold:
            ftp.storbinary("STOR hold.txt", io.BytesIO(b"the host waits 120 s before it ends\n"))
            say(f"{title_folder(args)}/hold.txt: put (the host will wait 120 s before it ends)")
        elif ftp_delete(ftp, "hold.txt"):
            say(f"{title_folder(args)}/hold.txt: removed")
        if args.env:
            text = "".join(f"{setting}\n" for setting in args.env)
            ftp.storbinary("STOR environment.txt", io.BytesIO(text.encode("utf-8")))
            say(f"{title_folder(args)}/environment.txt: {' '.join(args.env)}")
        elif ftp_delete(ftp, "environment.txt"):
            say(f"{title_folder(args)}/environment.txt: removed")
    finally:
        ftp_close(ftp)
    elapsed = time.monotonic() - started
    say(f"uploaded {len(files)} files, {total / 1048576.0:.1f} MB, in {elapsed:.1f} s")
    return 0


# ---------- the kernel log

def klog_lines(connection: socket.socket, args: argparse.Namespace, until: float,
               idle: Optional[float] = None, ticks: bool = False):
    """the kernel log's lines as they come, until the monotonic time until;
    with idle, also as soon as the server has said nothing for that long (the
    way the replayed backlog is told from the run); with ticks, None after
    each second without a line, so the caller can look at other things. A
    connection the server closes ends the lines."""
    rest = b""
    connection.settimeout(idle if idle is not None else 1.0)
    while time.monotonic() < until:
        try:
            data = connection.recv(65536)
        except socket.timeout:
            if idle is not None:
                return
            if ticks:
                yield None
            continue
        if not data:
            raise DeployError(f"klogsrv at {args.host}:{args.klog_port} closed the connection")
        rest += data
        cut = rest.find(b"\n")
        while cut >= 0:
            yield rest[:cut].decode("utf-8", "replace").rstrip("\r")
            rest = rest[cut + 1:]
            cut = rest.find(b"\n")


# follow's results
FOLLOW_LAST_LINE, FOLLOW_SIGNAL, FOLLOW_TIMEOUT, FOLLOW_GONE, FOLLOW_MINUTES = 0, 1, 2, 3, 4


def action_follow(args: argparse.Namespace, folder: Optional[Path] = None,
                  on_ready: Optional[Callable[[], None]] = None, minutes: float = 0.0) -> int:
    """FOLLOW_LAST_LINE when the host's last line arrives, FOLLOW_SIGNAL on a
    fatal signal, FOLLOW_GONE when ps5vkctl says the title no longer runs,
    FOLLOW_TIMEOUT at --timeout; with minutes, FOLLOW_MINUTES when that long
    has passed since on_ready (the title still running) instead of the
    timeout"""
    folder = folder or new_log_folder(args)
    capture = folder / "klog.txt"
    try:
        connection = socket.create_connection((args.host, args.klog_port), timeout=10)
    except OSError as error:
        raise DeployError(f"klogsrv at {args.host}:{args.klog_port} did not answer ({error})") from error
    status = FOLLOW_TIMEOUT
    with connection, capture.open("w", encoding="utf-8", newline="\n") as log_file:
        # the server replays its buffer to a new connection: that is what
        # arrives before it first falls quiet (phase 0 B measured the replay in
        # well under a second)
        backlog = 0
        for line in klog_lines(connection, args, time.monotonic() + BACKLOG_LIMIT_SECONDS, idle=BACKLOG_QUIET_SECONDS):
            log_file.write(line + "\n")
            backlog += 1
        log_file.write("---- the backlog ends; the run follows ----\n")
        duration = minutes * 60.0 if minutes else args.timeout
        say(f"klog {args.host}:{args.klog_port}: {backlog} backlog lines skipped; following for {duration:.0f} s")
        if on_ready:
            on_ready()
        until = time.monotonic() + duration
        next_status = 0.0

        # the server has been seen to send each line twice (phase 0 B): a line
        # is shown when it first comes, and one later copy of it is let pass
        copies_owed: dict = {}
        seen_host = False
        in_signal = False
        signal_lines = 0
        lines = klog_lines(connection, args, until, ticks=True)
        while True:
            # (a connection the server closes, as rest mode may, ends the follow
            # as the title's going does; the fetch is tried after)
            try:
                line = next(lines)
            except StopIteration:
                break
            except DeployError as error:
                say(f"follow: {error}")
                status = FOLLOW_GONE
                break
            # (once the host has begun, whether the title still runs: a run
            # killed or ended without its last line ends the follow too)
            if seen_host and time.monotonic() >= next_status:
                next_status = time.monotonic() + STATUS_SECONDS
                try:
                    running = title_running(args)
                except DeployError as error:
                    say(f"follow: {error}")
                    running = True
                if not running:
                    say(f"follow: ps5vkctl says {args.title} no longer runs")
                    status = FOLLOW_GONE
                    break
            if line is None:
                continue
            log_file.write(line + "\n")
            at = line.find("halo: ")
            radv = line.find(RADV_PREFIX)
            if at >= 0:
                text = line[at:]
            elif radv >= 0:
                text = line[radv:]
            elif line.startswith("#") and (in_signal or line.startswith(FATAL_SIGNAL)):
                text = line
            else:
                continue
            if copies_owed.get(text, 0) > 0:
                copies_owed[text] -= 1
                continue
            copies_owed[text] = copies_owed.get(text, 0) + 1
            if text.startswith(FATAL_SIGNAL):
                in_signal = True
                signal_lines = 0
            if in_signal:
                say(f"  {text}")
                signal_lines += 1
                # (the block ends with its list of libraries; a signal after
                # the host's lines ends the follow, one before them is another
                # process's)
                if text.startswith("# dynamic libraries") or signal_lines > 40:
                    in_signal = False
                    if seen_host:
                        status = FOLLOW_SIGNAL
                        break
                continue
            seen_host = True
            say(text)
            if text.startswith(HOST_LAST_LINES):
                status = FOLLOW_LAST_LINE
                break
            log_file.flush()
    if status == FOLLOW_TIMEOUT and minutes:
        status = FOLLOW_MINUTES
        say(f"follow: {minutes:g} minutes have passed")
    elif status == FOLLOW_TIMEOUT:
        say(f"follow: no last line from the host within {args.timeout:.0f} s")
    say(f"klog capture: {capture}")
    return status


# ---------- the whole loop

def kill_and_wait(args: argparse.Namespace) -> None:
    if action_kill(args) != 0:
        raise DeployError(f"{args.title} could not be killed")
    for _ in range(20):
        if not title_running(args):
            return
        time.sleep(0.5)


def screenshot_fetcher(args: argparse.Namespace, folder: Path, stop: threading.Event) -> None:
    """a long run's screenshots into its folder every SCREENSHOT_FETCH_SECONDS,
    so the console's cap loses none (a thread of its own: the follower goes
    on reading the kernel log meanwhile)"""
    while not stop.wait(SCREENSHOT_FETCH_SECONDS):
        try:
            ftp = ftp_open(args)
            try:
                saved = fetch_screenshots(ftp, folder)
            finally:
                ftp_close(ftp)
            say(f"(fetched {len(saved)} screenshots into {folder.name} during the run)")
        except (DeployError, ftplib.Error, OSError) as error:
            say(f"(the screenshots could not be fetched during the run: {error})")


def action_run(args: argparse.Namespace) -> int:
    started = time.monotonic()
    if title_running(args):
        say(f"-- {args.title} is running: kill")
        kill_and_wait(args)
    fetch_leftovers(args)
    say("-- upload")
    action_upload(args)
    game_log_from = game_log_size(args)
    say("-- launch and follow")
    folder = new_log_folder(args)
    launched = []
    stop = threading.Event()
    fetcher = threading.Thread(target=screenshot_fetcher, args=(args, folder, stop), daemon=True)

    def launch() -> None:
        launched.append(time.monotonic())
        if action_launch(args) != 0:
            raise DeployError(f"{args.title} was not launched")
        if args.minutes:
            fetcher.start()

    try:
        status = action_follow(args, folder, on_ready=launch, minutes=args.minutes)
    finally:
        stop.set()
        if fetcher.is_alive():
            fetcher.join()
    if status in (FOLLOW_MINUTES, FOLLOW_TIMEOUT) and title_running(args):
        say(f"-- {args.title} still runs: kill")
        kill_and_wait(args)
    say("-- this run's files")
    action_fetch(args, folder, game_log_from)
    say(f"--run: {time.monotonic() - started:.1f} s in all, {time.monotonic() - launched[0]:.1f} s from the launch"
        if launched else f"--run: {time.monotonic() - started:.1f} s")
    return 0 if status == FOLLOW_LAST_LINE or (status == FOLLOW_MINUTES and args.minutes) else 1


ACTIONS = {
    "fetch": action_fetch,
    "upload": action_upload,
    "status": action_status,
    "launch": action_launch,
    "kill": action_kill,
    "restart": action_restart,
    "follow": action_follow,
    "load-ps5vkctl": action_load_ps5vkctl,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("actions", nargs="*", metavar="ACTION",
                        help="fetch, upload, status, launch, kill, restart, follow, load-ps5vkctl")
    parser.add_argument("--run", action="store_true", help="fetch, kill if running, upload, launch and follow, fetch")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--ftp-port", type=int, default=2121)
    parser.add_argument("--ctl-port", type=int, default=9111)
    parser.add_argument("--klog-port", type=int, default=3232)
    parser.add_argument("--manager-port", type=int, default=8084)
    parser.add_argument("--title", default=DEFAULT_TITLE)
    parser.add_argument("--title-dir", default=str(ROOT / "build" / "ps5" / "title" / DEFAULT_TITLE))
    parser.add_argument("--logs", default=str(ROOT / "build" / "ps5" / "logs"))
    parser.add_argument("--timeout", type=float, default=180.0, help="follow's, in seconds (default 180)")
    parser.add_argument("--minutes", type=float, default=0.0,
                        help="with --run: follow this long from the launch, fetching the screenshots every 5 "
                             "minutes, then kill the title and fetch")
    parser.add_argument("--hold", action="store_true", help="upload puts hold.txt in the title's folder")
    parser.add_argument("--force", action="store_true", help="upload while the title runs")
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                        help="upload writes environment.txt in the title's folder with this line (again for each)")
    parser.add_argument("--ps5vkctl", default=str(DEFAULT_PS5VKCTL), help="the payload load-ps5vkctl sends")
    args = parser.parse_args()
    if not args.run and not args.actions:
        parser.error("name an action, or --run")
    if args.minutes and not args.run:
        parser.error("--minutes is --run's")
    bad = [setting for setting in args.env if "=" not in setting or setting.startswith("=") or "\n" in setting]
    if bad:
        parser.error(f"--env takes NAME=VALUE, not {', '.join(bad)}")
    unknown = [action for action in args.actions if action not in ACTIONS]
    if unknown:
        parser.error(f"unknown action {', '.join(unknown)}: {', '.join(ACTIONS)}")
    try:
        if args.run:
            return action_run(args)
        status = 0
        for action in args.actions:
            status = ACTIONS[action](args)
            if status != 0:
                break
        return status
    except DeployError as error:
        print(f"ps5_deploy: {error}", file=sys.stderr)
        return 1
    except ftplib.Error as error:
        print(f"ps5_deploy: the FTP server at {args.host}:{args.ftp_port}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

"""Watch the projects folder for palette/passes pairs and upload each pair zipped to palettes/."""

import base64
import ctypes
import hashlib
import json
import os
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from datetime import datetime, timedelta
from io import BytesIO
from pathlib import Path

PROJECTS = Path(r"D:\[Library]\[Audio]\[Works]\[Projects]")
API = "https://api.github.com"
REPO = "to-the-sun/amanuensis-analysis"
REMOTE_DIR = "palettes"
BRANCH = "main"
POLL_SECONDS = 30
SETTLE_SECONDS = 10

HERE = Path(__file__).resolve().parent
LOCAL = Path(os.environ.get("LOCALAPPDATA") or Path.home()) / "palette_watcher"
STATE = LOCAL / "state.json"
LOG = LOCAL / "watcher.log"
PAIR = (("palette_", ".wav"), ("passes_", ".txt"))


def log(message):
    LOCAL.mkdir(parents=True, exist_ok=True)
    with LOG.open("a", encoding="utf-8") as handle:
        handle.write(f"{datetime.now():%Y-%m-%d %H:%M:%S} {message}\n")


def say(message):
    print(f"{datetime.now():%H:%M:%S} {message}", flush=True)


def describe(error):
    if isinstance(error, urllib.error.HTTPError):
        return f"HTTP {error.code} {error.read().decode('utf-8', 'replace')[:300]}"
    return traceback.format_exc().strip().splitlines()[-1]


def fail(message):
    log(message)
    ctypes.windll.user32.MessageBoxW(None, message, "Palette watcher", 0x10)
    raise SystemExit(1)


def token():
    if os.environ.get("GITHUB_TOKEN"):
        return os.environ["GITHUB_TOKEN"]
    if (HERE / "github_token.txt").exists():
        return (HERE / "github_token.txt").read_text(encoding="utf-8").strip()
    for path in (HERE / "credentials.json", HERE / "transcription" / "credentials.json"):
        if path.exists():
            value = json.loads(path.read_text(encoding="utf-8")).get("github_token")
            if value:
                return value
    fail(f"Save a fine-grained GitHub token with contents read/write on {REPO} as github_token.txt next to {Path(__file__).name}.")


def remote(path, method="GET", payload=None):
    request = urllib.request.Request(
        f"{API}/repos/{REPO}/{path}",
        json.dumps(payload).encode() if payload else None,
        {
            "Authorization": f"Bearer {token()}",
            "Accept": "application/vnd.github+json",
            "User-Agent": "palette-watcher",
        },
        method=method,
    )
    return urllib.request.urlopen(request, timeout=120)


def blob_sha(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def upload(name, data, message):
    path = f"contents/{REMOTE_DIR}/{urllib.parse.quote(name)}"
    sha = None
    try:
        with remote(path) as response:
            sha = json.load(response)["sha"]
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
    if sha == blob_sha(data):
        return False
    payload = {"message": message, "content": base64.b64encode(data).decode(), "branch": BRANCH}
    if sha:
        payload["sha"] = sha
    remote(path, "PUT", payload).close()
    return True


def bundle(entries):
    buffer = BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for entry in entries:
            archive.write(entry, entry.name)
    return buffer.getvalue()


def modified(entry):
    return datetime.fromtimestamp(entry.stat().st_mtime)


def pairs(since, until):
    for folder in sorted(entry for entry in PROJECTS.iterdir() if entry.is_dir()):
        found = {}
        for entry in folder.iterdir():
            name = entry.name.lower()
            for prefix, extension in PAIR:
                if name.startswith(prefix) and name.endswith(extension):
                    found.setdefault(name[len(prefix):-len(extension)], {})[prefix] = entry
        settled = {
            key: match
            for key, match in found.items()
            if len(match) == len(PAIR) and all(modified(match[prefix]) <= until for prefix, _ in PAIR)
        }
        if any(since < modified(entry) for match in settled.values() for entry in match.values()):
            yield folder.name, [settled[key][prefix] for key in sorted(settled) for prefix, _ in PAIR]


def load_state():
    try:
        return datetime.fromisoformat(json.loads(STATE.read_text(encoding="utf-8"))["last_check"])
    except (OSError, ValueError, KeyError):
        return None


def save_state(moment):
    LOCAL.mkdir(parents=True, exist_ok=True)
    STATE.write_text(json.dumps({"last_check": moment.isoformat()}), encoding="utf-8")


def sweep():
    since = load_state()
    if since is None:
        baseline = datetime.now()
        save_state(baseline)
        message = f"no previous check time; nothing uploaded, next check starts at {baseline:%Y-%m-%d %H:%M:%S}"
        say(message)
        log(message)
        return
    until = datetime.now() - timedelta(seconds=SETTLE_SECONDS)
    uploaded = 0
    earliest_failure = None
    for folder, entries in pairs(since, until):
        try:
            name = f"{folder}.zip"
            if upload(name, bundle(entries), f"Add {folder} palette zip"):
                say(f"uploaded {REMOTE_DIR}/{name}")
                log(f"uploaded {REMOTE_DIR}/{name}")
                uploaded += 1
            else:
                say(f"already up to date: {REMOTE_DIR}/{name}")
        except Exception as error:
            message = f"failed {folder}.zip: {describe(error)}"
            say(message)
            log(message)
            stuck = min(modified(entry) for entry in entries)
            earliest_failure = stuck if earliest_failure is None else min(earliest_failure, stuck)
    if not uploaded:
        say(f"nothing new up to {until:%H:%M:%S}")
    save_state(until if earliest_failure is None else min(until, earliest_failure - timedelta(seconds=1)))


def main():
    say(f"watching {PROJECTS}")
    say(f"log: {LOG}")
    log(f"watching {PROJECTS}")
    problem = None
    while True:
        try:
            sweep()
            problem = None
        except Exception as error:
            message = f"failed: {describe(error)}"
            if message != problem:
                say(message)
                log(message)
                problem = message
        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()

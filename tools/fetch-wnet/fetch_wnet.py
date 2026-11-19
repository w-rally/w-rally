"""fetch_wnet - the pinned WNet.dll (the net module, from the private w-rally/wnet repo), downloaded and verified.

    python tools/fetch-wnet/fetch_wnet.py <out dir> [--lock cmake/wnet.lock]

cmake/wnet.lock pins the release: its repo and tag, the manifest's version, WNet.dll's SHA-256 and the release
signing public key. The DLL is used only when all of these hold:
  - the manifest's Ed25519 signature verifies under the pinned public key (and the key id matches),
  - the manifest describes this DLL (size, SHA-256) and is the pinned version,
  - the DLL's SHA-256 is the pinned one.
This protects players from a tampered download (supply chain); it is not anti-cheat.

The repo is private: a token that can read it comes from WNET_GITHUB_TOKEN, else GH_TOKEN / GITHUB_TOKEN, else
`gh auth token`. Without one the script says so and exits 2 (the build goes on without the net module: offline).
Exit 0: <out dir>/WNet.dll is the verified pinned DLL (already there and verified: no download). 1: the download
failed. 2: no token. 3: downloaded, but NOT VERIFIED (never used).

Standard library only, its own verify-only Ed25519 (RFC 8032 5.1.7) below.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

RELEASE_TAG = b"wnet-release-v1\x00"   # what WNet's tools/release_sign.py signs: this, then the manifest's bytes
ASSETS = ("WNet.dll", "WNet.dll.manifest.json", "WNet.dll.manifest.json.sig")

# ------------------------------------------------------------- Ed25519 verify (RFC 8032 5.1.7)
_P = 2 ** 255 - 19
_L = 2 ** 252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _add(a, b):
    A = (a[1] - a[0]) * (b[1] - b[0]) % _P
    B = (a[1] + a[0]) * (b[1] + b[0]) % _P
    C = 2 * a[3] * b[3] * _D % _P
    D = 2 * a[2] * b[2] % _P
    E, F, G, H = B - A, D - C, D + C, B + A
    return (E * F % _P, G * H % _P, F * G % _P, E * H % _P)


def _mul(s, p):
    q = (0, 1, 1, 0)
    while s:
        if s & 1:
            q = _add(q, p)
        p = _add(p, p)
        s >>= 1
    return q


def _x(y, sign):
    if y >= _P:
        return None
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P:
        return None
    return _P - x if (x & 1) != sign else x


def _point(b):
    y = int.from_bytes(b, "little")
    sign, y = y >> 255, y & ((1 << 255) - 1)
    x = _x(y, sign)
    return None if x is None else (x, y, 1, x * y % _P)


_G = (lambda y: (_x(y, 0), y, 1, _x(y, 0) * y % _P))(4 * pow(5, _P - 2, _P) % _P)


def ed25519_verify(pub: bytes, msg: bytes, sig: bytes) -> bool:
    if len(pub) != 32 or len(sig) != 64:
        return False
    a, r = _point(pub), _point(sig[:32])
    s = int.from_bytes(sig[32:], "little")
    if a is None or r is None or s >= _L:
        return False
    k = int.from_bytes(hashlib.sha512(sig[:32] + pub + msg).digest(), "little") % _L
    lhs, rhs = _mul(s, _G), _add(r, _mul(k, a))
    return (lhs[0] * rhs[2] - rhs[0] * lhs[2]) % _P == 0 and (lhs[1] * rhs[2] - rhs[1] * lhs[2]) % _P == 0


# ------------------------------------------------------------------------------ verification
def verify(dll: bytes, manifest: bytes, sig_line: str, lock: dict) -> str:
    """'' when the DLL is the pinned, signed one; else what is wrong."""
    pub = bytes.fromhex(lock["publicKey"])
    parts = sig_line.split()
    if len(parts) != 3 or parts[0] != "ed25519":
        return "the signature file is not 'ed25519 <key id> <signature>'"
    if parts[1] != hashlib.sha256(pub).digest()[:8].hex():
        return f"signed with key {parts[1]}, not the pinned key"
    if not ed25519_verify(pub, RELEASE_TAG + manifest, bytes.fromhex(parts[2])):
        return "the manifest's signature does not verify"
    m = json.loads(manifest)
    actual = hashlib.sha256(dll).hexdigest()
    if m.get("sha256") != actual or m.get("size") != len(dll):
        return "WNet.dll is not the file the signed manifest describes"
    if m.get("version") != lock["version"]:
        return f"the manifest is version {m.get('version')}, the lock pins {lock['version']}"
    if actual != lock["sha256"].lower():
        return f"WNet.dll's SHA-256 is {actual}, the lock pins {lock['sha256']}"
    return ""


def verify_dir(d: Path, lock: dict) -> str:
    try:
        return verify((d / ASSETS[0]).read_bytes(), (d / ASSETS[1]).read_bytes(),
                      (d / ASSETS[2]).read_text(encoding="ascii"), lock)
    except OSError as e:
        return str(e)


# ---------------------------------------------------------------------------------- download
def token() -> str:
    for name in ("WNET_GITHUB_TOKEN", "GH_TOKEN", "GITHUB_TOKEN"):
        if os.environ.get(name):
            return os.environ[name]
    try:
        return subprocess.run(["gh", "auth", "token"], capture_output=True, text=True, timeout=20).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def _get(url: str, tok: str, accept: str) -> bytes:
    """GitHub's API with the token; an asset's redirect to its storage followed WITHOUT the token."""
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {tok}", "Accept": accept,
                                               "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "wrally-fetch-wnet"})
    opener = urllib.request.build_opener(_NoRedirect)
    try:
        with opener.open(req, timeout=60) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code in (301, 302, 303, 307, 308) and e.headers.get("Location"):
            plain = urllib.request.Request(e.headers["Location"], headers={"User-Agent": "wrally-fetch-wnet"})
            with urllib.request.urlopen(plain, timeout=120) as r:
                return r.read()
        raise


def download(lock: dict, tok: str, to: Path):
    api = f"https://api.github.com/repos/{lock['repo']}/releases/tags/{lock['tag']}"
    release = json.loads(_get(api, tok, "application/vnd.github+json"))
    assets = {a["name"]: a["url"] for a in release.get("assets", [])}
    to.mkdir(parents=True, exist_ok=True)
    for name in ASSETS:
        if name not in assets:
            raise RuntimeError(f"release {lock['tag']} has no {name}")
        (to / name).write_bytes(_get(assets[name], tok, "application/octet-stream"))


def main(argv) -> int:
    args = argv[1:]
    lock_path = Path(__file__).resolve().parents[2] / "cmake" / "wnet.lock"
    if "--lock" in args:
        i = args.index("--lock")
        lock_path = Path(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 1
    out = Path(args[0])
    lock = json.loads(lock_path.read_text(encoding="utf-8"))

    if (out / ASSETS[0]).exists() and not verify_dir(out, lock):
        print(f"fetch-wnet: {out / 'WNet.dll'}: WNet {lock['version']}, verified")
        return 0
    tok = token()
    if not tok:
        print("fetch-wnet: no token for the private w-rally/wnet releases (WNET_GITHUB_TOKEN, GH_TOKEN, or `gh auth "
              "login`): building without the net module", file=sys.stderr)
        return 2
    tmp = out.with_name(out.name + ".download")
    shutil.rmtree(tmp, ignore_errors=True)
    try:
        download(lock, tok, tmp)
    except (urllib.error.URLError, RuntimeError, OSError, ValueError) as e:
        print(f"fetch-wnet: download of {lock['repo']} {lock['tag']} failed: {e}", file=sys.stderr)
        shutil.rmtree(tmp, ignore_errors=True)
        return 1
    why = verify_dir(tmp, lock)
    if why:
        print(f"fetch-wnet: NOT VERIFIED, not used: {why}", file=sys.stderr)
        shutil.rmtree(tmp, ignore_errors=True)
        return 3
    shutil.rmtree(out, ignore_errors=True)
    tmp.rename(out)
    print(f"fetch-wnet: {out / 'WNet.dll'}: WNet {lock['version']}, downloaded and verified")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

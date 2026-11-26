#!/usr/bin/env python3
"""Creates a GitHub release of ravencaio/tlou-remastered-pc and uploads its packages to it.

  python tools/publish_release.py v0.2.4 <full commit SHA> tools/release-notes/v0.2.4.md \
      dist/TLOU-Remastered-windows.zip [dist/TLOU-Remastered-linux.zip]

  python tools/publish_release.py --add v0.2.4 dist/TLOU-Remastered-linux.zip
      (uploads a package to a release that already exists)

The token comes from Git's credential manager (`git credential fill`); gh is not needed. The
commit must already be on GitHub, and it must be the FULL 40-character SHA (a short one gets
HTTP 422). Each package must carry one of the names in PACKAGES: the launcher's updater
(launcher/updater.py) looks for its platform's name and that asset's SHA-256 digest. The Linux
package is normally attached by the "Linux package" workflow (.github/workflows/linux-package.yml)
once the release is published. See tools/RELEASING.md.
"""
import json
import subprocess
import sys
import urllib.parse
import urllib.request
from pathlib import Path

REPO = "ravencaio/tlou-remastered-pc"
# The asset names launcher/updater.py looks for, one per platform.
PACKAGES = ("TLOU-Remastered-windows.zip", "TLOU-Remastered-linux.zip")


def github():
    """An API caller authenticated with the token from Git's credential manager."""
    cred = subprocess.run(["git", "credential", "fill"],
                          input="protocol=https\nhost=github.com\n\n",
                          capture_output=True, text=True, check=True).stdout
    token = dict(line.split("=", 1) for line in cred.splitlines() if "=" in line)["password"]
    headers = {"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
               "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "release-script"}

    def call(url, data=None, content_type="application/json"):
        req = urllib.request.Request(url, data=data, headers={**headers,
                                                              "Content-Type": content_type})
        with urllib.request.urlopen(req) as resp:
            return json.loads(resp.read())

    return call


def checked_packages(paths):
    """The package paths, after checking each exists and has a name the updater knows."""
    packages = [Path(path) for path in paths]
    for package in packages:
        if package.name not in PACKAGES:
            raise SystemExit(f"{package.name}: a package must be named one of {', '.join(PACKAGES)} "
                             "(launcher/updater.py looks for these)")
        if not package.is_file():
            raise SystemExit(f"{package}: not found")
    names = [package.name for package in packages]
    if len(set(names)) != len(names):
        raise SystemExit("the same package name is given twice")
    return packages


def upload(call, release, package):
    asset = call(f"https://uploads.github.com/repos/{REPO}/releases/{release['id']}/assets"
                 f"?name={urllib.parse.quote(package.name)}", package.read_bytes(), "application/zip")
    print("asset", asset["id"], asset["size"], asset.get("digest"), asset["browser_download_url"])


def main():
    args = sys.argv[1:]
    if len(args) >= 3 and args[0] == "--add":
        tag, packages = args[1], checked_packages(args[2:])
        call = github()
        release = call(f"https://api.github.com/repos/{REPO}/releases/tags/{urllib.parse.quote(tag)}")
        present = {asset["name"] for asset in release.get("assets", [])}
        for package in packages:
            if package.name in present:
                raise SystemExit(f"{tag} already has {package.name}; delete it on GitHub first")
        print("release", release["id"], release["html_url"])
        for package in packages:
            upload(call, release, package)
        return
    if len(args) < 4:
        raise SystemExit(__doc__)
    tag, sha, notes = args[:3]
    packages = checked_packages(args[3:])
    if len(sha) != 40:
        raise SystemExit("give the full 40-character commit SHA (git rev-parse HEAD)")
    call = github()
    body = Path(notes).read_text(encoding="utf-8")
    release = call(f"https://api.github.com/repos/{REPO}/releases",
                   json.dumps({"tag_name": tag, "target_commitish": sha,
                               "name": f"Release {tag}", "body": body,
                               "draft": False, "prerelease": False}).encode())
    print("release", release["id"], release["html_url"])
    for package in packages:
        upload(call, release, package)


if __name__ == "__main__":
    main()

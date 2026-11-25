"""metronet-latest.json of a release: what a launcher reads to know the newest version.

    python src/launcher/release_manifest.py <metronet-launcher.exe> <out.json>

A release is one file for players, metronet-launcher.exe, with the mod inside. release.json (dist/release.json
when there is one, else src/launcher/release.json) holds what a person writes for each release: the version,
the protocol, the date, a title and up to four short notes for the launcher's news, and the protocol of
earlier versions. This adds the launcher's size and SHA-256. The json goes into the GitHub release as an asset
beside the launcher: a launcher asks for .../releases/latest/download/metronet-latest.json, downloads the file
it names, checks it and installs the mod from inside it. The release's tag is v<mod version>.
"""
import hashlib
import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def source(path):
    return io.open(os.path.join(HERE, path), encoding="utf-8").read()


def main(launcher, out):
    notes = os.path.join(HERE, "dist", "release.json")
    if not os.path.exists(notes):
        notes = os.path.join(HERE, "src", "launcher", "release.json")
    rel = json.load(io.open(notes, encoding="utf-8-sig"))
    version = re.search(r'METRONET_VERSION "([^"]+)"', source("src/common/version.h")).group(1)
    protocol = int(re.search(r"kProtocol = (\d+)", source("src/mod/link.h")).group(1))
    lver = re.search(r'kLauncherVersion\[\] = L"([^"]+)"', source("src/launcher/launcher.h")).group(1)
    assert rel["version"] == version, "%s is for %s, the source is %s" % (notes, rel["version"], version)
    assert rel["protocol"] == protocol, "%s says protocol %s, the source %s" % (notes, rel["protocol"], protocol)
    assert 1 <= len(rel["notes"]) <= 4 and all(len(n) <= 110 for n in rel["notes"]), "one to four notes of at most 110 characters"
    assert len(rel["title"]) <= 40, "a title of at most 40 characters"
    data = open(launcher, "rb").read()
    assert ("version %s\n" % version).encode() in data and ("launcher %s\n" % lver).encode() in data, \
        "this launcher was not built from this source (its package index says otherwise): rebuild"
    file = {"name": os.path.basename(launcher), "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "version": lver}
    doc = {"schema": 2, "version": version, "protocol": protocol, "date": rel["date"], "title": rel["title"], "notes": rel["notes"],
           "launcher": file, "protocols": rel["protocols"]}
    io.open(out, "w", encoding="utf-8", newline="\n").write(json.dumps(doc, ensure_ascii=False, indent=2) + "\n")
    print("written:", out)
    print("  launcher %s with the mod %s (protocol %d) inside: %d bytes, sha256 %s" % (lver, version, protocol, file["size"], file["sha256"]))


if __name__ == "__main__":
    main(*sys.argv[1:3])

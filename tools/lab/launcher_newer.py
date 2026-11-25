"""Makes a "newer release" for the launcher's tests out of a built launcher: a copy whose package index
(resource 300) and release notes (resource 299) say another version of the mod and of the launcher, and the
metronet-latest.json that describes that copy.

    python tools/lab/launcher_newer.py <metronet-launcher.exe> <out folder> <mod version> <launcher version>
"""
import ctypes
import hashlib
import io
import json
import os
import re
import shutil
import struct
import sys
from ctypes import wintypes as wt

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.BeginUpdateResourceW.restype = wt.HANDLE
k32.BeginUpdateResourceW.argtypes = [wt.LPCWSTR, wt.BOOL]
k32.UpdateResourceW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR, wt.WORD, ctypes.c_void_p, wt.DWORD]
k32.EndUpdateResourceW.argtypes = [wt.HANDLE, wt.BOOL]
RT_RCDATA = ctypes.cast(10, wt.LPCWSTR)


def res(path, rid):
    """A resource's bytes and its language, read from the file's own resource table."""
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = struct.unpack_from("<H", d, pe + 20)[0]
    sec = pe + 24 + opt
    rva, raw = 0, 0
    for i in range(nsec):
        name = d[sec + 40 * i:sec + 40 * i + 8].rstrip(bytes(1))
        if name == b".rsrc":
            rva, raw = struct.unpack_from("<I", d, sec + 40 * i + 12)[0], struct.unpack_from("<I", d, sec + 40 * i + 20)[0]
    assert raw, "no .rsrc"

    def entries(off):
        named, ids = struct.unpack_from("<HH", d, raw + off + 12)
        return [struct.unpack_from("<II", d, raw + off + 16 + 8 * i) for i in range(named + ids)]

    def child(off, want):
        for ident, to in entries(off):
            if ident == want:
                return to & 0x7FFFFFFF
        raise KeyError(want)

    by_lang = entries(child(child(0, 10), rid))
    lang, to = by_lang[0]
    data_rva, size = struct.unpack_from("<II", d, raw + to)
    at = raw + (data_rva - rva)
    return d[at:at + size], lang


def main(src, out, mod, launcher):
    os.makedirs(out, exist_ok=True)
    dst = os.path.join(out, "metronet-launcher.exe")
    shutil.copyfile(src, dst)
    index, lang_i = res(src, 300)
    notes, lang_n = res(src, 299)
    text = index.decode("utf-8")
    text = re.sub(r"(?m)^version \S+", "version " + mod, text)
    text = re.sub(r"(?m)^launcher \S+", "launcher " + launcher, text)
    rel = json.loads(notes.decode("utf-8-sig"))
    protocol = rel["protocol"]
    rel["version"] = mod
    rel["title"] = "Проверка обновления"
    rel["notes"] = ["Эта версия сделана для проверки обновления лаунчера."]
    rel["protocols"][mod] = protocol
    new_index, new_notes = text.encode("utf-8"), json.dumps(rel, ensure_ascii=False, indent=2).encode("utf-8")
    h = k32.BeginUpdateResourceW(dst, False)
    assert h, ctypes.get_last_error()
    for rid, lang, data in ((300, lang_i, new_index), (299, lang_n, new_notes)):
        buf = ctypes.create_string_buffer(data, len(data))
        assert k32.UpdateResourceW(h, RT_RCDATA, ctypes.cast(rid, wt.LPCWSTR), lang, buf, len(data)), ctypes.get_last_error()
    assert k32.EndUpdateResourceW(h, False), ctypes.get_last_error()
    data = open(dst, "rb").read()
    file = {"name": "metronet-launcher.exe", "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "version": launcher}
    doc = {"schema": 2, "version": mod, "protocol": protocol, "date": rel["date"], "title": rel["title"], "notes": rel["notes"],
           "launcher": file, "protocols": rel["protocols"]}
    io.open(os.path.join(out, "metronet-latest.json"), "w", encoding="utf-8", newline="\n").write(
        json.dumps(doc, ensure_ascii=False, indent=2) + "\n")
    return file


if __name__ == "__main__":
    made = main(*sys.argv[1:5])
    print("made: mod %s, launcher %s, %d bytes, sha256 %s" % (sys.argv[3], sys.argv[4], made["size"], made["sha256"][:16]))

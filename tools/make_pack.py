#!/usr/bin/env python3
"""Build an Ampere kernel pack for the Linux DLSS-G snippet from sdli1995's DLSSG SM86 release.

The release's fg_kernels.dll (MIT, github.com/sdli1995) holds sm_86 builds of every kernel of the Windows DLSS-G
runtime, keyed by the SHA-256 of the runtime's own blob. The Linux snippet (libnvidia-ngx-dlssg.so) of the same version
ships the same kernels; most blobs are byte-identical, the rest differ only in packaging and are matched by their PTX
entry name. Output: <out>/<sha256 of the Linux blob>.cubin, read by libdlssg_sm86.so at vkCreateCuModuleNVX.

usage: make_pack.py <fg_kernels.dll> <libnvidia-ngx-dlssg.so.X> <out dir> [--sm 86]
"""
import argparse, hashlib, json, os, re, struct, sys

try:
    import pefile
except ImportError:
    sys.exit("needs python-pefile (pacman -S python-pefile / pip install pefile)")


def resources(path):
    pe = pefile.PE(path)
    out = {}
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        for e in t.directory.entries:
            key = str(e.name) if e.name else e.id
            d = e.directory.entries[0].data.struct
            out[key] = pe.get_data(d.OffsetToData, d.Size)
    return out


def fatbins(data):
    i = 0
    while (i := data.find(b"\x50\xed\x55\xba", i)) != -1:
        _, hdr, size = struct.unpack_from("<HHQ", data, i + 4)
        if hdr == 16 and 0 < size < 64 << 20:
            yield data[i:i + 16 + size]
            i += 16 + size
        else:
            i += 4


def entries(blob):
    return sorted(set(m.decode() for m in re.findall(rb"\.entry ([A-Za-z0-9_$]+)", blob)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("snippet")
    ap.add_argument("out")
    ap.add_argument("--sm", default="86")
    a = ap.parse_args()

    res = resources(a.pack)
    manifest = json.loads(res["MANIFEST"])
    kernels = manifest["kernels"]
    by_hash = {k["blob_sha256"]: k for k in kernels}
    by_entry = {}
    for k in kernels:
        by_entry.setdefault(tuple(sorted(k["entries"])), []).append(k)

    os.makedirs(a.out, exist_ok=True)
    index, missing = [], []
    for blob in fatbins(open(a.snippet, "rb").read()):
        h = hashlib.sha256(blob).hexdigest()
        k, how = by_hash.get(h), "hash"
        if not k:
            cand = by_entry.get(tuple(entries(blob)), [])
            k, how = (cand[0], "entry") if len(cand) == 1 else (None, "")
        if not k or a.sm not in k["images"]:
            missing.append({"sha256": h, "bytes": len(blob), "entries": entries(blob)})
            continue
        image = res[k["images"][a.sm]["cubin"]]
        if image[:4] != b"\x7fELF":
            sys.exit(f"resource {k['images'][a.sm]['cubin']} is not a cubin")
        with open(os.path.join(a.out, h + ".cubin"), "wb") as f:
            f.write(image)
        index.append({"sha256": h, "by": how, "entries": k["entries"], "cubin": k["images"][a.sm]["cubin"]})

    with open(os.path.join(a.out, "index.json"), "w") as f:
        json.dump({"model": manifest["model"], "pack": manifest["id"], "sm": int(a.sm), "kernels": index,
                   "missing": missing}, f, indent=1)
    print(f"{len(index)} kernels written to {a.out} ({sum(x['by'] == 'hash' for x in index)} by hash), "
          f"{len(missing)} without an sm_{a.sm} image")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())

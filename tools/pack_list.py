"""List the resources of an sdli1995 DLSSG SM86 kernel pack (fg_kernels.dll)."""
import sys, pefile, collections
pe = pefile.PE(sys.argv[1], fast_load=False)
kinds = collections.Counter()
for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
    tname = str(t.name) if t.name else t.id
    for e in t.directory.entries:
        ename = str(e.name) if e.name else e.id
        for l in e.directory.entries:
            d = l.data.struct
            blob = pe.get_data(d.OffsetToData, d.Size)
            head = blob[:8]
            kinds[(tname, head[:4].hex())] += 1
            if not (isinstance(ename, int) and ename >= 1000):
                print(tname, ename, d.Size, blob[:200])
for k, v in sorted(kinds.items(), key=str): print(k, v)

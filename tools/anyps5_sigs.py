#!/usr/bin/env python3
"""Extract the C signatures of the AnyPS5 exports the runtime routes to (pkg/thunkgen/anyps5.go).

usage: (cd <anyps5>/core/libs/prx && python3 -I tools/anyps5_sigs.py > pkg/thunkgen/anyps5_sigs.tsv)
Scans APS5_VABI declarations and definitions (.hpp/.cpp) of libSceAgc, libSceAgcDriver, libSceVideoOut,
libScePad and libSceUserService. Internal symbols (_nid_no_patch, driver-internal _nid_postfix) are skipped; the symbol of an sce* _nid_postfix export is listed with its suffix; the first
signature of a name wins.
"""
import glob, re, sys

LIBS = ['libSceAgc', 'libSceAgcDriver', 'libSceVideoOut', 'libScePad', 'libSceUserService']
PAT = re.compile(r'^([A-Za-z_][\w:<>\s\*&,]*?)\s+APS5_VABI\s+(\w+)\s*\(([^)]*)\)\s*[{;]', re.M)
rows, seen = [], set()
for lib in LIBS:
    files = sorted(glob.glob(lib + '/**/*.cpp', recursive=True) + glob.glob(lib + '/**/*.hpp', recursive=True))
    for f in files:
        if '/tests/' in f or '/smoke/' in f:
            continue
        for m in PAT.finditer(open(f).read()):
            name = m.group(2)
            if 'nid_no_patch' in name or name in seen:
                continue
            # sceFoo_nid_postfix is the C symbol of the export sceFoo (the suffix keeps it apart from a namesake);
            # other *_nid_postfix functions are driver internals.
            if name.endswith('_nid_postfix') and not name.startswith('sce'):
                continue
            ret = ' '.join(m.group(1).split())
            if ret == 'return' or ret.endswith(' return'):
                continue
            seen.add(name)
            rows.append((lib, name, ret, ' '.join(m.group(3).split())))
rows.sort()
print('# lib\tname\treturn\tparams   (tools/anyps5_sigs.py over AnyPS5 core/libs/prx, APS5_VABI declarations)')
for r in rows:
    print('\t'.join(r))

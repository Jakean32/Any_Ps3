#!/usr/bin/env python3
"""Generate core/include/anyps3/hle/module_tables.h from public NID tables.

Sources (vendored under tools/reference/psl1ght/, all public):
  - PSL1GHT stub-library export tables (MIT license; see
    tools/reference/psl1ght/PSL1GHT_LICENSE). These are the low-level SPRX
    export NIDs that retail games import through their libstub tables.
  - rpcs3_ppu_function_names.cpp: a NID->name display table published by the
    RPCS3 project (GPLv2). Only the NID->name FACTS are used here (numeric
    identifiers and function names are not copyrightable); no code is taken
    from that project. It supplies the real LV2 names for sysPrxForUser and
    the C-library NIDs.

The PS3 module names are the real /dev_flash SPRX module names (psdevwiki);
where a name is a best effort it is marked in the comment and confirmed
against the user's bring-up log later.

Regenerate:  python3 tools/reference/gen_module_tables.py
"""
import re, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REF = os.path.join(ROOT, 'tools', 'reference', 'psl1ght')
OUT = os.path.join(ROOT, 'core', 'include', 'anyps3', 'hle', 'module_tables.h')

# PSL1GHT stub library -> real PS3 module name (the module the game imports).
PSL1GHT_MODULES = {
    'liblv2': ('sysPrxForUser', 'LV2 syscall wrappers (threads, sync, prx, memory, fs, strings)'),
    'libsysutil': ('cellSysutil', 'system utility (low-level sysUtil*/video*/msgDialog* exports)'),
    'libsysmodule': ('cellSysmodule', 'sysModule* exports'),
    'libgcm_sys': ('cellGcmSys', 'graphics command manager system side'),
    'libaudio': ('cellAudio', 'audio system side'),
    'libspurs': ('cellSpurs', 'SPURS task framework'),
    'libfont': ('cellFont', 'font renderer'),
    'libnet': ('cellNet', 'network (best-effort name)'),
    'libnetctl': ('cellNetCtl', 'network control'),
    'libio': ('cellIo', 'misc io exports not covered by the cellPad/Mouse/Kb split'),
}
# libio mixes several real modules by name prefix.
LIBIO_SPLIT = {
    'ioPad': 'cellPad', 'ioMouse': 'cellMouse', 'ioKb': 'cellKb',
    'sys_config': 'sys_config',
}

def parse_psl1ght(path):
    txt = open(path).read()
    return re.findall(r'EXPORT\(\s*(\w+)\s*,\s*0x([0-9a-fA-F]+)\s*\)', txt)

def parse_rpcs3_module(src, module):
    """Extract `case 0xNID: return "name";` inside one module block."""
    lines = src.splitlines()
    start = None
    for i, l in enumerate(lines):
        if f'_module == "{module}"' in l:
            start = i
            break
    if start is None:
        return []
    out = []
    for l in lines[start:]:
        m = re.match(r'\s*case 0x([0-9a-fA-F]+): return "(\w+)";', l)
        if m:
            out.append((m.group(2), int(m.group(1), 16)))
        if re.match(r'^}', l):  # end of function
            break
    return out

def main():
    entries = {}   # (module) -> [(nid, name)]
    sources = {}

    for lib, (mod, desc) in PSL1GHT_MODULES.items():
        path = os.path.join(REF, f'{lib}_exports.h')
        if not os.path.exists(path):
            continue
        pairs = parse_psl1ght(path)
        for name, nid in pairs:
            target = mod
            if lib == 'libio':
                target = mod  # default bucket (io* misc)
                for prefix, real in LIBIO_SPLIT.items():
                    if name.startswith(prefix):
                        target = real
                        break
            entries.setdefault(target, [])
            entries[target].append((int(nid, 16), name))
        sources[mod] = f'PSL1GHT {lib}/exports.h (MIT)'

    rpcs3_src = open(os.path.join(REF, 'rpcs3_ppu_function_names.cpp')).read()

    # sysPrxForUser: real LV2 names from the RPCS3 display table win over the
    # PSL1GHT aliases (same NIDs — cross-verified).
    prx = parse_rpcs3_module(rpcs3_src, 'sysPrxForUser')
    if prx:
        by_nid = {nid: name for name, nid in prx}
        merged = []
        seen = set()
        for nid, name in entries.get('sysPrxForUser', []):
            merged.append((nid, by_nid.get(nid, name)))
            seen.add(nid)
        for name, nid in prx:
            if nid not in seen:
                merged.append((nid, name))
        entries['sysPrxForUser'] = merged
        sources['sysPrxForUser'] += ' + rpcs3 NID->name display table (cross-check)'

    # C library: register under all module names games are known to use.
    for real in ('libc', 'sys_libc'):
        entries[real] = [(nid, name) for name, nid in parse_rpcs3_module(rpcs3_src, 'sys_libc')]
        sources[real] = 'rpcs3 NID->name display table (facts only)'
    for real in ('libm', 'sys_libm'):
        entries[real] = [(nid, name) for name, nid in parse_rpcs3_module(rpcs3_src, 'sys_libm')]
        sources[real] = 'rpcs3 NID->name display table (facts only)'
    for real in ('libstdcxx', 'sys_libstdcxx'):
        entries[real] = [(nid, name) for name, nid in parse_rpcs3_module(rpcs3_src, 'sys_libstdcxx')]
        sources[real] = 'rpcs3 NID->name display table (facts only)'

    h = []
    h.append('// AUTO-GENERATED by tools/reference/gen_module_tables.py — do not edit.')
    h.append('// Public NID tables: NID values and function names are public interface')
    h.append('// facts (see the generator script header for the exact sources).')
    h.append('#pragma once')
    h.append('#include <cstddef>')
    h.append('#include <cstdint>')
    h.append('')
    h.append('namespace anyps3 {')
    h.append('')
    h.append('struct ModuleExportInfo {')
    h.append('    const char* module;')
    h.append('    uint32_t nid;')
    h.append('    const char* name;  // public interface name (diagnostics)')
    h.append('};')
    h.append('')
    total = 0
    for mod in sorted(entries):
        pairs = sorted(entries[mod])
        if not pairs:
            continue
        # de-dup NIDs per module
        seen = set()
        uniq = []
        for nid, name in pairs:
            if nid in seen:
                continue
            seen.add(nid)
            uniq.append((nid, name))
        total += len(uniq)
        h.append(f'// module "{mod}" — {len(uniq)} exports — source: {sources.get(mod, "?")}')
        h.append(f'inline constexpr ModuleExportInfo kExports_{re.sub(r"[^A-Za-z0-9]", "_", mod)}[] = {{')
        for nid, name in uniq:
            h.append(f'    {{"{mod}", 0x{nid:08x}u, "{name}"}},')
        h.append('};')
        h.append('')
    h.append('inline constexpr const ModuleExportInfo* kAllModuleExportLists[] = {')
    for mod in sorted(entries):
        if not entries[mod]:
            continue
        h.append(f'    kExports_{re.sub(r"[^A-Za-z0-9]", "_", mod)},')
    h.append('};')
    sizes = [len([1 for nid, _ in sorted(entries[mod]) if nid not in [n for n, _ in sorted(entries[mod])[:sorted(entries[mod]).index((nid, _)) if False else -1]]]) for mod in ()]
    h.append('inline constexpr size_t kAllModuleExportSizes[] = {')
    for mod in sorted(entries):
        pairs = sorted(entries[mod])
        if not pairs:
            continue
        seen, n = set(), 0
        for nid, name in pairs:
            if nid in seen:
                continue
            seen.add(nid)
            n += 1
        h.append(f'    {n},')
    h.append('};')
    h.append('')
    h.append(f'// total known exports: {total}')
    h.append('')
    h.append('}  // namespace anyps3')
    open(OUT, 'w').write('\n'.join(h) + '\n')
    print(f'wrote {OUT}: {len(entries)} modules, {total} exports')

if __name__ == '__main__':
    main()

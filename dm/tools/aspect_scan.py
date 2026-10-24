#!/usr/bin/env python3
# Find where a game's code holds the 16:9 aspect ratio, to patch it to another one (the 1.43:1 iPad view shows the game's 16:9 picture letterboxed; a game draws a full 1.43:1 picture only once its own
# projection and HUD layout use that ratio). The scanner reads the game's main NSO and lists, as module offsets (main+0x...):
#   code      a MOVZ / MOVK pair that builds 16/9 as a 32-bit float (0x3FE38E39) in a register, within 4 instructions of each other, either order; the pair is printed as a ready-to-paste CUTS entry
#             (dm/tools/patch_nso.py: (offset, old bytes, new bytes, comment); the rewriter checks the old bytes) that rebuilds the register with the new ratio instead
#   data      the 32-bit float 16/9 and the double 16/9 in the read-only and data segments (literal pools: one load site per user, so patching one changes every user; listed for finding the load sites, not patched)
# It does not know which hit is the camera's projection and which the HUD's layout: that takes a trial per candidate on a device (patch one, look at the picture). Nothing here is specific to one game.
# Usage: aspect_scan.py <main NSO> [--ratio 1.43] [--cuts] [--raw]
#   --ratio R   the ratio the CUTS entries build (default 1.43, the viewport front_ios.m fits)
#   --cuts      print only the CUTS lines
#   --raw       the file is an already decompressed module image (as dm/tools/nso.py writes), text at 0
import struct, sys

def load_nso(path):
    d = open(path, 'rb').read()
    if d[:4] != b'NSO0': sys.exit(f'{path}: not an NSO (no NSO0 magic); use --raw for a decompressed image')
    flags = struct.unpack_from('<I', d, 0xc)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
    csz = struct.unpack_from('<III', d, 0x60)
    out = []
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        if flags >> i & 1:
            try: import lz4.block
            except ImportError: sys.exit('this NSO is compressed: pip install lz4 (the repo\'s tools need it as well)')
            raw = lz4.block.decompress(raw, uncompressed_size=sz)
        if len(raw) != sz: sys.exit(f'segment {i}: {len(raw)} bytes, expected {sz}')
        out.append((mo, bytearray(raw)))
    return d[0x40:0x50].hex(), out

def f32_bits(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def movz(rd, imm, hw=0): return 0x52800000 | hw << 21 | imm << 5 | rd   # movz wd, #imm, lsl #(16*hw)
def movk(rd, imm, hw=0): return 0x72800000 | hw << 21 | imm << 5 | rd
def is_movz(w): return w & 0x7F800000 == 0x52800000 and w >> 31 == 0
def is_movk(w): return w & 0x7F800000 == 0x72800000 and w >> 31 == 0
def imm_hw(w): return w >> 5 & 0xFFFF, w >> 21 & 3

def main():
    a = sys.argv[1:]
    raw = '--raw' in a; cuts_only = '--cuts' in a; ratio = 1.43
    if '--ratio' in a:
        try: ratio = float(a[a.index('--ratio') + 1])
        except (IndexError, ValueError): sys.exit('--ratio needs a number')
    if not 0.5 < ratio < 4: sys.exit('--ratio out of range')
    files = [x for i, x in enumerate(a) if not x.startswith('--') and (i == 0 or a[i - 1] != '--ratio')]
    if len(files) != 1: sys.exit(__doc__)
    if raw:
        bid = '?'; img = open(files[0], 'rb').read(); segs = [(0, bytearray(img))]
    else:
        bid, segs = load_nso(files[0])
    OLD = f32_bits(16 / 9); NEW = f32_bits(ratio)
    ohi, olo, nhi, nlo = OLD >> 16, OLD & 0xFFFF, NEW >> 16, NEW & 0xFFFF
    if not cuts_only:
        print(f'# build id {bid}; 16/9 = {OLD:#010x}, {ratio} = {NEW:#010x}')
    text_mo, text = segs[0]
    cands = []   # (index of the first instruction, [(index, old word, new word) x 2], register)
    n = len(text) // 4
    words = struct.unpack_from(f'<{n}I', text, 0)
    used = set()
    for i, w in enumerate(words):
        if i in used or not (is_movz(w) or is_movk(w)): continue
        imm, hw = imm_hw(w); rd = w & 31
        if not ((hw == 0 and imm == olo) or (hw == 1 and imm == ohi)): continue
        for j in range(i + 1, min(i + 5, n)):
            v = words[j]
            if j in used or not (is_movz(v) or is_movk(v)) or v & 31 != rd: continue
            imm2, hw2 = imm_hw(v)
            first_lo = hw == 0 and imm == olo and hw2 == 1 and imm2 == ohi
            first_hi = hw == 1 and imm == ohi and hw2 == 0 and imm2 == olo
            # the first instruction is the MOVZ (it clears the register), the second the MOVK that completes the value (a MOVK first would be overwritten by the MOVZ)
            if not (first_lo or first_hi) or not (is_movz(w) and is_movk(v)): continue
            def rebuilt(word):   # the same instruction kind and register, the new half
                k, h = imm_hw(word)
                newimm = nlo if h == 0 else nhi
                return (movz if is_movz(word) else movk)(word & 31, newimm, h)
            cands.append((i, [(i, w, rebuilt(w)), (j, v, rebuilt(v))], rd)); used.update((i, j)); break
    if not cuts_only:
        print(f'\n## code: {len(cands)} MOVZ/MOVK pair(s) building 16/9 in a register')
    for k, (i, parts, rd) in enumerate(cands):
        base = text_mo + i * 4
        if cuts_only:
            for (o, ow, nw) in parts:
                print(f"    ({text_mo + o * 4:#x}, '{struct.pack('<I', ow).hex()}', '{struct.pack('<I', nw).hex()}', 'aspect candidate {k}: 16/9 → {ratio} (w{rd})'),")
            continue
        print(f'\n[{k}] main+{base:#x}  w{rd}')
        for (o, ow, nw) in parts:
            print(f"    ({text_mo + o * 4:#x}, '{struct.pack('<I', ow).hex()}', '{struct.pack('<I', nw).hex()}', 'aspect candidate {k}: 16/9 → {ratio} (w{rd})'),")
    # data segments: the float and the double
    if not cuts_only:
        want32 = struct.pack('<f', 16 / 9); want64 = struct.pack('<d', 16 / 9)
        print('\n## data: literal-pool values')
        found = 0
        for si, (mo, buf) in enumerate(segs[1:] if not raw else [], 1):
            for want, name in ((want32, 'float 16/9'), (want64, 'double 16/9')):
                p = 0
                while True:
                    p = buf.find(want, p)
                    if p < 0: break
                    if p % len(want) == 0:
                        found += 1; print(f'  main+{mo + p:#x}  {name}  (segment {si})')
                    p += 1
        if not found: print('  none')
        print('\n# Candidates are not labelled: the camera projection and the HUD layout are different sites. Patch one candidate at a time (CUTS in patch_nso.py, then the rewriter and a device build) and look at the picture:')
        print('# a projection hit makes the 3D scene wider / taller without squeezing; a layout hit moves HUD elements. docs/development.md ("Aspect ratio") has the workflow.')

if __name__ == '__main__': main()

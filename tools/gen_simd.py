#!/usr/bin/env python3
# Authoritatively derive the WASM SIMD (0xFD) opcode table: author the mnemonic
# list (wasm-tools validates spelling + operand types when it assembles), then
# read the real sub-opcodes back out of the assembled binary. Emits the
# instructions.inc SIMD rows.
import re, subprocess, sys, os

WT = os.path.expanduser(r"~/.cargo/bin/wasm-tools.exe")

MNEMONICS = """
v128.load v128.load8x8_s v128.load8x8_u v128.load16x4_s v128.load16x4_u
v128.load32x2_s v128.load32x2_u v128.load8_splat v128.load16_splat
v128.load32_splat v128.load64_splat v128.load32_zero v128.load64_zero v128.store
v128.load8_lane v128.load16_lane v128.load32_lane v128.load64_lane
v128.store8_lane v128.store16_lane v128.store32_lane v128.store64_lane
v128.const i8x16.shuffle i8x16.swizzle
i8x16.splat i16x8.splat i32x4.splat i64x2.splat f32x4.splat f64x2.splat
i8x16.extract_lane_s i8x16.extract_lane_u i8x16.replace_lane
i16x8.extract_lane_s i16x8.extract_lane_u i16x8.replace_lane
i32x4.extract_lane i32x4.replace_lane i64x2.extract_lane i64x2.replace_lane
f32x4.extract_lane f32x4.replace_lane f64x2.extract_lane f64x2.replace_lane
i8x16.eq i8x16.ne i8x16.lt_s i8x16.lt_u i8x16.gt_s i8x16.gt_u i8x16.le_s i8x16.le_u i8x16.ge_s i8x16.ge_u
i16x8.eq i16x8.ne i16x8.lt_s i16x8.lt_u i16x8.gt_s i16x8.gt_u i16x8.le_s i16x8.le_u i16x8.ge_s i16x8.ge_u
i32x4.eq i32x4.ne i32x4.lt_s i32x4.lt_u i32x4.gt_s i32x4.gt_u i32x4.le_s i32x4.le_u i32x4.ge_s i32x4.ge_u
i64x2.eq i64x2.ne i64x2.lt_s i64x2.gt_s i64x2.le_s i64x2.ge_s
f32x4.eq f32x4.ne f32x4.lt f32x4.gt f32x4.le f32x4.ge
f64x2.eq f64x2.ne f64x2.lt f64x2.gt f64x2.le f64x2.ge
v128.not v128.and v128.andnot v128.or v128.xor v128.bitselect v128.any_true
i8x16.abs i8x16.neg i8x16.popcnt i8x16.all_true i8x16.bitmask
i8x16.narrow_i16x8_s i8x16.narrow_i16x8_u
i8x16.shl i8x16.shr_s i8x16.shr_u
i8x16.add i8x16.add_sat_s i8x16.add_sat_u i8x16.sub i8x16.sub_sat_s i8x16.sub_sat_u
i8x16.min_s i8x16.min_u i8x16.max_s i8x16.max_u i8x16.avgr_u
i16x8.extadd_pairwise_i8x16_s i16x8.extadd_pairwise_i8x16_u
i16x8.abs i16x8.neg i16x8.q15mulr_sat_s i16x8.all_true i16x8.bitmask
i16x8.narrow_i32x4_s i16x8.narrow_i32x4_u
i16x8.extend_low_i8x16_s i16x8.extend_high_i8x16_s i16x8.extend_low_i8x16_u i16x8.extend_high_i8x16_u
i16x8.shl i16x8.shr_s i16x8.shr_u
i16x8.add i16x8.add_sat_s i16x8.add_sat_u i16x8.sub i16x8.sub_sat_s i16x8.sub_sat_u
i16x8.mul i16x8.min_s i16x8.min_u i16x8.max_s i16x8.max_u i16x8.avgr_u
i16x8.extmul_low_i8x16_s i16x8.extmul_high_i8x16_s i16x8.extmul_low_i8x16_u i16x8.extmul_high_i8x16_u
i32x4.extadd_pairwise_i16x8_s i32x4.extadd_pairwise_i16x8_u
i32x4.abs i32x4.neg i32x4.all_true i32x4.bitmask
i32x4.extend_low_i16x8_s i32x4.extend_high_i16x8_s i32x4.extend_low_i16x8_u i32x4.extend_high_i16x8_u
i32x4.shl i32x4.shr_s i32x4.shr_u
i32x4.add i32x4.sub i32x4.mul i32x4.min_s i32x4.min_u i32x4.max_s i32x4.max_u
i32x4.dot_i16x8_s
i32x4.extmul_low_i16x8_s i32x4.extmul_high_i16x8_s i32x4.extmul_low_i16x8_u i32x4.extmul_high_i16x8_u
i64x2.abs i64x2.neg i64x2.all_true i64x2.bitmask
i64x2.extend_low_i32x4_s i64x2.extend_high_i32x4_s i64x2.extend_low_i32x4_u i64x2.extend_high_i32x4_u
i64x2.shl i64x2.shr_s i64x2.shr_u
i64x2.add i64x2.sub i64x2.mul
i64x2.extmul_low_i32x4_s i64x2.extmul_high_i32x4_s i64x2.extmul_low_i32x4_u i64x2.extmul_high_i32x4_u
f32x4.ceil f32x4.floor f32x4.trunc f32x4.nearest f32x4.abs f32x4.neg f32x4.sqrt
f32x4.add f32x4.sub f32x4.mul f32x4.div f32x4.min f32x4.max f32x4.pmin f32x4.pmax
f64x2.ceil f64x2.floor f64x2.trunc f64x2.nearest f64x2.abs f64x2.neg f64x2.sqrt
f64x2.add f64x2.sub f64x2.mul f64x2.div f64x2.min f64x2.max f64x2.pmin f64x2.pmax
i32x4.trunc_sat_f32x4_s i32x4.trunc_sat_f32x4_u f32x4.convert_i32x4_s f32x4.convert_i32x4_u
i32x4.trunc_sat_f64x2_s_zero i32x4.trunc_sat_f64x2_u_zero
f64x2.convert_low_i32x4_s f64x2.convert_low_i32x4_u
f32x4.demote_f64x2_zero f64x2.promote_low_f32x4
""".split()

# type codes: v=v128 i=i32 I=i64 f=f32 d=f64
def lane_scalar(prefix):
    return {'i8x16':'i','i16x8':'i','i32x4':'i','i64x2':'I','f32x4':'f','f64x2':'d'}[prefix]

def sig(mnem):
    prefix, op = mnem.split('.', 1)
    ls = lane_scalar(prefix) if prefix != 'v128' else 'i'
    if op == 'const': return ('', 'v', 'v128const')
    if op == 'store': return ('iv', '', 'memarg')
    if op.startswith('store') and op.endswith('_lane'): return ('iv', '', 'memarg_lane')
    if op.startswith('load') and op.endswith('_lane'): return ('iv', 'v', 'memarg_lane')
    if op.startswith('load'): return ('i', 'v', 'memarg')
    if op == 'shuffle': return ('vv', 'v', 'bytes16')
    if op == 'swizzle': return ('vv', 'v', 'none')
    if op == 'splat': return (ls, 'v', 'none')
    if op.startswith('extract_lane'): return ('v', ls, 'lane')
    if op.startswith('replace_lane'): return ('v' + ls, 'v', 'lane')
    if op == 'not': return ('v', 'v', 'none')
    if op in ('and', 'andnot', 'or', 'xor'): return ('vv', 'v', 'none')
    if op == 'bitselect': return ('vvv', 'v', 'none')
    if op in ('any_true', 'all_true', 'bitmask'): return ('v', 'i', 'none')
    if op in ('shl', 'shr_s', 'shr_u'): return ('vi', 'v', 'none')
    if op in ('eq','ne','lt_s','lt_u','gt_s','gt_u','le_s','le_u','ge_s','ge_u','lt','gt','le','ge'):
        return ('vv', 'v', 'none')
    if op == 'q15mulr_sat_s': return ('vv', 'v', 'none')
    if op.startswith(('extend_','extadd_pairwise_','convert_','trunc_sat_','demote_','promote_')) \
            or op in ('abs','neg','popcnt','sqrt','ceil','floor','trunc','nearest'):
        return ('v', 'v', 'none')
    if op in ('add','sub','mul','div','min','max','pmin','pmax','avgr_u',
              'add_sat_s','add_sat_u','sub_sat_s','sub_sat_u','min_s','min_u','max_s','max_u') \
            or op.startswith(('narrow_','dot_','extmul_')):
        return ('vv', 'v', 'none')
    raise SystemExit('unknown op: ' + mnem)

def natural(op):
    if op.startswith(('load8x8','load16x4','load32x2')): return 3
    if op in ('load','store'): return 4
    m = re.search(r'(8|16|32|64)', op)
    return {'8':0,'16':1,'32':2,'64':3}[m.group(1)]

def pascal(mnem):
    return ''.join(w[:1].upper() + w[1:] for w in mnem.replace('.', '_').split('_'))

TYPE = {'v':'V128','i':'I32','I':'I64','f':'F32','d':'F64'}
PARAM = {'v':None,'i':'local.get 3','I':'local.get 4','f':'local.get 5','d':'local.get 6'}

def wat_body(mnem):
    prefix, op = mnem.split('.', 1)
    pops, pushes, shape = sig(mnem)
    lines, vidx = [], 0
    if shape == 'v128const':
        lines.append('v128.const i32x4 0 0 0 0')
    else:
        for t in pops:
            if t == 'v':
                lines.append(f'local.get {vidx}'); vidx += 1
            else:
                lines.append(PARAM[t])
        if shape == 'bytes16':
            lines.append(mnem + ' ' + ' '.join(str(i) for i in range(16)))
        elif shape == 'lane':
            lines.append(mnem + ' 0')
        elif shape == 'memarg_lane':
            lines.append(mnem + ' 0')       # lane immediate; natural memarg default
        else:
            lines.append(mnem)
    lines += ['drop'] * len(pushes)
    return '\n    '.join(lines)

# Build one function per op.
funcs = []
for m in MNEMONICS:
    funcs.append(f'  (func (param v128 v128 v128 i32 i64 f32 f64)\n    {wat_body(m)})')
wat = '(module\n  (memory 1)\n' + '\n'.join(funcs) + '\n)\n'

with open('C:/tmp/simd_all.wat', 'w') as f:
    f.write(wat)

# Assemble (this validates every mnemonic + operand types).
r = subprocess.run([WT, 'parse', 'C:/tmp/simd_all.wat', '-o', 'C:/tmp/simd_all.wasm'],
                   capture_output=True, text=True)
if r.returncode != 0:
    sys.stderr.write(r.stderr)
    raise SystemExit('wasm-tools parse failed')

data = open('C:/tmp/simd_all.wasm', 'rb').read()

# Minimal wasm reader to reach the code section and read each body's 0xFD sub-op.
def rdu(b, p):
    v = shift = 0
    while True:
        x = b[p]; p += 1
        v |= (x & 0x7f) << shift; shift += 7
        if not (x & 0x80): return v, p

p = 8
code = None
while p < len(data):
    sid = data[p]; p += 1
    size, p = rdu(data, p)
    if sid == 10:
        code = data[p:p + size]; break
    p += size

opcodes = []
q = 0
count, q = rdu(code, q)
for _ in range(count):
    bsize, q = rdu(code, q)
    body = code[q:q + bsize]; q += bsize
    i = body.find(0xFD)
    sub, _ = rdu(body, i + 1)
    opcodes.append(sub)

assert len(opcodes) == len(MNEMONICS), (len(opcodes), len(MNEMONICS))

# Emit instructions.inc rows.
out = []
for m, sub in zip(MNEMONICS, opcodes):
    prefix, op = m.split('.', 1)
    pops, pushes, shape = sig(m)
    name = pascal(m)
    shape_macro = {'none':'NONE','lane':'LANE','bytes16':'BYTES16','v128const':'BYTES16'}.get(shape)
    if shape == 'memarg': shape_macro = f'MEMARG({natural(op)})'
    if shape == 'memarg_lane': shape_macro = f'MEMARG_LANE({natural(op)})'
    eff = '(' + ', '.join(TYPE[t] for t in pops) + ') -> (' + ', '.join(TYPE[t] for t in pushes) + ')'
    out.append((name, f'WASMP_PFX(0xFD,{sub})', shape_macro, eff))

# Column-align for readability.
w1 = max(len(x[0]) for x in out)
w2 = max(len(x[1]) for x in out)
w3 = max(len(x[2]) for x in out)
with open('C:/tmp/simd_rows.inc', 'w') as f:
    for name, opc, shp, eff in out:
        f.write(f'WASMP_OP({name+",":<{w1+1}} {opc+",":<{w2+1}} {shp+",":<{w3+1}} {eff+",":<26} SIMD)\n')
print(f'generated {len(out)} SIMD rows; opcode range {min(opcodes)}..{max(opcodes)}')

# --- Also emit a C++ test that emits EVERY SIMD op through the library --------
PUSH = {'v':'fn.V128Const(z);','i':'fn.I32Const(0);','I':'fn.I64Const(0);',
        'f':'fn.F32Const(0);','d':'fn.F64Const(0);'}
def call(name, shape):
    if shape in ('bytes16','v128const'): return f'fn.{name}(z);'
    if shape == 'lane': return f'fn.{name}(0);'
    if shape == 'memarg_lane': return f'fn.{name}(MemArg{{}}, 0);'
    return f'fn.{name}();'  # none / memarg

blocks = []
names = []
for m in MNEMONICS:
    pops, pushes, shape = sig(m)
    name = pascal(m)
    names.append(name)
    body = ''.join('        ' + PUSH[t] + '\n' for t in pops)
    body += '        ' + call(name, shape) + '\n'
    body += ''.join('        fn.Drop();\n' for _ in pushes)
    blocks.append(f'    {{ Fn fn(mod, vsig);\n{body}        mod.DefineFunction(ids[{len(names)-1}], fn.Finish()); }}')

cpp = '''// simd_full.cpp — GENERATED by tools/gen_simd.py. Emits every SIMD instruction
// through the library into one module; the external wasm-tools leg validates it.
#define _CRT_SECURE_NO_WARNINGS
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <wasmp/wasmp.hpp>
using namespace wasmp;
using Fn = FunctionEmitter<policy::Trust>;
int main() {
    Features feat = Features::All();
    feat.memory64 = false;
    Module mod(feat);
    mod.Memory(MemLimits{.min = 1});
    TypeIdx vsig = mod.FuncTypeOf<void()>();
    std::vector<FuncIdx> ids;
    for (int i = 0; i < %d; ++i) ids.push_back(mod.DeclareFunction(vsig));
    const std::array<uint8_t, 16> z{};
%s
    auto bytes = mod.Assemble();
    if (const char* dir = std::getenv("WASMP_GOLDEN_DIR")) {
        std::string full = std::string(dir) + "/simd_full.wasm";
        if (FILE* f = std::fopen(full.c_str(), "wb")) {
            std::fwrite(bytes.data(), 1, bytes.size(), f); std::fclose(f);
        }
    }
    std::printf("emitted %%d SIMD ops, %%zu bytes", %d, bytes.size());
    return 0;
}
''' % (len(names), '\n'.join(blocks), len(names))
open("C:/Projects/WASMP!/tests/simd_full.cpp", "w", encoding="utf-8").write(cpp)
print(f"wrote simd_full.cpp with {len(names)} ops")

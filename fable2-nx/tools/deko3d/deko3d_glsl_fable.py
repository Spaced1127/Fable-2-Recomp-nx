#!/usr/bin/env python3
"""SPIRV-Cross Vulkan GLSL -> GLSL that uam takes, for Fable II's SDK shader
model (ReXGlue Xenos).

What changes:
- Vulkan `set = N,` qualifiers are removed (uam has one descriptor space).
- Separate `uniform texture2DArray t;` + `uniform sampler s;` pairs used only
  through `sampler2DArray(t, s)` become one combined sampler; each distinct
  (texture, sampler) pair gets its own binding, assigned sequentially.
- A uniform block declared with the `scalar` layout (the SDK's
  XeSystemConstants) becomes a raw `uint[]` std430 block, and every member
  access is rewritten to a typed read at the member's explicit byte offset.
  Mesa/uam cannot express scalar layout (vec3 at offset 124), but the bytes
  are the same: this preserves them exactly.
- Unused uniform blocks are dropped.
- `gl_VertexIndex`/`gl_InstanceIndex` become `gl_VertexID`/`gl_InstanceID`.

    python deko3d_glsl_fable.py in.vk.glsl out.glsl --stage vert|frag
"""
import argparse
import re

SAMPLER_CTOR = re.compile(
    r'\b((?:[iu]?sampler(?:1D|2D|3D|Cube)(?:Array|CubeArray|MS|MSArray)?))'
    r'\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*\)')
DECL = re.compile(
    r'^layout\([^)]*\)\s*uniform\s+(?:[iu]?texture[A-Za-z0-9]*|sampler)\s+[A-Za-z_]\w*\s*;\s*$',
    re.M)
BLOCK = re.compile(
    r'layout\(([^)]*)\)\s*uniform\s+([A-Za-z_]\w*)\s*\{(.*?)\}\s*([A-Za-z_]\w*)\s*;\s*\n',
    re.S)
MEMBER = re.compile(
    r'layout\(offset\s*=\s*(\d+)\)\s*([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*(?:\[(\d+)\])?\s*;')

WORDS = {
    'uint': 1, 'int': 1, 'float': 1,
    'vec2': 2, 'ivec2': 2, 'uvec2': 2,
    'vec3': 3, 'ivec3': 3, 'uvec3': 3,
    'vec4': 4, 'ivec4': 4, 'uvec4': 4,
}


class Unsupported(Exception):
    pass


def read_expr(kind, word_expr):
    if kind == 'uint':
        return f'dk_sys[{word_expr}]'
    if kind == 'int':
        return f'int(dk_sys[{word_expr}])'
    if kind == 'float':
        return f'dk_sys_f({word_expr})'
    n = WORDS[kind]
    if kind.startswith('uvec'):
        return f'dk_sys_u{n}({word_expr})'
    if kind.startswith('ivec'):
        return f'dk_sys_i{n}({word_expr})'
    if kind.startswith('vec'):
        return f'dk_sys_v{n}({word_expr})'
    raise Unsupported(f'unhandled member type {kind}')


def rewrite_scalar_block(text):
    parsed = []

    def collect(m):
        qualifiers, block_name, body, instance = m.groups()
        if 'scalar' not in [q.strip() for q in qualifiers.split(',')]:
            return m.group(0)
        members = []
        end_words = 0
        for mm in MEMBER.finditer(body):
            offset, kind, name, count = int(mm.group(1)), mm.group(2), mm.group(3), mm.group(4)
            if offset % 4:
                raise Unsupported(f'{block_name}.{name}: offset {offset} is not word-aligned')
            words = WORDS.get(kind)
            if not words:
                raise Unsupported(f'{block_name}.{name}: unhandled type {kind}')
            count = int(count) if count else 1
            members.append((name, kind, offset // 4, count, words))
            end_words = max(end_words, offset // 4 + words * count)
        parsed.append((block_name, instance, members, end_words))
        return ''

    text = BLOCK.sub(collect, text)
    if not parsed:
        return text
    if len(parsed) > 1:
        raise Unsupported('more than one scalar-layout block in a shader')
    block_name, instance, members, words = parsed[0]
    for name, kind, word, count, stride in members:
        if count > 1:
            pat = re.compile(r'\b%s\.%s\s*\[([^\]]+)\]' % (re.escape(instance), re.escape(name)))
            text2 = []
            pos = 0
            for am in pat.finditer(text):
                text2.append(text[pos:am.start()])
                text2.append(read_expr(kind, f'{word}u + {stride}u * uint({am.group(1).strip()})'))
                pos = am.end()
            text2.append(text[pos:])
            text = ''.join(text2)
        pat = re.compile(r'\b%s\.%s\b' % (re.escape(instance), re.escape(name)))
        text = pat.sub(read_expr(kind, f'{word}u'), text)
    helpers = [
        'float dk_sys_f(uint i) { return uintBitsToFloat(dk_sys[i]); }',
        'vec2 dk_sys_v2(uint i) { return vec2(dk_sys_f(i), dk_sys_f(i + 1u)); }',
        'vec3 dk_sys_v3(uint i) { return vec3(dk_sys_f(i), dk_sys_f(i + 1u), dk_sys_f(i + 2u)); }',
        'vec4 dk_sys_v4(uint i) { return vec4(dk_sys_f(i), dk_sys_f(i + 1u), dk_sys_f(i + 2u), dk_sys_f(i + 3u)); }',
        'uvec2 dk_sys_u2(uint i) { return uvec2(dk_sys[i], dk_sys[i + 1u]); }',
        'uvec3 dk_sys_u3(uint i) { return uvec3(dk_sys[i], dk_sys[i + 1u], dk_sys[i + 2u]); }',
        'uvec4 dk_sys_u4(uint i) { return uvec4(dk_sys[i], dk_sys[i + 1u], dk_sys[i + 2u], dk_sys[i + 3u]); }',
        'ivec2 dk_sys_i2(uint i) { return ivec2(dk_sys[i], dk_sys[i + 1u]); }',
        'ivec3 dk_sys_i3(uint i) { return ivec3(dk_sys[i], dk_sys[i + 1u], dk_sys[i + 2u]); }',
        'ivec4 dk_sys_i4(uint i) { return ivec4(dk_sys[i], dk_sys[i + 1u], dk_sys[i + 2u], dk_sys[i + 3u]); }',
    ]
    # std430 on a *uniform* block is invalid; a read-only shader storage block
    # has the same binding semantics and tight uint layout.
    decl = 'layout(binding = 0, std430) readonly buffer %s { uint dk_sys[%du]; };' % (block_name, words)
    lines = text.split('\n')
    insert_at = 0
    for i, line in enumerate(lines):
        if line.startswith('#version') or line.startswith('#extension') or \
           line.startswith('#define') or line.startswith('#if') or line.startswith('#else') or \
           line.startswith('#endif') or line.strip() == '':
            insert_at = i + 1
        else:
            break
    lines[insert_at:insert_at] = [decl] + helpers + ['']
    return '\n'.join(lines)


def rebind_shared_memory(text):
    """Give XeSharedMemory its own storage-buffer binding.

    SPIRV-Cross emits both XeSystemConstants and XeSharedMemory at binding 0
    (they live in different Vulkan descriptor sets). deko3d has one storage
    buffer namespace per stage, so the shared-memory mirror moves to binding 1.
    """
    out = []
    for line in text.split('\n'):
        if 'buffer XeSharedMemory' in line or re.search(
                r'layout\([^)]*\)\s*(?:restrict\s+)?(?:readonly\s+)?buffer\s+XeSharedMemory\b',
                line):
            line = re.sub(r'binding\s*=\s*\d+', 'binding = 1', line, count=1)
        out.append(line)
    return '\n'.join(out)


def convert(text, stage):
    text = re.sub(r'^#extension GL_EXT_scalar_block_layout\s*:\s*\w+\s*$', '',
                  text, flags=re.M)
    text = re.sub(r'\bset\s*=\s*\d+\s*,\s*', '', text)
    if stage == 'vert':
        text = text.replace('gl_VertexIndex', 'gl_VertexID')
        text = text.replace('gl_InstanceIndex', 'gl_InstanceID')

    text = rewrite_scalar_block(text)
    text = rebind_shared_memory(text)

    # Drop uniform blocks whose instance name is never referenced.
    def drop_unused_block(m):
        if re.search(r'\b%s\b' % re.escape(m.group(4)), text[m.end():]):
            return m.group(0)
        return ''

    text = BLOCK.sub(drop_unused_block, text)

    pairs = {}
    order = []

    def ctor(m):
        key = (m.group(1), m.group(2), m.group(3))
        if key not in pairs:
            pairs[key] = 'dk_%s_%s' % (m.group(2), m.group(3))
            order.append(key)
        return pairs[key]

    text = SAMPLER_CTOR.sub(ctor, text)

    declared = [(m.group(1), m.group(2)) for m in
                re.finditer(r'uniform\s+((?:[iu]?texture[A-Za-z0-9]*|sampler))\s+([A-Za-z_]\w*)\s*;', text)]
    text = DECL.sub('', text)
    bare = []
    for kind, name in declared:
        if re.search(r'\b%s\b' % re.escape(name), text):
            bare.append('%s %s' % (kind, name))
    if bare:
        raise Unsupported('texture/sampler used without a constructor: ' + ', '.join(bare[:6]))

    if order:
        decls = []
        for binding, (kind, tex, samp) in enumerate(order):
            decls.append('layout(binding = %d) uniform %s %s;' %
                         (binding, kind, pairs[(kind, tex, samp)]))
        lines = text.split('\n')
        insert_at = 0
        for i, line in enumerate(lines):
            if line.startswith('#version') or line.startswith('#extension') or \
               line.startswith('#define') or line.startswith('#if') or line.startswith('#else') or \
               line.startswith('#endif') or line.strip() == '':
                insert_at = i + 1
            else:
                break
        lines[insert_at:insert_at] = decls + ['']
        text = '\n'.join(lines)
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--stage', choices=('vert', 'frag'), required=True)
    a = ap.parse_args()
    text = open(a.src, encoding='utf-8').read()
    try:
        out = convert(text, a.stage)
    except Unsupported as e:
        raise SystemExit(f'unsupported: {e}')
    open(a.dst, 'w', encoding='utf-8', newline='\n').write(out)


if __name__ == '__main__':
    main()

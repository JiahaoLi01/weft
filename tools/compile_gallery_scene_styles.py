"""Offline adapter for the pinned Tailwind sheet. No browser/style capture.

Select utilities referenced by the original CSS/D3 component source, preserve
stylesheet order, resolve its fixed theme constants and arithmetic, and retain
the original keyframes. Unsupported syntax fails instead of inventing a style.
Normal builds consume checked-in outputs; this script never reads the live game.
"""
from pathlib import Path
import re, json, hashlib, ast, operator
ROOT = Path(__file__).resolve().parents[1]
sheet_path = ROOT / 'examples/crpg-gallery/native/gallery.css'
source_path = ROOT / 'tests/fixtures/crpg-gallery/UI/src/components/debug/TestGallery.tsx'
sheet = re.sub(r'/\*.*?\*/', '', sheet_path.read_text(encoding='utf-8'), flags=re.S)
source = source_path.read_text(encoding='utf-8')
source = source[:source.index('// ── D3 Interactive')]
classes = set(c for x in re.findall(r'className="([^"]*)"', source) for c in x.split())

def blocks(text):
    pos = 0
    while pos < len(text):
        start = text.find('{', pos)
        if start < 0: break
        # Top-level @layer declarations have semicolons before the first block.
        head = text[pos:start].split(';')[-1].strip()
        depth, end, quote = 1, start + 1, ''
        while depth:
            if end >= len(text): raise ValueError('unclosed block')
            c = text[end]
            if quote:
                if c == quote and text[end-1] != '\\': quote = ''
            elif c in "\"'": quote = c
            elif c == '{': depth += 1
            elif c == '}': depth -= 1
            end += 1
        yield head, text[start+1:end-1]
        pos = end

variables = {}
for k,v in re.findall(r'(--[\w-]+)\s*:\s*([^;{}]+);', sheet):
    if not k.startswith('--tw-'): variables.setdefault(k,v)
# Registered initial values are equivalent to these emitted preflight defaults.
variables['--tw-border-style'] = 'solid'
ops = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul, ast.Div: operator.truediv}
def arithmetic(s):
    def visit(n):
        if isinstance(n, ast.Constant) and isinstance(n.value, (int,float)): return n.value
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, ast.USub): return -visit(n.operand)
        if isinstance(n, ast.BinOp) and type(n.op) in ops: return ops[type(n.op)](visit(n.left),visit(n.right))
        raise ValueError('unsupported fixed arithmetic: '+s)
    return visit(ast.parse(s, mode='eval').body)

def normalize(v):
    for _ in range(20):
        if 'var(' not in v: break
        start=v.find('var(');end=start+4;depth=1
        while depth:
            if v[end]=='(':depth+=1
            elif v[end]==')':depth-=1
            end+=1
        args=v[start+4:end-1].split(',',1)
        replacement=variables.get(args[0].strip(),args[1].strip() if len(args)>1 else None)
        if replacement is None: raise ValueError('unknown variable: '+v)
        v=v[:start]+replacement+v[end:]
    if 'var(' in v: raise ValueError(v)
    v = re.sub(r'(-?[\d.]+)rem\b', lambda m: f'{float(m[1])*16:g}px', v)
    while 'calc(' in v:
        def calc(m):
            expr = m[1]
            if 'infinity' in expr: return '999999px'
            units = re.findall(r'px|%', expr)
            if len(set(units)) > 1: raise ValueError(expr)
            n = arithmetic(re.sub(r'px|%', '', expr))
            return f'{n:g}'+(units[0] if units else '')
        next_v = re.sub(r'calc\(([^()]*)\)', calc, v)
        if next_v == v: raise ValueError(v)
        v = next_v
    def mix(m):
        color, opacity = m[1], float(m[2])/100
        if len(color) == 4: color = '#'+''.join(c*2 for c in color[1:])
        rgb = [int(color[i:i+2],16) for i in (1,3,5)]
        return f'rgba({rgb[0]}, {rgb[1]}, {rgb[2]}, {opacity:g})'
    v = re.sub(r'color-mix\(in (?:srgb|oklab), (#[0-9a-f]+) ([\d.]+)%, transparent\)', mix, v)
    if 'color-mix(' in v or 'oklch(' in v: raise ValueError('color subset: '+v)
    return v

def declarations(body):
    # The nested @supports overrides are identical for a color+transparent mix.
    flat = body[:body.find('@supports')] if '@supports' in body else body
    if '{' in flat: raise ValueError('nested selected utility')
    result=[]
    for item in flat.split(';'):
        if not item.strip(): continue
        k,v=item.split(':',1)
        result.append((k.strip(),normalize(v.strip())))
    return result

rules=[]; found=set(); animations=[]
def walk(text):
    for head,body in blocks(text):
        if head.startswith('@layer'): walk(body)
        elif head.startswith('@keyframes'):
            # All original keyframes are generic input, including unused ones.
            animations.append(head+' {'+body+'}')
        elif head.startswith('.'):
            token = re.sub(r'\\(.)',r'\1',head[1:])
            if token not in classes: continue
            found.add(token)
            ds = declarations(body)
            expanded=[]
            for k,v in ds:
                pairs = {'margin-inline':['margin-left','margin-right'], 'margin-block':['margin-top','margin-bottom'],
                         'padding-inline':['padding-left','padding-right'], 'padding-block':['padding-top','padding-bottom']}
                for key in pairs.get(k,[k]): expanded.append((key,v))
            rules.append(head+' {'+' '.join(k+': '+v+';' for k,v in expanded)+'}')
walk(sheet)
if classes-found: raise ValueError('Missing original utilities: '+repr(classes-found))
out=ROOT/'examples/crpg-gallery'
css='/* Compiled from pinned Tailwind; explicit utility adapter, 16px root. */\n'+'\n'.join(rules)+'\n'
animation='\n'.join(animations)+'\n'
(out/'scene-utilities.css').write_text(css,encoding='utf-8')
(out/'scene-keyframes.css').write_text(animation,encoding='utf-8')
sha=lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
(out/'scene-style-manifest.json').write_text(json.dumps({'format':'gallery-scene-styles-v1','compiler':'tools/compile_gallery_scene_styles.py',
    'root_font_px':16,'input_sha256':{str(p.relative_to(ROOT)).replace('\\','/'):sha(p) for p in (sheet_path,source_path,Path(__file__))},
    'output_sha256':{str(p.relative_to(ROOT)).replace('\\','/'):sha(p) for p in (out/'scene-utilities.css',out/'scene-keyframes.css')},
    'utility_count':len(found),'utilities':sorted(found), 'adapter':'fixed theme/arithmetic/logical edges; transparent color mixtures use equivalent sRGB; no browser computed-style capture'},indent=2)+'\n',encoding='utf-8')
print(f'Compiled {len(found)} original utilities, {len(animations)} original keyframe blocks')

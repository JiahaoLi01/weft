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
source = source[:source.index('// ── Canvas 2D Particle')] + source[source.index('function D3CandlestickChart'):source.index('const WEBGL_TESTS')] + source[source.index('function FramerFadeIn'):source.index('// ═══════════════════════════════════════════════════════════\n// Lottie Tests')]
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
for name,body in re.findall(r'@property\s+(--[\w-]+)\s*\{([^{}]*)\}',sheet):
    m=re.search(r'initial-value\s*:\s*([^;]+)',body)
    if m: variables[name]=m[1].strip()
ops = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul, ast.Div: operator.truediv}
def arithmetic(s):
    def visit(n):
        if isinstance(n, ast.Constant) and isinstance(n.value, (int,float)): return n.value
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, ast.USub): return -visit(n.operand)
        if isinstance(n, ast.BinOp) and type(n.op) in ops: return ops[type(n.op)](visit(n.left),visit(n.right))
        raise ValueError('unsupported fixed arithmetic: '+s)
    return visit(ast.parse(s, mode='eval').body)

def oklch(m):
    import math
    parts=m[1].split()
    L=float(parts[0].rstrip('%')) / (100 if parts[0].endswith('%') else 1)
    C=float(parts[1]); H=float(parts[2])*math.pi/180
    a=C*math.cos(H);b=C*math.sin(H)
    l=(L+.3963377774*a+.2158037573*b)**3
    z=(L-.1055613458*a-.0638541728*b)**3
    q=(L-.0894841775*a-1.291485548*b)**3
    rgb=(4.0767416621*l-3.3077115913*z+.2309699292*q,-1.2684380046*l+2.6097574011*z-.3413193965*q,-.0041960863*l-.7034186147*z+1.707614701*q)
    def encode(x):
        x=max(0,min(1,x));return round(255*(12.92*x if x<=.0031308 else 1.055*x**(1/2.4)-.055))
    return '#' + ''.join(f'{encode(x):02x}' for x in rgb)

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
    v = re.sub(r'oklch\(([^()]*)\)', oklch, v)
    if 'color-mix(' in v or 'oklch(' in v: raise ValueError('color subset: '+v)
    return v

def declarations(body):
    # The nested @supports overrides are identical for a color+transparent mix.
    flat = body[:body.find('@supports')] if '@supports' in body else body
    if '{' in flat: raise ValueError('nested selected utility')
    result=[]
    saved=variables.copy()
    for k,v in re.findall(r'(--[\w-]+)\s*:\s*([^;{}]+);',flat):variables[k]=v.strip()
    for item in flat.split(';'):
        if not item.strip(): continue
        k,v=item.split(':',1)
        if k.strip().startswith('--tw-'):continue
        result.append((k.strip(),normalize(v.strip())))
    variables.clear();variables.update(saved)
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
            if body.strip().startswith('&:'):
                state,nested=next(blocks(body))
                head+=state[1:]
                if nested.strip().startswith('@media'):
                    media,nested=next(blocks(nested))
                    if media!='@media (hover: hover)':raise ValueError(media)
                body=nested
            ds = declarations(body)
            expanded=[]
            for k,v in ds:
                pairs = {'margin-inline':['margin-left','margin-right'], 'margin-block':['margin-top','margin-bottom'],
                         'padding-inline':['padding-left','padding-right'], 'padding-block':['padding-top','padding-bottom']}
                for key in pairs.get(k,[k]): expanded.append((key,v))
            rules.append(head+' {'+' '.join(k+': '+v+';' for k,v in expanded)+'}')
walk(sheet)
markers=set(re.findall(r"querySelector(?:All)?\(['\"]\.([\w-]+)['\"]\)",source))
if classes-found-markers: raise ValueError('Missing original utilities: '+repr(classes-found-markers))
out=ROOT/'examples/crpg-gallery'
css='/* Compiled from pinned Tailwind; explicit utility adapter, 16px root. */\n'+'\n'.join(rules)+'\n'
animation='\n'.join(animations)+'\n'
(out/'cluster-utilities.css').write_text(css,encoding='utf-8')
(out/'cluster-keyframes.css').write_text(animation,encoding='utf-8')
sha=lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
(out/'cluster-style-manifest.json').write_text(json.dumps({'format':'gallery-scene-styles-v1','compiler':'tools/compile_gallery_cluster_styles.py',
    'root_font_px':16,'input_sha256':{str(p.relative_to(ROOT)).replace('\\','/'):sha(p) for p in (sheet_path,source_path,Path(__file__))},
    'output_sha256':{str(p.relative_to(ROOT)).replace('\\','/'):sha(p) for p in (out/'cluster-utilities.css',out/'cluster-keyframes.css')},
    'utility_count':len(found),'utilities':sorted(found),'unstyled_selector_markers':sorted(classes-found), 'adapter':'fixed theme/arithmetic/logical edges; fixed OKLCH quantized to sRGB, hover/active mouse conditions; no browser computed-style capture'},indent=2)+'\n',encoding='utf-8')
print(f'Compiled {len(found)} original utilities, {len(animations)} original keyframe blocks')

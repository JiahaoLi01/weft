"""Offline adapter for the pinned Tailwind sheet. No browser/style capture.

Select utilities referenced by the original CSS/D3 component source, preserve
stylesheet order, resolve its fixed theme constants and arithmetic, and retain
the original keyframes. Unsupported syntax fails instead of inventing a style.
Normal builds consume checked-in outputs; this script never reads the live game.

A second output serves the nested tooltip page (examples/crpg-gallery/src/tooltip, tools/build_tooltip_page.mjs): its
utilities are those of every token of its sources that names one, and of every class name its code builds from such a
token by replacing a part of it, as Tailwind's candidate scan of the captured sources found them for gallery.css (a
built name is styled only where some source spells it), with the page's base rules.
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

# Base-layer (preflight) rules that the scene's UA sheet does not already give: replaced elements are blocks. The scene
# UA sheet has preflight's border-box, zero borders and no UA margins or heading sizes; other base rules are unused.
PREFLIGHT={'img, svg, video, canvas, audio, iframe, embed, object'}
def select(classes, base=frozenset(), rename={}, inert={}, dropped=None):
  """The sheet's rules for these class tokens, in sheet order, with the preflight rules and the named base rules;
  renamed base rules under their new selector, without the inert declarations (recorded in dropped)."""
  rules=[]; found=set(); animations=[]
  def walk(text):
    for head,body in blocks(text):
        if head.startswith('@layer'): walk(body)
        elif head in PREFLIGHT or head in base or head in rename:
            kept=[]
            for k,v in declarations(body):
                if k in inert and inert[k]==v:
                    dropped.append(rename.get(head,head)+' { '+k+': '+v+' }')
                    continue
                kept.append((k,v))
            rules.append(rename.get(head,head)+' {'+' '.join(k+': '+v+';' for k,v in kept)+'}')
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
                # Logical edges of a left-to-right horizontal page.
                pairs = {'margin-inline':['margin-left','margin-right'], 'margin-block':['margin-top','margin-bottom'],
                         'padding-inline':['padding-left','padding-right'], 'padding-block':['padding-top','padding-bottom'],
                         'inset-inline':['left','right'], 'inset-block':['top','bottom'],
                         'inset-inline-start':['left'], 'inset-inline-end':['right'],
                         'inset-block-start':['top'], 'inset-block-end':['bottom']}
                for key in pairs.get(k,[k]): expanded.append((key,v))
            rules.append(head+' {'+' '.join(k+': '+v+';' for k,v in expanded)+'}')
  walk(sheet)
  return rules, found, animations
rules, found, animations = select(classes)
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

# The nested tooltip page: every token of its sources, split as tools/build_gallery.mjs splits the candidates it gives
# Tailwind; the page's base rules (sizes of html, body and #root, its font and color; inert user-select). The base rule
# that only sets the cursor of enabled buttons is left out: its :not() is outside the scene's selectors and the cursor
# draws nothing.
page_sources = [ROOT/'tests/fixtures/crpg-gallery/UI/src/components/NestedTooltip.tsx',
                ROOT/'examples/crpg-gallery/src/tooltip/NestedTooltipDemo.tsx',
                ROOT/'examples/crpg-gallery/src/tooltip/SmokeBg.tsx']
page_tokens = set(t for f in page_sources for t in re.split(r'[\s"\'`<>=;{},()]+', f.read_text(encoding='utf-8')) if t)
# Class names the page's code builds by replacing the first occurrence of a part of a token, as String.replace does:
# ItemCard's and a tooltip's divider take the rarity's border class with border made bg, a keyword's accent bar its
# text color with text- made bg-. gallery.css styles those of them that some source spells.
PAGE_REPLACEMENTS = sorted(set(re.findall(r"\.replace\('([^']+)', '([^']+)'\)",
                                          ''.join(f.read_text(encoding='utf-8') for f in page_sources))))
if PAGE_REPLACEMENTS != [('border', 'bg'), ('text-', 'bg-')]: raise ValueError('page class replacements')
page_tokens |= {t.replace(a, b, 1) for t in page_tokens for a, b in PAGE_REPLACEMENTS if a in t}
PAGE_BASE = {'html, body, #root', 'input, textarea, [contenteditable], .selectable'}
OMITTED = {'button:not(:disabled), [role="button"]:not(:disabled)': 'sets only the cursor; :not() is outside the scene profile'}
# The page is the whole document: preflight's html rule gives the line height the page inherits. Its :host matches
# nothing in a document. Its declarations the scene does not have do nothing here at these values: no text inflation
# (desktop), no tabs (white-space collapses them), no tap highlight, and font settings at their initial normal.
PAGE_RENAME = {'html, :host': 'html'}
PAGE_INERT = {'-webkit-text-size-adjust': '100%', 'tab-size': '4', 'font-feature-settings': 'normal',
              'font-variation-settings': 'normal', '-webkit-tap-highlight-color': 'transparent'}
page_dropped = []
page_rules, page_found, page_animations = select(page_tokens, PAGE_BASE, PAGE_RENAME, PAGE_INERT, page_dropped)
if not any(r.startswith('html {') for r in page_rules) or len(page_dropped) != len(PAGE_INERT):
    raise ValueError('preflight html rule')
if len([r for r in page_rules if r.split(' {')[0] in PAGE_BASE]) != len(PAGE_BASE): raise ValueError('page base rules')
page_css = '/* Compiled from pinned Tailwind for the nested tooltip page; explicit utility adapter, 16px root. */\n' + '\n'.join(page_rules) + '\n'
(out/'tooltip-utilities.css').write_text(page_css, encoding='utf-8')
(out/'tooltip-keyframes.css').write_text('\n'.join(page_animations)+'\n', encoding='utf-8')
(out/'tooltip-style-manifest.json').write_text(json.dumps({'format':'gallery-scene-styles-v1','compiler':'tools/compile_gallery_cluster_styles.py',
    'root_font_px':16,'input_sha256':{str(p.relative_to(ROOT)).replace('\\','/'):sha(p) for p in (sheet_path,*page_sources,Path(__file__))},
    'output_sha256':{str(p.relative_to(ROOT)).replace('\\','/'):sha(p) for p in (out/'tooltip-utilities.css',out/'tooltip-keyframes.css')},
    'utility_count':len(page_found),'utilities':sorted(page_found),'built_class_replacements':PAGE_REPLACEMENTS,'base_rules':sorted(PAGE_BASE),'omitted_base_rules':OMITTED,'renamed_base_rules':PAGE_RENAME,'inert_declarations':page_dropped,
    'adapter':'tokens of the page sources as Tailwind candidates; fixed theme/arithmetic/logical edges; fixed OKLCH quantized to sRGB, hover/active mouse conditions'},indent=2)+'\n',encoding='utf-8')
print(f'Compiled {len(page_found)} utilities and {len(PAGE_BASE)} base rules for the nested tooltip page')

# The floating UI page (tools/build_floating_page.mjs): the classes its sources name (each className is a literal
# there), with the page's base rules (html, body and #root) and the theme's two font stacks its components name inline
# (var(--font-default), var(--font-subtitle)), which its html rule gives every element. Words of its comments are no
# classes of it: Tailwind's scan would style them too (visible, hidden, transform, filter), with properties outside the
# scene profile.
fixture = ROOT/'tests/fixtures/crpg-gallery/UI/src'
floating_sources = [fixture/'components/FloatingUIOverlay.tsx',
                    *sorted((fixture/'components/floating-ui').glob('*.tsx')),
                    ROOT/'examples/crpg-gallery/src/floating/AnchoredFloatingUIOverlay.tsx']
floating_text = ''.join(f.read_text(encoding='utf-8') for f in floating_sources)
floating_literals = re.findall(r'className="([^"]*)"', floating_text)
if len(floating_literals) != floating_text.count('className='): raise ValueError('a floating className that is no literal')
floating_tokens = set(c for x in floating_literals for c in x.split())
FLOATING_BASE = {'html, body, #root'}
floating_dropped = []
floating_rules, floating_found, floating_animations = select(floating_tokens, FLOATING_BASE, PAGE_RENAME, PAGE_INERT, floating_dropped)
if not any(r.startswith('html {') for r in floating_rules) or len(floating_dropped) != len(PAGE_INERT):
    raise ValueError('preflight html rule')
FLOATING_FONTS = {k: variables[k] for k in ('--font-default', '--font-subtitle')}
floating_rules.append('html {' + ' '.join(k + ': ' + v + ';' for k, v in FLOATING_FONTS.items()) + '}')
floating_css = '/* Compiled from pinned Tailwind for the floating UI page; explicit utility adapter, 16px root. */\n' + '\n'.join(floating_rules) + '\n'
(out/'floating-utilities.css').write_text(floating_css, encoding='utf-8')
(out/'floating-keyframes.css').write_text('\n'.join(floating_animations)+'\n', encoding='utf-8')
(out/'floating-style-manifest.json').write_text(json.dumps({'format':'gallery-scene-styles-v1','compiler':'tools/compile_gallery_cluster_styles.py',
    'root_font_px':16,'input_sha256':{str(p.relative_to(ROOT)).replace('\\','/'):sha(p) for p in (sheet_path,*floating_sources,Path(__file__))},
    'output_sha256':{str(p.relative_to(ROOT)).replace('\\','/'):sha(p) for p in (out/'floating-utilities.css',out/'floating-keyframes.css')},
    'utility_count':len(floating_found),'utilities':sorted(floating_found),'base_rules':sorted(FLOATING_BASE),'theme_fonts':FLOATING_FONTS,'renamed_base_rules':PAGE_RENAME,'inert_declarations':floating_dropped,
    'adapter':'tokens of the page sources as Tailwind candidates; fixed theme/arithmetic/logical edges; fixed OKLCH quantized to sRGB, hover/active mouse conditions'},indent=2)+'\n',encoding='utf-8')
print(f'Compiled {len(floating_found)} utilities and {len(FLOATING_BASE)} base rules for the floating UI page')

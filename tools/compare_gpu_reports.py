"""Check native linear color results and the separately captured browser behavior."""
from pathlib import Path
import argparse
import hashlib
import json
import math

ROOT = Path(__file__).resolve().parents[1]
# Independent numeric expectations: sRGB_encode(0.5)=188, encode(0.25)=137.
# Alpha/coverage stay linear. Browser measurements use encoded-space blending.
EXPECTED = {
    'white-half': ([188,188,188,255], [128,128,128,255]),
    'ordered-red-blue': ([137,0,188,255], [64,0,128,255]),
    'srgb-gray-input': ([128,128,128,255], [128,128,128,255]),
    'coverage-128': ([188,188,188,255], [128,128,128,255]),
    'parent-opacity-group': ([0,0,188,255], [0,0,128,255]),
    'premultiplied-image-edge': ([188,0,0,255], [127,0,0,255]),
    'actual-freetype-glyph': ([255,255,255,255], [255,255,255,255]),
    'glyph-exterior': ([0,0,0,255], [0,0,0,255]),
    'scissor-interior': ([255,0,0,255], [255,0,0,255]),
    'scissor-exterior': ([0,0,0,255], [0,0,0,255]),
}


def check_pixels(actual, expected, tolerance, label):
    if len(actual) != 4 or any(not isinstance(v,int) or not 0 <= v <= 255 for v in actual):
        raise SystemExit(f'Invalid RGBA8: {label}')
    if any(abs(a-b) > (0 if c==3 else tolerance) for c,(a,b) in enumerate(zip(actual,expected))):
        raise SystemExit(f'{label}: expected {expected}, actual {actual}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('native', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    reference = json.loads(args.reference.read_text(encoding='utf-8'))
    native = json.loads(args.native.read_text(encoding='utf-8'))
    for key,value in [('fixture','caseelephant-d3d11-color-v1'),('width',256),('height',128)]:
        if reference[key] != value or native[key] != value:
            raise SystemExit('Fixture identity/size mismatch')
    for key,path in [('html_sha256',ROOT/'tests/fixtures/gpu_color_reference.html'),
                     ('font_sha256',ROOT/'third_party/ahem/Ahem.ttf'),
                     ('dom_sha256',args.reference.with_suffix('.dom.html')),
                     ('png_sha256',args.reference.with_suffix('.png'))]:
        if hashlib.sha256(path.read_bytes()).hexdigest() != reference[key]:
            raise SystemExit('Reference inputs changed; recapture: '+str(path))
    if not native['passed'] or not native['actual_completion_query'] or native['color_contract'] != 'linear-srgb-premultiplied':
        raise SystemExit('Missing real GPU/color evidence')
    if native['direct_rtt_max_difference'] > 2 or native['native_textures_retired'] != 6:
        raise SystemExit('Parity or lifetime failure')
    if native['debug_layer'] and native['debug_warning_error_count'] != 0:
        raise SystemExit('D3D11 debug layer reported warnings/errors')
    for key in ('transparent_linear_direct','transparent_linear_rtt'):
        if len(native[key]) != 4 or any(not math.isfinite(a) or abs(a-b) >= 1e-5 for a,b in zip(native[key],[0,0.5,0,0.5])):
            raise SystemExit('Transparent premultiplied alpha failure')
    browser = {c['id']:c['rgba'] for c in reference['samples']}
    checks = {c['id']:c for c in native['checks']}
    if len(reference['samples']) != 10 or len(native['checks']) != 10 or set(browser) != set(EXPECTED) or set(checks) != set(EXPECTED):
        raise SystemExit('Missing/duplicate/extra color checks')
    rows = []
    for key,(linear,encoded) in EXPECTED.items():
        check_pixels(checks[key]['direct'],linear,2,key+' direct')
        check_pixels(checks[key]['rtt'],linear,2,key+' rtt')
        check_pixels(browser[key],encoded,1,key+' browser')
        rows.append({'id':key,'native':checks[key]['direct'],'browser':browser[key],
                     'intentional_color_difference':linear!=encoded})
    summary={'fixture':native['fixture'],'driver':native['driver'],'adapter':native['adapter'],
             'checks':rows,'native_contract_passed':True,'browser_behavior_passed':True,
             'browser_pixel_parity_promised':False,'matched_contract':True}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(summary,indent=2)+'\n',encoding='utf-8')
    print('PASS 10 native color/alpha expectations and independent browser samples; recorded intentional blend-space differences')


if __name__ == '__main__':
    main()

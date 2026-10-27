#!/usr/bin/env python3
"""Compose Porca's SVG assets from the supplied mark and bundled outlined lettering.

No packages or network access are required. PNG/ICO/ICNS distributions are
rasterized from porca_icon.svg; the SVGs remain the editable source assets.
"""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
BRAND = ROOT / 'resources/branding'
IMAGES = ROOT / 'resources/images'
NS = 'http://www.w3.org/2000/svg'
ET.register_namespace('', NS)


def element(name, **attrs):
    return ET.Element(f'{{{NS}}}{name}', {k.replace('_', '-'): str(v) for k, v in attrs.items()})


def canvas(width, height, title):
    root = element('svg', width=width, height=height, viewBox=f'0 0 {width} {height}')
    ET.SubElement(root, f'{{{NS}}}title').text = title
    return root


def place(root, filename, x, y, width, color=None, prefix=''):
    source = ET.parse(BRAND / filename).getroot()
    scale = width / float(source.get('width'))
    group = element('g', transform=f'translate({x} {y}) scale({scale:.8f})')
    for child in source:
        child = deepcopy(child)
        for node in child.iter():
            if color and node.tag.endswith('path'):
                node.set('fill', color)
            if prefix:
                for key, value in list(node.attrib.items()):
                    if key == 'id': node.set(key, prefix + value)
                    elif 'url(#' in value: node.set(key, value.replace('url(#', f'url(#{prefix}'))
        group.append(child)
    root.append(group)


def save(root, path):
    ET.indent(root, space='  ')
    path.write_bytes(ET.tostring(root, encoding='utf-8', xml_declaration=True) + b'\n')


def lockup(dark):
    root = canvas(660, 240, 'Porca — ' + ('dark' if dark else 'light') + ' background logo')
    place(root, 'porca-pig-source.svg', 8, 8, 224)
    ink = '#FAF7F2' if dark else '#1E1E1E'
    place(root, 'porca-wordmark.svg', 264, 70, 356, ink)
    return root


def splash(dark):
    root = canvas(640, 320, 'Porca startup — ' + ('dark' if dark else 'light'))
    bg, ink, line = ('#1E1E1E', '#FAF7F2', '#403D39') if dark else ('#FAF7F2', '#1E1E1E', '#DED8CF')
    root.append(element('rect', width=640, height=320, fill=bg))
    place(root, 'porca-pig-source.svg', 48, 24, 212)
    place(root, 'porca-wordmark.svg', 294, 86, 294, ink)
    root.append(element('path', d='M48 240H592', stroke=line, stroke_width=1, fill='none'))
    return root


def main():
    mark = ET.parse(BRAND / 'porca-pig-source.svg').getroot()
    mark.set('viewBox', '0 0 1024 960')
    mark.set('height', '960')
    save(mark, IMAGES / 'porca_mark.svg')
    icon = canvas(1024, 1024, 'Porca application icon')
    icon.append(element('rect', x=32, y=32, width=960, height=960, rx=210, fill='#1E1E1E'))
    place(icon, 'porca-pig-source.svg', 128, 138, 768)
    save(icon, IMAGES / 'porca_icon.svg')
    for dark in (False, True):
        suffix = 'dark' if dark else 'light'
        save(lockup(dark), IMAGES / f'porca_logo_{suffix}.svg')
        save(splash(dark), IMAGES / f'porca_splash_{suffix}.svg')
    # A wide, transparent lockup also gives the README a vector-first identity.
    save(lockup(False), BRAND / 'porca-lockup-light.svg')
    save(lockup(True), BRAND / 'porca-lockup-dark.svg')
    board = canvas(1408, 920, 'Porca brand assets: logos, icons and startup backgrounds')
    board.append(element('rect', width=1408, height=920, fill='#E8E2D9'))
    for x, dark in ((32, False), (736, True)):
        board.append(element('rect', x=x, y=32, width=640, height=230, rx=16, fill='#1E1E1E' if dark else '#FAF7F2'))
        group = element('g', transform=f'translate({x+16} 38) scale(0.92)')
        group.extend(list(lockup(dark))[1:])
        # Keep gradient IDs unique when composing both themes into one document.
        for node in group.iter():
            for key, val in list(node.attrib.items()):
                if key=='id': node.set(key, f'{x}-'+val)
                elif 'url(#' in val: node.set(key, val.replace('url(#', f'url(#{x}-'))
        board.append(group)
        group = element('g', transform=f'translate({x} 296)')
        group.extend(list(splash(dark))[1:])
        for node in group.iter():
            for key, val in list(node.attrib.items()):
                if key=='id': node.set(key, f's{x}-'+val)
                elif 'url(#' in val: node.set(key, val.replace('url(#', f'url(#s{x}-'))
        board.append(group)
    for x, size in ((48,240),(336,160),(544,96),(688,64),(800,32)):
        group = element('g', transform=f'translate({x} 648) scale({size/1024})')
        group.extend(deepcopy(list(icon))[1:])
        for node in group.iter():
            for key,val in list(node.attrib.items()):
                if key=='id': node.set(key,f'i{x}-'+val)
                elif 'url(#' in val: node.set(key,val.replace('url(#',f'url(#i{x}-'))
        board.append(group)
    save(board, BRAND / 'porca-brand-kit.svg')
    sources = ['porca-pig-source.svg', 'porca-wordmark.svg']
    manifest = {
        'source': 'porca-pig-source.svg',
        'sources': {name: hashlib.sha256((BRAND / name).read_bytes()).hexdigest() for name in sources},
        'source_note': 'User-supplied pig artwork, preserved unchanged. Earlier brand-board.svg retained as historical provenance.',
        'lettering': {'wordmark': 'Nunito 900', 'format': 'outlined paths', 'licenses': '../design-system/fonts/*-OFL.txt'},
        'palette': {'mark_coral':'#FE7674', 'ui_coral':'#FF7B7B', 'charcoal':'#1E1E1E', 'cream':'#FAF7F2'},
        'generator':'../../scripts/generate_brand_assets.py',
        'derived_assets':'../images/porca_{mark,icon,logo_*,splash_*}.svg',
        'splash': {'size':[640,320], 'native_footer': 'Version, loading status and progress are drawn by SplashScreen below y=240.'}
    }
    (BRAND/'asset-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__ == '__main__':
    main()

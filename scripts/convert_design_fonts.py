#!/usr/bin/env python3
"""Offline native font instances; requires fontTools and a local Node executable."""
import argparse
import subprocess
import sys
import types
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--node', required=True, help='Local Node executable for Brotli decompression')
args = parser.parse_args()
# FontTools' WOFF2 reader only needs decompression. Node provides Brotli locally.
brotli = types.ModuleType('brotli')
def decompress(data):
    return subprocess.run([args.node, '-e', "process.stdout.write(require('node:zlib').brotliDecompressSync(require('node:fs').readFileSync(0)))"], input=data, stdout=subprocess.PIPE, check=True).stdout
brotli.decompress = decompress
sys.modules['brotli'] = brotli
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

folder = Path(__file__).resolve().parents[1] / 'resources/design-system/fonts'
styles = {400:'Regular', 500:'Medium', 600:'SemiBold', 650:'Heading', 700:'Bold', 800:'ExtraBold', 900:'Black'}
for family, weights in [('Figtree',(400,500,600,650,700)), ('Nunito',(800,900)), ('JetBrainsMono',(400,500))]:
    source = TTFont(folder / (family + '-Variable.woff2'))
    for weight in weights:
        font = instantiateVariableFont(source, {'wght':weight}, inplace=False)
        font.flavor = None
        style = styles[weight]
        family_name = 'JetBrains Mono' if family == 'JetBrainsMono' else family
        records = {(r.platformID, r.platEncID, r.langID) for r in font['name'].names if r.nameID in (1,2,4,6)}
        names = {1:family_name, 2:style, 3:'PorcaStatic;'+family+'-'+style,
                 4:family_name+' '+style, 6:family+'-'+style, 16:family_name, 17:style}
        for name_id, text in names.items():
            for platform, encoding, language in records:
                font['name'].setName(text, name_id, platform, encoding, language)
        font.save(folder / (family+'-'+str(weight)+'.ttf'))

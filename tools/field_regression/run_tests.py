#!/usr/bin/env python3
"""Field-regression harness: actual production components, explicit lower HAL stubs.

No private book is required by CI. Optional --epub stays in a temporary local
folder and is never written into repository fixtures or printed as prose.
JPEGDEC must be the pinned dependency with scripts/jpegdec_patches applied.
"""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path, PurePosixPath
import subprocess
import tempfile
import zipfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--jpegdec', type=Path, required=True)
parser.add_argument('--epub', type=Path)
args = parser.parse_args()
if not (args.jpegdec / 'src/JPEGDEC.cpp').is_file():
    parser.error('--jpegdec must contain src/JPEGDEC.cpp')
ENV = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1'}
CXX = os.environ.get('CXX', 'g++')
FLAGS = [CXX, '-std=c++20', '-D__LINUX__', '-DNO_SIMD', '-O0', '-g', '-fno-exceptions', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
COMMON = ['tools/field_regression/loader_stubs', 'tools/field_regression/stubs', 'tools/rc2_hotfix_tests/stubs', 'lib/hal', 'tools/casper_stability/stubs', 'lib/Rivulet', 'lib/Utf8', 'lib/Memory', 'lib/Serialization', 'src', 'src/activities/reader', 'lib/JpegToBmpConverter', 'lib/GfxRenderer', 'lib/jpgd']
INCLUDES = ['-I'+str(ROOT / p) for p in COMMON]+['-I'+str(args.jpegdec/'src')]
SOURCES = [
    'src/activities/reader/ChapterLoader.cpp', 'lib/hal/HalStorage.cpp', 'lib/Epub/Epub/css/CssParser.cpp',
    'lib/Rivulet/ChapterIr.cpp', 'lib/Rivulet/HtmlToIr.cpp', 'lib/Rivulet/IrTokenizer.cpp',
    'lib/Rivulet/PageLayouter.cpp', 'lib/Rivulet/PageMap.cpp', 'lib/Rivulet/LaidOutPage.cpp',
    'lib/Rivulet/FontLadder.cpp', 'lib/Rivulet/RivuletEngine.cpp', 'lib/Utf8/Utf8.cpp',
    'lib/JpegToBmpConverter/JpegToBmpConverter.cpp', 'lib/GfxRenderer/BitmapHelpers.cpp',
    'lib/jpgd/jpgd.cpp', 'lib/jpgd/jpgd_spill.cpp',
    'tools/field_regression/chapter_pipeline.cpp', 'tools/field_regression/cover_pipeline.cpp',
    'tools/field_regression/jpeg_spill.cpp',
]

def run(cmd: list[str]) -> None:
    subprocess.run(cmd, check=True, env=ENV)

with tempfile.TemporaryDirectory(prefix='casper-field-') as temporary:
    work = Path(temporary)
    def compile_source(item: tuple[int,str]) -> tuple[str,str]:
        i, name = item; obj = work / f'{i}.o'
        run(FLAGS+INCLUDES+['-c',str(ROOT/name),'-o',str(obj)])
        return name,str(obj)
    with ThreadPoolExecutor(max_workers=3) as pool:
        objects = dict(pool.map(compile_source, enumerate(SOURCES)))
    decoder = work/'jpegdec.o'
    # JPEGDEC deliberately uses unaligned host word accesses. Keep ASan and all
    # other UBSan checks; actual Rivulet/HAL/jpgd objects keep alignment checks.
    run(FLAGS+INCLUDES+['-fno-sanitize=alignment','-c',str(args.jpegdec/'src/JPEGDEC.cpp'),'-o',str(decoder)])
    def link(name: str, names: list[str], extra: list[str]|None=None) -> Path:
        exe = work/name
        run(FLAGS+[objects[n] for n in names]+(extra or [])+['-o',str(exe)])
        return exe
    chapter = link('chapter_pipeline', SOURCES[:12]+['tools/field_regression/chapter_pipeline.cpp'])
    cover = link('cover_pipeline', ['lib/hal/HalStorage.cpp']+SOURCES[12:16]+['tools/field_regression/cover_pipeline.cpp'],[str(decoder)])
    spill = link('jpeg_spill', SOURCES[14:16]+['tools/field_regression/jpeg_spill.cpp'])
    fixtures = ROOT/'test/progressive_cover_jpeg/fixtures'
    run([str(spill),str(fixtures/'cover_progressive.jpg')])
    for name in ['cover_progressive.jpg','cover_progressive_gray.jpg','cover_baseline_gray.jpg']:
        run([str(cover),str(fixtures/name)])
    book = work/'synthetic';(book/'text').mkdir(parents=True);(book/'style').mkdir()
    (book/'style/book.css').write_text('.h{text-align:center;font-size:1.5em;font-weight:bold}.p{text-indent:1.2em;text-align:justify}.i{font-style:italic}',encoding='utf-8')
    manifest=work/'spines.txt'
    spines=[]
    for i in range(16):
        href=f'text/part{i}.xhtml';spines.append(href)
        paragraphs=''.join(f"<div class='p'>Paragraph {j}. Ordinary text <span class='i'>italic text</span> and plain text.</div>" for j in range(25))
        (book/href).write_text(f"<html><head><link rel='stylesheet' href='../style/book.css'></head><body><div class='h'>[{i+1}]</div>{paragraphs}</body></html>",encoding='utf-8')
    manifest.write_text('\n'.join(spines)+'\n',encoding='utf-8')
    run([str(chapter),str(book),str(manifest)])
    if args.epub:
        # Reject unsafe archive entries and decompression bombs in the optional
        # local input. Do not extract bundled fonts or copy the book to CI.
        private = work/'private';private.mkdir()
        with zipfile.ZipFile(args.epub) as z:
            if sum(i.file_size for i in z.infolist())>128*1024*1024:
                raise ValueError('Test EPUB exceeds 128 MiB expanded-size bound')
            for info in z.infolist():
                path=PurePosixPath(info.filename)
                if path.is_absolute() or '..' in path.parts:
                    raise ValueError('Unsafe EPUB path')
                if info.is_dir() or path.suffix.lower() in ('.ttf','.otf','.woff','.woff2'):continue
                dest=private/str(path);dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(z.read(info))
        container=ET.parse(private/'META-INF/container.xml')
        opf_name=next(node.attrib['full-path'] for node in container.iter() if node.tag.endswith('rootfile'))
        opf=ET.parse(private/opf_name);ns={'o':'http://www.idpf.org/2007/opf'}
        items={n.attrib['id']:n.attrib for n in opf.findall('.//o:manifest/o:item',ns)}
        base=PurePosixPath(opf_name).parent
        spines=[str(base/items[n.attrib['idref']]['href']) for n in opf.findall('.//o:spine/o:itemref',ns)]
        manifest.write_text('\n'.join(spines)+'\n',encoding='utf-8')
        run([str(chapter),str(private),str(manifest)])
        cover_item=next((i for i in items.values() if 'cover-image' in i.get('properties','').split()),None)
        if not cover_item:
            meta=next((n for n in opf.iter() if n.attrib.get('name')=='cover'),None)
            if meta is not None:cover_item=items.get(meta.attrib.get('content',''))
        if cover_item and cover_item.get('media-type')=='image/jpeg':
            image=private/str(base/cover_item['href'])
            run([str(spill),str(image)]);run([str(cover),str(image)])
    print('PASS field integration suite. No physical device or performance certification.',flush=True)

#!/usr/bin/env python3
"""Production XML/path parsers and TOC-selection policy; host capture sink.
Private EPUB is optional, never copied to the repository or printed as prose.
Requires g++ and libexpat development headers (Ubuntu: libexpat1-dev).
"""
import argparse,os,subprocess,tempfile,zipfile,posixpath
from pathlib import Path
import xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--epub',type=Path);a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='casper-nav-') as tmp:
    exe=Path(tmp)/'navigation'
    includes=['tools/navigation_regression/stubs','tools/rc2_hotfix_tests/stubs','lib/Epub','lib/Memory','lib/FsHelpers','lib/XmlParserUtils']
    sources=['tools/navigation_regression/navigation.cpp','lib/Epub/Epub/parsers/TocNavParser.cpp','lib/Epub/Epub/parsers/TocNcxParser.cpp','lib/FsHelpers/FsHelpers.cpp']
    subprocess.run(['g++','-std=c++20','-O0','-g','-fno-exceptions','-DCASPER_ALLOCATION_TESTING','-include','cstring','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']+['-I'+str(ROOT/i) for i in includes]+[str(ROOT/s) for s in sources]+['-lexpat','-o',str(exe)],check=True)
    args=[]
    if a.epub:
        with zipfile.ZipFile(a.epub) as z:
            opf=next(n.attrib['full-path'] for n in ET.fromstring(z.read('META-INF/container.xml')).iter() if n.tag.endswith('rootfile'))
            root=ET.fromstring(z.read(opf));base=posixpath.dirname(opf)
            items=[e.attrib for e in root.iter() if e.tag.endswith('}item')]
            nav=next(i['href'] for i in items if 'nav' in i.get('properties','').split())
            ncx=next(i['href'] for i in items if i.get('media-type')=='application/x-dtbncx+xml')
            # Only navigation resources, not book text or embedded fonts.
            for name,href in [('nav.xhtml',nav),('toc.ncx',ncx)]:
                path=posixpath.normpath(posixpath.join(base,href))
                if z.getinfo(path).file_size>256*1024:raise ValueError('Oversized navigation fixture')
                (Path(tmp)/name).write_bytes(z.read(path))
            assert posixpath.dirname(nav)==posixpath.dirname(ncx),'Fixture runner currently expects co-located navigation files'
            args=[str(Path(tmp)/'nav.xhtml'),str(Path(tmp)/'toc.ncx'),posixpath.dirname(posixpath.join(base,ncx))+'/']
    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'}
    subprocess.run([str(exe)]+args,check=True,env=env)

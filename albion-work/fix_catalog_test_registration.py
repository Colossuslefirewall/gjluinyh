from pathlib import Path
import hashlib,json,sys
R=Path(sys.argv[1]) if len(sys.argv)>1 else Path(r'C:\Users\admin\Desktop\Albion-Experimental-20261120')
S=R/'Source'/'WindowsProject1-Stable'
old=S/'tests/file_catalog_tests.cpp';new=S/'tests/file_catalog_windows.cpp'
if old.exists():
 assert not new.exists()
 old.rename(new)
for path in [R/'scripts/file-catalog-tests.cmd',R/'scripts/patch-discovery-generation.py']:
 text=path.read_text(encoding='utf-8-sig').replace('file_catalog_tests.cpp','file_catalog_windows.cpp');path.write_bytes(text.encode('utf-8'))
p=S/'tests/run-windows-tests.cmd';text=p.read_text(encoding='utf-8-sig')
old='stable-core-tests.exe\nif errorlevel 1 goto :failed\npopd'
new='''stable-core-tests.exe
if errorlevel 1 goto :failed
cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /I"%~dp0.." "%~dp0file_catalog_windows.cpp" /Fe:file-catalog-tests.exe /link user32.lib
if errorlevel 1 goto :failed
file-catalog-tests.exe
if errorlevel 1 goto :failed
popd'''
if new not in text:
 assert text.count(old)==1
 text=text.replace(old,new);p.write_bytes(text.encode('utf-8'))
p=R/'evidence/discovery-source-patch.json';files=json.loads(p.read_text(encoding='utf-8-sig'));files.pop('tests/file_catalog_tests.cpp',None)
for rel in ['tests/file_catalog_windows.cpp','tests/run-windows-tests.cmd']:
 files[rel]=hashlib.sha256((S/rel).read_bytes()).hexdigest()
p.write_text(json.dumps(files,indent=2),encoding='utf-8')
print('WINDOWS_CATALOG_REGISTRATION_PASS portable glob unchanged; catalog in Windows core suite')

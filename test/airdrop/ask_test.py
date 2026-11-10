from pathlib import Path
import plistlib, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='airdrop-ask-') as td:
 d=Path(td);binary=d/'test'
 subprocess.run(['c++','-std=c++17','-g','-fsanitize=address,undefined',str(root/'test/airdrop/ask_test.cpp'),'-o',str(binary)],check=True)
 cases=[({'Files':[{'FileName':'Book.epub','FileSize':123}]},'Book.epub\n1 123 1\n'),
 ({'Files':[{'FileName':'Кітап Żółć 📚.epub','FileSize':30*1024*1024}]},'Кітап Żółć 📚.epub\n1 31457280 1\n'),
 ({'Files':[{'FileName':'first.epub','FileSize':30*1024*1024+1},{'FileName':'second.epub'}]},'first.epub\n2 31457281 0\n'),
 ({'Files':[{'FileName':'unknown.epub'}]},'unknown.epub\n1 0 0\n'),
 ({'Files':[{'FileName':'x'*300}]},'x'*255+'\n1 0 0\n'),
 ({'Files':[]},None),({'Files':[{'FileSize':2}]},None),({'Files':[{'FileName':'x','FileSize':'wrong'}]},None),
 ({'Files':[{'FileName':'x'}]*65},None)]
 for i,(obj,expected) in enumerate(cases):
  path=d/f'{i}.plist';path.write_bytes(plistlib.dumps(obj,fmt=plistlib.FMT_BINARY))
  result=subprocess.run([str(binary),str(path)],capture_output=True,text=True)
  assert result.returncode==(2 if expected is None else 0),(i,result.stderr,result.stdout)
  if expected is not None:assert result.stdout==expected,(i,result.stdout,expected)
 print('Ask metadata: independent plistlib fixtures, Unicode, missing sizes, 30 MiB boundaries, long names, malformed structures, framing and cancellation passed under ASan/UBSan')

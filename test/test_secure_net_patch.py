"""The pinned SDK patch applies once, repeats safely, and rejects source drift."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('secure_patch',ROOT/'scripts/patch_secure_net.py')
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
patches=sorted((ROOT/'scripts/patches').glob('secure-net-*.patch'))
patch=''.join(p.read_text() for p in patches)
with tempfile.TemporaryDirectory(prefix='secure-net-patch-') as directory:
    project=Path(directory)
    (project/'scripts/patches').mkdir(parents=True)

    for part in patches:
        (project/'scripts/patches'/part.name).write_text(part.read_text())
    sdk=project/'freeink-sdk'
    sdk.mkdir()
    subprocess.run(['git','init','-q',str(sdk)],check=True)
    for line in patch.splitlines():
        if not line.startswith('diff --git a/'): continue
        name=line.split(' b/',1)[1]
        original=subprocess.run(['git','show','HEAD:'+name],cwd=ROOT/'freeink-sdk',capture_output=True)
        if original.returncode: continue  # new file supplied by the patch
        file=sdk/name
        file.parent.mkdir(parents=True,exist_ok=True)
        file.write_bytes(original.stdout)
    module.apply_patch(project)
    first={p.relative_to(sdk):p.read_bytes() for p in sdk.rglob('*') if p.is_file() and '.git' not in p.parts}
    module.apply_patch(project)
    assert first=={p.relative_to(sdk):p.read_bytes() for p in sdk.rglob('*') if p.is_file() and '.git' not in p.parts}
    # A partial or incompatible SDK must fail rather than silently skip the fix.
    target=next(line.split(' b/',1)[1] for line in patch.splitlines() if line.startswith('diff --git a/'))
    (sdk/target).write_text('incompatible SDK\n')
    try:
        module.apply_patch(project)
    except subprocess.CalledProcessError:
        pass
    else:
        raise AssertionError('incompatible SDK accepted')
print('SecureNet patch is idempotent and fails closed on SDK drift')

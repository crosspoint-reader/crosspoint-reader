#!/usr/bin/env python3
"""Compile the real indicator and renderer display methods against a recording display."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'lib/GfxRenderer/GfxRenderer.cpp').read_text()
methods = [
    'HalDisplay::RefreshMode GfxRenderer::applyPromotedRefresh',
    'void GfxRenderer::displayBuffer(', 'void GfxRenderer::displayBufferAsync(',
    'void GfxRenderer::waitRefreshComplete(', 'GfxRenderer::DisplayContent GfxRenderer::prepareOverlay(',
    'void GfxRenderer::displayGrayscaleBase(', 'bool GfxRenderer::displayGrayscaleBase(',
    'void GfxRenderer::displayGrayBuffer(', 'void GfxRenderer::copyGrayscaleLsbBuffers(',
    'void GfxRenderer::copyGrayscaleMsbBuffers(', 'void GfxRenderer::writeGrayscalePlaneStrip(',
    'void GfxRenderer::cleanupGrayscaleWithFrameBuffer(', 'void GfxRenderer::releaseFrameBufferForBuild(',
    'bool GfxRenderer::restoreFrameBufferAfterBuild(',
]
bodies = []
for signature in methods:
    start = source.index(signature)
    cursor = source.index('{', start)
    depth = 1
    cursor += 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    bodies.append(source[start:cursor])
source = (ROOT / 'src/activities/ActivityManager.cpp').read_text()
start = source.index('void ActivityManager::renderTaskLoop(')
cursor = source.index('{', start) + 1
depth = 1
while depth:
    depth += (source[cursor] == '{') - (source[cursor] == '}')
    cursor += 1
bodies.append(source[start:cursor])
with tempfile.TemporaryDirectory(prefix='edge-indicator-test-') as directory:
    root = Path(directory)
    for name in ('EdgeSwipeIndicator.cpp', 'EdgeSwipeIndicator.h'):
        shutil.copy2(ROOT / 'src/components' / name, root / name)
    for name in ('GfxRenderer.h', 'HalDisplay.h', 'HalFrontlight.h', 'UITheme.h',
                 'MappedInputManager.h', 'activities/Activity.h',
                 'components/themes/BaseTheme.h'):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#pragma once\n')
    (root / 'Logging.h').write_text(
        '#pragma once\ninline void hostLog(const char*, const char*, ...) {}\n'
        '#define LOG_DBG(...) hostLog(__VA_ARGS__)\n#define LOG_ERR(...) hostLog(__VA_ARGS__)\n')
    (root / 'GfxRendererDisplay.inc').write_text('\n\n'.join(bodies))
    exe = root / 'indicator-test'
    subprocess.run([
        os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-DFREEINK_CAP_TOUCH=1',
        '-I' + str(root), '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'src/components'),
        '-I' + str(ROOT / 'lib/Memory'),
        '-I' + str(ROOT / 'freeink-sdk/libs/assets/Icons/include'),
        str(ROOT / 'test/edge_swipe/EdgeSwipeIndicatorTest.cpp'), '-o', str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)

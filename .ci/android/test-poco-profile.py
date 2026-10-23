#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Compile the real profile/settings with an in-memory INI backend (requires fmt headers)."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def function(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fmt-include', type=Path)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    config = (root / 'src/frontend_common/config.cpp').read_text()
    methods = '\n'.join(function(config, 'void Config::' + name)
                        for name in ('ReadSettingGeneric', 'WriteSettingGeneric'))
    source = Path(__file__).with_suffix('.cpp').read_text().replace('// CONFIG_METHODS', methods)
    with tempfile.TemporaryDirectory(prefix='eden-poco-profile-') as directory:
        cpp, binary = Path(directory) / 'profile.cpp', Path(directory) / 'profile-test'
        cpp.write_text(source)
        flags = ['-std=c++20', '-O2', '-D__ANDROID__', '-DHAS_NCE',
                 '-DEDEN_POCOPHONE_X7_PRO', '-DFMT_HEADER_ONLY', '-DCANNOT_EXPLICITLY_INSTANTIATE',
                 '-I', str(root / 'src'), '-I', str(root / 'src/android/app/src/main/jni')]
        if args.fmt_include:
            flags += ['-I', str(args.fmt_include)]
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run([os.environ.get('CXX', 'c++'), *flags, str(cpp),
                        str(root / 'src/common/settings_common.cpp'), '-o', str(binary)], check=True)
        environment = os.environ.copy()
        environment.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
        subprocess.run([str(binary)], env=environment, check=True)


if __name__ == '__main__':
    main()

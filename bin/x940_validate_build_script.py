#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check Android build orchestration with fake Meson/Ninja; no compilation."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


FAKE_TOOL = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

tool = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ['X940_TEST_LOG'], 'a') as log:
    log.write(json.dumps({'tool': tool, 'args': args, 'cwd': os.getcwd()}) + '\n')
if tool == 'meson':
    if os.environ.get('X940_TEST_FAIL') == 'meson':
        sys.exit(12)
    positions = []
    skip = False
    for arg in args[1:]:
        if skip:
            skip = False
        elif arg == '--cross-file':
            skip = True
        elif not arg.startswith('-'):
            positions.append(arg)
    build = Path(positions[0])
    build.mkdir(parents=True, exist_ok=True)
    prefix = next(arg.split('=', 1)[1] for arg in args if arg.startswith('--prefix='))
    (build / 'fake-prefix.json').write_text(json.dumps(prefix))
else:
    if os.environ.get('X940_TEST_FAIL') == 'ninja':
        sys.exit(13)
    build = Path(args[args.index('-C') + 1])
    if 'install' in args:
        target = Path(json.loads((build / 'fake-prefix.json').read_text())) / 'lib/libvulkan_radeon.so'
        skip = os.environ.get('X940_TEST_FAIL') == 'missing-install'
    else:
        target = build / 'src/amd/vulkan/libvulkan_radeon.so'
        skip = os.environ.get('X940_TEST_FAIL') == 'missing-build'
    if not skip:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b'fake-library-for-script-test')
'''


def main():
    source = Path(__file__).resolve().parents[1] / 'build-xclipse940-android.sh'
    subprocess.run(['bash', '-n', str(source)], check=True)
    with tempfile.TemporaryDirectory(prefix='x940-build-script-') as temp:
        base = Path(temp)
        tools = base / 'tools'
        tools.mkdir()
        for name in ('meson', 'ninja'):
            tool = tools / name
            tool.write_text(FAKE_TOOL)
            tool.chmod(0o755)

        def run(name, *, prefix=True, api=None, failure=None, cross=True):
            folder = base / name
            repo = folder / 'repo with spaces'
            caller = folder / 'unrelated caller'
            repo.mkdir(parents=True)
            caller.mkdir()
            script = repo / source.name
            shutil.copyfile(source, script)
            cross_file = caller / 'cross file.ini'
            cross_file.write_text('[host_machine]\nsystem = "android"\n')
            chosen_prefix = caller / 'custom output' if prefix else repo / 'out-xclipse940-android'
            build = repo / 'build-xclipse940-android'
            caller_build = caller / 'build-xclipse940-android'
            sentinels = []
            for directory in (chosen_prefix, build, caller_build):
                directory.mkdir()
                sentinel = directory / 'unrelated-file.txt'
                sentinel.write_text('preserve this file')
                sentinels.append(sentinel)
            log = folder / 'calls.jsonl'
            env = dict(os.environ)
            for key in ('PREFIX', 'ANDROID_API', 'ANDROID_CROSS_FILE', 'DESTDIR'):
                env.pop(key, None)
            env.update(PATH=str(tools) + os.pathsep + env['PATH'], X940_TEST_LOG=str(log))
            if cross is True:
                env['ANDROID_CROSS_FILE'] = cross_file.name
            elif isinstance(cross, str):
                env['ANDROID_CROSS_FILE'] = cross
            if prefix:
                env['PREFIX'] = 'custom output'
            if api is not None:
                env['ANDROID_API'] = api
            if failure:
                env['X940_TEST_FAIL'] = failure
            result = subprocess.run(['bash', str(script)], cwd=caller, env=env,
                                    capture_output=True, text=True)
            for sentinel in sentinels:
                assert sentinel.read_text() == 'preserve this file', sentinel
            calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
            return result, calls, chosen_prefix, build, repo, cross_file

        for name, kwargs in [('custom-prefix', {}), ('default-prefix', {'prefix': False}),
                             ('custom-api', {'api': '35'})]:
            result, calls, prefix, build, repo, cross_file = run(name, **kwargs)
            assert result.returncode == 0, result.stderr
            assert [call['tool'] for call in calls] == ['meson', 'ninja', 'ninja']
            assert all(Path(call['cwd']) == repo for call in calls)
            args = calls[0]['args']
            assert args[:4] == ['setup', '--reconfigure', str(build), str(repo)]
            assert args[args.index('--cross-file') + 1] == str(cross_file)
            assert '--prefix=' + str(prefix) in args and '--libdir=lib' in args
            assert '-Dplatform-sdk-version=' + kwargs.get('api', '34') in args
            assert (prefix / 'lib/libvulkan_radeon.so').stat().st_size
        print('Caller-relative paths, spaces, API selection and prefix/build preservation: OK')

        for failure, expected_calls in [('meson', 1), ('ninja', 2),
                                        ('missing-build', 2), ('missing-install', 3)]:
            result, calls, *_ = run(failure, failure=failure)
            assert result.returncode != 0 and len(calls) == expected_calls, result
            assert 'Android RADV library:' not in result.stdout
        for name, kwargs in [('missing-cross', {'cross': False}),
                             ('nonexistent-cross', {'cross': 'not-there.ini'}),
                             ('invalid-api', {'api': 'abc'})]:
            result, calls, *_ = run(name, **kwargs)
            assert result.returncode != 0 and not calls
        print('Configure/build failures, missing artifacts and invalid inputs stop execution: OK')
    print('Build-script checks passed; real Android compilation remains pending.')


if __name__ == '__main__':
    main()

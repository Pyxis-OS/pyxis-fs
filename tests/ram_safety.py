# SPDX-License-Identifier: MPL-2.0
"""Small privileged-setup checks for the native comparison's target contract."""
import json
import os
import shutil


def run(launcher, binary, name, identity):
    results = []
    for filesystem in ('ext4', 'btrfs'):
        directory = launcher.SCRATCH / 'safety'
        directory.mkdir()
        loop = trace = None
        descriptors = []
        try:
            loop = launcher.OwnedLoop(directory / 'image')
            trace = launcher.BlockTrace(name, loop.device)
            root = loop.mount(filesystem, trace)
            os.chown(root, *identity)
            for path, flags in ((root, os.O_DIRECTORY), (loop.device, 0), (loop.image, 0)):
                descriptors.append(os.open(path, os.O_RDONLY | os.O_NOFOLLOW | flags))
            base = [str(binary), '--filesystem', 'native', '--root', str(root),
                    '--population', '32', '--case', 'compiler', '--durability', 'operation']

            def command(handles):
                arguments = base.copy()
                for option, fd in zip(('root', 'loop', 'backing'), handles):
                    arguments += [f'--native-{option}-fd', str(fd)]
                return arguments

            def refused(arguments, handles, reason):
                try:
                    launcher.capture(arguments, trace=trace, identity=identity, pass_fds=handles)
                except RuntimeError as error:
                    launcher.require(reason in str(error), 'unexpected refusal: ' + str(error))
                else:
                    raise RuntimeError('unsafe native target was accepted')
                launcher.require(not list(root.iterdir()), 'refusal changed the native root')

            refused(base, (), 'native mode requires launcher-held')
            mismatched = command(descriptors)
            mismatched[mismatched.index('--root') + 1] = str(launcher.SCRATCH / 'tmp')
            refused(mismatched, descriptors, 'native root must match the held directory')
            wrong = os.open(directory / 'wrong', os.O_CREAT | os.O_EXCL | os.O_RDONLY, 0o600)
            try:
                handles = descriptors[:2] + [wrong]
                refused(command(handles), handles, 'held loop must cover the verified RAM backing')
            finally:
                os.close(wrong)
            handles = [descriptors[0], loop.fd, descriptors[2]]
            refused(command(handles), handles, 'native descriptors must be open read-only')
            # A real persistent source file is held read-only: it can never receive
            # workload writes, and must not qualify as native RAM backing.
            wrong = os.open(__file__, os.O_RDONLY | os.O_NOFOLLOW)
            try:
                handles = descriptors[:2] + [wrong]
                refused(command(handles), handles, 'native loop backing must use the validated RAM mount')
            finally:
                os.close(wrong)
            text, _ = launcher.capture(command(descriptors), trace=trace,
                                       identity=identity, pass_fds=descriptors)
            record = next(json.loads(line) for line in reversed(text.splitlines()) if line.startswith('{'))
            launcher.require(record['verification']['contents_verified'] and record['verification']['files'] > 0, 'native oracle verification missing')
            for fd in descriptors:
                os.close(fd)
            descriptors.clear()
            loop.unmount(trace)
            trace.finish()
            results.append({'filesystem': filesystem, 'refusals': 5,
                            'verified_files': record['verification']['files'],
                            'submitted_bytes_including_format': trace.bytes})
        finally:
            for fd in descriptors:
                os.close(fd)
            try:
                if trace:
                    trace.close()
            finally:
                try:
                    if loop:
                        loop.close()
                finally:
                    if launcher.mount_at(directory / 'mounted') is None:
                        shutil.rmtree(directory)
    return results

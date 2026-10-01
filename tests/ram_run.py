#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Scoped Linux launcher for pyxis-fs contract tests and the small RAM baseline."""
import argparse
import errno
import fcntl
import json
import os
from pathlib import Path
import re
import resource
import selectors
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import time
import uuid

SCRATCH = Path('/run/pyxis-fs-ram')
BOUND_SOURCE = Path('/run/pyxis-fs-source')
SCRATCH_BYTES = 2 * 1024**3
LOCAL_JOB_BYTES = 4 * 1024**3
LOG_BYTES = 1024**2
TRACE_BYTES = 16 * 1024**2
SOURCE = Path(__file__).resolve().parent.parent
TRACE_ROOT = Path('/sys/kernel/tracing/instances')
LOOP_GET_STATUS64 = 0x4C05
LOOP_CONFIGURE = 0x4C0A
LOOP_CTL_GET_FREE = 0x4C82
LO_FLAGS_AUTOCLEAR = 4
LOOP_INFO_BYTES = 232
LOOP_CONFIG_BYTES = 304


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def mount_entries():
    return [line.split() for line in Path('/proc/self/mountinfo').read_text().splitlines()]


def mount_at(path):
    return next((entry for entry in reversed(mount_entries()) if entry[4] == str(path)), None)


def job_cgroup():
    membership = Path('/proc/self/cgroup').read_text().strip()
    require(membership.startswith('0::') and '\n' not in membership, 'cgroup v2 is required')
    actual = membership[3:]
    for entry in mount_entries():
        separator = entry.index('-')
        if entry[separator + 1] != 'cgroup2':
            continue
        root = entry[3].rstrip('/')
        # A cgroup namespace may report / while mountinfo retains a host root.
        if actual == root or actual.startswith(root + '/'):
            relative = actual[len(root):].lstrip('/')
        else:
            relative = actual.lstrip('/')
        require('..' not in Path(relative).parts, 'unresolvable cgroup namespace')
        group = Path(entry[4]) / relative
        try:
            processes = (group / 'cgroup.procs').read_text().splitlines()
        except OSError:
            continue
        if str(os.getpid()) in processes:
            return group
    raise RuntimeError('cannot resolve the actual job cgroup')


def preflight(ci_quick=False):
    entry = mount_at(SCRATCH)
    require(entry is not None, 'dedicated scratch mount is missing')
    separator = entry.index('-')
    mount_noswap = 'noswap' in entry[separator + 3].split(',')
    require(entry[separator + 1] == 'tmpfs' and (ci_quick or mount_noswap),
            'scratch must be tmpfs; mount-level noswap is required outside quick CI')
    fs = os.statvfs(SCRATCH)
    require(0 < fs.f_blocks * fs.f_frsize <= SCRATCH_BYTES and
            0 < fs.f_files <= 65536, 'scratch byte/inode limits exceed the agreed bounds')
    group = job_cgroup()
    memory = (group / 'memory.max').read_text().strip()
    require(memory.isdigit() and 0 < int(memory) < 2**64 - 1,
            f'job memory.max must be finite and positive (observed {memory})')
    swap = (group / 'memory.swap.max').read_text().strip()
    require(swap == '0', f'job swap is not disabled (memory.max={memory}, memory.swap.max={swap})')
    require((group / 'memory.swap.current').read_text().strip() == '0', 'job already has swapped memory')
    require(resource.getrlimit(resource.RLIMIT_CORE) == (0, 0), 'hard core-dump limit must be zero')
    return {'scratch_bytes': fs.f_blocks * fs.f_frsize, 'scratch_inodes': fs.f_files,
            'memory_max': int(memory), 'swap_max': 0, 'cgroup': str(group),
            'kernel': os.uname().release, 'mount_noswap': mount_noswap,
            'storage_mode': 'ci-quick' if ci_quick else 'strict'}


def interrupted(signum, _frame):
    # A second stop must not interrupt release of this job's owned resources.
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    raise InterruptedError(f'RAM validation interrupted by signal {signum}')


def child_limits():
    os.setsid()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def capture(command, *, trace=None, phases=False, timeout=600, env=None, result_listener=None):
    """Capture bounded diagnostics in RAM; stop rather than truncate a valid run."""
    process = subprocess.Popen(command, cwd=SOURCE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, preexec_fn=child_limits, env=env)
    output = bytearray()
    deadline = time.monotonic() + timeout
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    if trace:
        selector.register(trace.fd, selectors.EVENT_READ)
    os.set_blocking(process.stdout.fileno(), False)
    phase_counts = []
    result_received = False
    result_stream = None
    if result_listener is not None:
        selector.register(result_listener, selectors.EVENT_READ)
    try:
        while True:
            require(time.monotonic() < deadline, 'child exceeded its time budget')
            if trace:
                trace.drain()
            # Own all waitpid calls in phase mode; Popen.poll must not reap an
            # exit before buffered diagnostics reach EOF on a later iteration.
            if phases and process.returncode is None:
                pid, status = os.waitpid(process.pid, os.WNOHANG | os.WUNTRACED)
                if pid and os.WIFSTOPPED(status):
                    require(os.WSTOPSIG(status) == signal.SIGSTOP, 'unexpected child stop')
                    require(len(phase_counts) < 2, 'extra comparison phase stop')
                    if trace:
                        trace.drain()
                        trace.phase_times.append(time.monotonic())
                    phase_counts.append(trace.bytes if trace else 0)
                    os.kill(process.pid, signal.SIGCONT)
                elif pid:
                    process.returncode = os.waitstatus_to_exitcode(status)
            for key, _ in selector.select(0.02):
                if trace and key.fd == trace.fd:
                    trace.drain()
                    continue
                if key.fileobj is result_listener:
                    result_stream, _ = result_listener.accept()
                    credentials = result_stream.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
                    _, peer_uid, _ = struct.unpack('=iii', credentials)
                    require(peer_uid == 0, 'result peer is not the root job')
                    result_stream.setblocking(False)
                    selector.unregister(result_listener)
                    selector.register(result_stream, selectors.EVENT_READ)
                    result_received = True
                    continue
                chunk = os.read(key.fd, 65536)
                if chunk:
                    output.extend(chunk)
                    require(len(output) <= LOG_BYTES, 'child diagnostic output exceeded 1 MiB')
                else:
                    selector.unregister(key.fileobj)
            if not phases:
                process.poll()
            active_streams = [key for key in selector.get_map().values()
                              if key.fileobj is not result_listener and
                              (not trace or key.fd != trace.fd)]
            if process.returncode is not None and not active_streams:
                break
        if trace:
            trace.drain()
        require(process.returncode == 0,
                f'command failed ({process.returncode}): {command[0]}\n' +
                output[-8192:].decode(errors='replace'))
        require(result_listener is None or result_received, 'job produced no RAM socket result')
        if phases:
            require(len(phase_counts) == 2, 'missing comparison phase stops')
        return output.decode(), phase_counts
    finally:
        # Also kill descendants when the direct child already exited.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
        selector.close()
        process.stdout.close()
        if result_stream is not None:
            result_stream.close()


def trace_path(name):
    require(re.fullmatch(r'pyxis-fs-ram-[0-9a-f]{12}', name) is not None, 'invalid private trace name')
    return TRACE_ROOT / name


def cleanup_trace(name):
    path = trace_path(name)
    if path.exists():
        (path / 'tracing_on').write_text('0')
        (path / 'events/block/block_bio_queue/enable').write_text('0')
        path.rmdir()


class BlockTrace:
    """One private tracefs instance, one owned loop device, bounded streaming records."""
    def __init__(self, name, device):
        self.name = name
        self.path = trace_path(name)
        self.fd = None
        self.bytes = 0
        self.flushes = 0
        self.fua_events = 0
        self.discard_sectors = 0
        self.trace_bytes = 0
        self.pending = b''
        self.phase_times = []
        self.path.mkdir()
        try:
            device_id = os.stat(device).st_rdev
            self.major, self.minor = os.major(device_id), os.minor(device_id)
            event = self.path / 'events/block/block_bio_queue'
            form = (event / 'format').read_text()
            require('nr_sector' in form and 'rwbs' in form and
                    '"%d,%d %s %llu + %u [%s]"' in form, 'unsupported block trace event')
            (self.path / 'tracing_on').write_text('0')
            (self.path / 'buffer_size_kb').write_text('64')
            require(int((self.path / 'buffer_total_size_kb').read_text()) <= TRACE_BYTES // 1024,
                    'trace ring buffers exceed 16 MiB budget')
            encoded = (self.major << 20) | self.minor
            (event / 'filter').write_text(f'dev == {encoded}')
            (event / 'enable').write_text('1')
            self.fd = os.open(self.path / 'trace_pipe', os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            (self.path / 'tracing_on').write_text('1')
        except BaseException:
            self.close()
            raise

    def drain(self):
        while True:
            try:
                chunk = os.read(self.fd, 65536)
            except BlockingIOError:
                break
            if not chunk:
                break
            self.trace_bytes += len(chunk)
            require(self.trace_bytes <= TRACE_BYTES, 'trace exceeds 16 MiB budget; measurement invalid')
            self.pending += chunk
            while b'\n' in self.pending:
                line, self.pending = self.pending.split(b'\n', 1)
                text = line.decode(errors='strict')
                require('LOST' not in text.upper(),
                        'trace records lost; measurement invalid: ' + text[:256])
                if not text.strip() or text.startswith('#'):
                    continue
                match = re.search(r'block_bio_queue:\s+(\d+),(\d+)\s+(\S+)\s+(\d+)\s+\+\s+(\d+)\s+\[.*\]$', text)
                require(match is not None, 'unrecognized block trace record; measurement invalid')
                major, minor, flags, _, sectors = match.groups()
                require((int(major), int(minor)) == (self.major, self.minor), 'foreign device in trace')
                if 'D' in flags:
                    self.discard_sectors += int(sectors)
                elif 'W' in flags:
                    self.bytes += int(sectors) * 512
                # The leading F denotes preflush/flush. A trailing F on a
                # write denotes FUA and must not be counted as another flush.
                self.flushes += flags.startswith('F')
                self.fua_events += len(flags) > 1 and flags.endswith('F')
        statistics_files = list(self.path.glob('per_cpu/cpu*/stats'))
        require(statistics_files, 'trace loss statistics unavailable; measurement invalid')
        for statistics in statistics_files:
            values = dict(line.split(':', 1) for line in statistics.read_text().splitlines() if ':' in line)
            for field in ('overrun', 'commit overrun', 'dropped events'):
                require(field in values and int(values[field]) == 0,
                        f'trace {field} is missing/nonzero; measurement invalid')

    def finish(self):
        (self.path / 'tracing_on').write_text('0')
        self.drain()
        require(not self.pending, 'partial final trace record; measurement invalid')

    def close(self):
        try:
            if self.path.exists():
                (self.path / 'tracing_on').write_text('0')
        finally:
            if self.fd is not None:
                os.close(self.fd)
                self.fd = None
            cleanup_trace(self.name)


def loop_status(fd):
    raw = fcntl.ioctl(fd, LOOP_GET_STATUS64, bytes(LOOP_INFO_BYTES))
    device, inode, _, offset, size = struct.unpack_from('=QQQQQ', raw)
    flags = struct.unpack_from('=I', raw, 52)[0]
    return device, inode, offset, size, flags


class OwnedLoop:
    def __init__(self, image):
        self.image = image
        self.device = None
        self.fd = None
        self.mountpoint = image.parent / 'mounted'
        self.mountpoint.mkdir()
        backing = os.open(image, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_CLOEXEC, 0o600)
        control = None
        try:
            os.ftruncate(backing, 1024**3)
            self.identity = os.fstat(backing)
            require(self.identity.st_dev == os.stat(SCRATCH).st_dev, 'loop backing escaped RAM scratch')
            control = os.open('/dev/loop-control', os.O_RDWR | os.O_CLOEXEC)
            for _ in range(32):
                number = fcntl.ioctl(control, LOOP_CTL_GET_FREE)
                self.device = f'/dev/loop{number}'
                node_deadline = time.monotonic() + 1
                while True:
                    try:
                        self.fd = os.open(self.device, os.O_RDWR | os.O_CLOEXEC)
                        break
                    except FileNotFoundError:
                        require(time.monotonic() < node_deadline, 'loop device node did not appear')
                        time.sleep(0.01)
                try:
                    fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    config = bytearray(LOOP_CONFIG_BYTES)
                    struct.pack_into('=I', config, 0, backing)
                    struct.pack_into('=I', config, 8 + 52, LO_FLAGS_AUTOCLEAR)
                    # The held fd and AUTOCLEAR become effective atomically.
                    # Closing the job's last fd after unmount releases this
                    # device, including when the whole job is killed by OOM.
                    fcntl.ioctl(self.fd, LOOP_CONFIGURE, config)
                    self.verify()
                    self.initial_written_bytes = self.completed_written_bytes()
                    break
                except OSError as error:
                    if error.errno not in (errno.EBUSY, errno.EWOULDBLOCK):
                        raise
                    os.close(self.fd)
                    self.fd = None
                    # Autoclear and udev inspection can briefly retain the
                    # previous device. No filesystem mutation has begun here.
                    time.sleep(0.05)
            require(self.fd is not None, 'no exclusively available loop device')
        except BaseException:
            self.close()
            raise
        finally:
            if control is not None:
                os.close(control)
            os.close(backing)

    def verify(self):
        device, inode, offset, size, flags = loop_status(self.fd)
        current = os.stat(self.image, follow_symlinks=False)
        require(stat.S_ISREG(current.st_mode) and
                (device, inode) == (self.identity.st_dev, self.identity.st_ino) ==
                (current.st_dev, current.st_ino), 'loop backing identity changed')
        require(current.st_dev == os.stat(SCRATCH).st_dev, 'loop backing escaped RAM scratch')
        require(offset == size == 0 and flags == LO_FLAGS_AUTOCLEAR, 'loop configuration changed')

    def completed_written_bytes(self):
        fields = (Path('/sys/block') / Path(self.device).name / 'stat').read_text().split()
        require(len(fields) >= 11, 'loop completed-write statistics unavailable')
        return int(fields[6]) * 512

    def mounted_entry(self):
        entry = mount_at(self.mountpoint)
        if entry:
            separator = entry.index('-')
            # Btrfs exposes an anonymous st_dev, so verify the actual mount
            # source instead of comparing the directory's st_dev to the loop.
            require(entry[separator + 2] == self.device, 'mounted foreign device')
        return entry

    def mount(self, filesystem, trace):
        self.verify()
        if filesystem == 'ext4':
            command = ['mkfs.ext4', '-q', '-F', '-b', '4096', '-E',
                       'lazy_itable_init=0,lazy_journal_init=0,nodiscard', self.device]
            version, _ = capture(['mkfs.ext4', '-V'], trace=trace)
        else:
            command = ['mkfs.btrfs', '-q', '-f', '-K', '-s', '4096', '-n', '16384', self.device]
            version, _ = capture(['mkfs.btrfs', '--version'], trace=trace)
        self.profile = {'mkfs_command': command[:-1] + ['owned-1GiB-loop'],
                        'mkfs_version': version.strip(), 'mount_options': 'noatime,nodiscard'}
        capture(command, trace=trace)
        if filesystem == 'ext4':
            properties, _ = capture(['tune2fs', '-l', self.device], trace=trace)
            self.profile['features'] = next(
                (line.split(':', 1)[1].strip() for line in properties.splitlines()
                 if line.startswith('Filesystem features:')), None)
        else:
            properties, _ = capture(['btrfs', 'inspect-internal', 'dump-super', self.device], trace=trace)
            self.profile['feature_flags'] = [line.strip() for line in properties.splitlines()
                                            if line.startswith(('compat_flags', 'compat_ro_flags',
                                                                'incompat_flags'))]
        self.verify()
        capture(['mount', '-t', filesystem, '-o', 'noatime,nodiscard',
                 self.device, str(self.mountpoint)], trace=trace)
        entry = self.mounted_entry()
        require(entry is not None and entry[entry.index('-') + 1] == filesystem, 'mount verification failed')
        self.verify()
        work = self.mountpoint / 'work'
        work.mkdir()
        directory = os.open(self.mountpoint, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        trace.drain()
        return work

    def unmount(self, trace=None):
        if self.mounted_entry():
            self.verify()
            capture(['umount', str(self.mountpoint)], trace=trace)
            require(self.mounted_entry() is None, 'owned filesystem remains mounted')

    def close(self):
        try:
            self.unmount()
        finally:
            if self.fd is not None:
                os.close(self.fd)
                self.fd = None


def callback_bytes(phase):
    return sum(phase[group][kind] for group in ('user', 'orphan', 'drain')
               for kind in ('data_blocks', 'metadata_blocks')) * 4096


def baseline(binary, name):
    results = []
    for population in (32, 256):
        for case in ('small', 'large', 'overwrite', 'compiler'):
            for filesystem, durability in (('pyxis', 'operation'), ('ext4', 'operation'),
                                            ('btrfs', 'operation'), ('ext4', 'batch'), ('btrfs', 'batch')):
                directory = SCRATCH / 'case'
                directory.mkdir()
                loop = None
                trace = None
                try:
                    root = directory
                    if filesystem != 'pyxis':
                        loop = OwnedLoop(directory / 'image')
                        trace = BlockTrace(name, loop.device)
                        root = loop.mount(filesystem, trace)
                    command = [str(binary), '--filesystem', 'pyxis' if filesystem == 'pyxis' else 'native',
                               '--root', str(root), '--population', str(population), '--case', case,
                               '--durability', durability, '--phase-stops', 'yes']
                    environment = os.environ.copy()
                    if filesystem == 'pyxis':
                        environment['TMPDIR'] = str(directory)
                    text, counts = capture(command, trace=trace, phases=True, env=environment)
                    records = [json.loads(line) for line in text.splitlines() if line.startswith('{')]
                    require(records and 'measurement' in records[-1], 'comparison produced no summary')
                    record = records[-1]
                    record.update(filesystem=filesystem, durability=durability, population=population, case=case)
                    if loop:
                        loop.unmount(trace)
                        trace.finish()
                        completed = loop.completed_written_bytes() - loop.initial_written_bytes
                        require(completed == trace.bytes,
                                'submitted/completed loop write bytes differ; measurement invalid')
                        record.update(profile=loop.profile, completed_write_bytes=completed,
                                      submission_source='block_bio_queue on owned loop',
                                      preparation_submitted_bytes=counts[0],
                                      measured_submitted_bytes=trace.bytes - counts[0],
                                      phase_end_submitted_bytes=counts[1],
                                      after_end_submitted_bytes=trace.bytes - counts[1],
                                      after_end_seconds=time.monotonic() - trace.phase_times[1],
                                      measurement_with_teardown_seconds=time.monotonic() - trace.phase_times[0],
                                      total_submitted_bytes=trace.bytes,
                                      flush_events=trace.flushes, fua_events=trace.fua_events,
                                      discard_sectors=trace.discard_sectors, trace_bytes=trace.trace_bytes,
                                      sector_bytes=512, filesystem_block_bytes=4096,
                                      btrfs_node_bytes=16384 if filesystem == 'btrfs' else None)
                    else:
                        preparation_bytes = callback_bytes(record['preparation'])
                        measured_bytes = callback_bytes(record['measurement'])
                        record.update(preparation_submitted_bytes=preparation_bytes,
                                      measured_submitted_bytes=measured_bytes,
                                      total_submitted_bytes=preparation_bytes + measured_bytes,
                                      submission_source='core write callbacks')
                    results.append(record)
                finally:
                    try:
                        if trace:
                            trace.close()
                    finally:
                        try:
                            if loop:
                                loop.close()
                        finally:
                            # Never traverse a filesystem when unmount failed.
                            if mount_at(directory / 'mounted') is None:
                                shutil.rmtree(directory)
    return results


def inner(suite, name, ci_quick=False):
    require(not ci_quick or suite == 'check', 'CI storage mode is restricted to the quick suite')
    evidence = preflight(ci_quick)
    os.environ['TMPDIR'] = str(SCRATCH / 'tmp')
    Path(os.environ['TMPDIR']).mkdir()
    build = SCRATCH / 'build'
    runner = build / 'pyxis-fs-tests'
    comparison = build / 'pyxis-fs-compare'
    targets = ['all', str(runner)] + ([str(comparison)] if suite == 'baseline' else [])
    if suite == 'preflight':
        return {'safety': evidence, 'status': 'preflight-only'}
    capture(['make', '-j16', f'BUILD={build}', *targets])
    if suite == 'baseline':
        results = baseline(comparison, name)
    else:
        command = [str(runner), '--suite', 'pr' if suite == 'check' else 'extended']
        if ci_quick:
            command += ['--ram-mode', 'ci']
        if suite == 'extended':
            command += ['--seed', '1']
        output, _ = capture(command)
        results = {'test_output': output}
    group = Path(evidence['cgroup'])
    require((group / 'memory.swap.current').read_text().strip() == '0', 'swap appeared during run')
    evidence['memory_peak'] = (group / 'memory.peak').read_text().strip()
    evidence['memory_events'] = (group / 'memory.events').read_text().strip()
    require(not any(int(line.split()[1]) for line in evidence['memory_events'].splitlines()
                    if line.split()[0] in ('oom', 'oom_kill', 'oom_group_kill')), 'job suffered OOM; invalid run')
    return {'safety': evidence, 'results': results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', choices=('preflight', 'check', 'extended', 'baseline'), default='check')
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--ci-quick', action='store_true',
                        help='quick suite only: verify zero-swap cgroup instead of requiring mount noswap')
    parser.add_argument('--cleanup-trace', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--result-socket', help=argparse.SUPPRESS)
    parser.add_argument('--name', default='pyxis-fs-ram', help=argparse.SUPPRESS)
    args = parser.parse_args()
    require(not args.ci_quick or (args.inside and args.suite == 'check'),
            '--ci-quick requires --inside --suite check')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    if args.cleanup_trace:
        cleanup_trace(args.name)
        return
    if args.inside:
        if args.result_socket:
            trace_path(args.result_socket)
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.connect('\0' + args.result_socket)
            sys.stdout = connection.makefile('w', buffering=1)
            sys.stderr = sys.stdout
        result = json.dumps(inner(args.suite, args.name, args.ci_quick),
                            sort_keys=True, separators=(',', ':'))
        require(len(result.encode()) <= 65536, 'summary exceeds 64 KiB')
        print(result)
        return
    require(os.geteuid() == 0, 'run this reviewed launcher with sudo; no unprivileged storage fallback')
    require(':' not in str(SOURCE) and not any(character.isspace() for character in str(SOURCE)),
            'source path cannot be represented safely as a systemd bind')
    name = 'pyxis-fs-ram-' + uuid.uuid4().hex[:12]
    launcher = BOUND_SOURCE / 'tests/ram_run.py'
    cleanup = f'{sys.executable} {launcher} --cleanup-trace --name {name}'
    command = ['systemd-run', '--quiet', '--wait', '--collect', '--unit', name,
               '--service-type=exec', '-p', f'MemoryMax={LOCAL_JOB_BYTES}', '-p', 'MemorySwapMax=0',
               '-p', 'OOMPolicy=kill', '-p', 'LimitCORE=0', '-p', 'TasksMax=128',
               '-p', 'RuntimeMaxSec=1200', '-p', 'PrivateMounts=yes', '-p', 'TimeoutStopSec=30',
               '-p', 'ProtectSystem=strict', '-p', 'ProtectHome=read-only',
               '-p', f'BindReadOnlyPaths={SOURCE}:{BOUND_SOURCE}',
               '-p', f'TemporaryFileSystem={SCRATCH}:rw,nosuid,nodev,noswap,size={SCRATCH_BYTES},nr_inodes=65536',
               '-p', 'InaccessiblePaths=/tmp /var/tmp /dev/shm',
               '-p', 'ReadWritePaths=/sys/kernel/tracing/instances',
               '-p', f'ExecStopPost={cleanup}', '-p', 'Environment=PYTHONDONTWRITEBYTECODE=1',
               '-p', 'StandardOutput=null', '-p', 'StandardError=null',
               sys.executable, str(launcher), '--inside', '--suite', args.suite, '--name', name,
               '--result-socket', name]
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind('\0' + name)
    listener.listen(1)
    try:
        output, _ = capture(command, timeout=1250, result_listener=listener)
        print(output, end='')
    finally:
        listener.close()
        subprocess.run(['systemctl', 'stop', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, ValueError) as error:
        print(f'RAM validation refused/invalid: {error}', file=sys.stderr)
        sys.exit(1)

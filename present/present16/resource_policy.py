
import math
import os
from pathlib import Path

MIB = 1024 ** 2
GIB = 1024 ** 3
VERSION = 'auto_ram_v1'

def _cgroup_dirs():
    roots = [Path('/sys/fs/cgroup'), Path('/sys/fs/cgroup/memory'),
             Path('/sys/fs/cgroup/cpu')]
    found = set(roots)
    try:
        for line in Path('/proc/self/cgroup').read_text().splitlines():
            _, controllers, relative = line.split(':', 2)
            mounts = [roots[0]] if not controllers else [
                Path('/sys/fs/cgroup') / c for c in controllers.split(',')]
            for mount in mounts:
                current = mount / relative.lstrip('/')

                while current == mount or mount in current.parents:
                    found.add(current)
                    if current == mount:
                        break
                    current = current.parent
    except (OSError, ValueError):
        pass
    return sorted(found)

def available_memory():
    limits = []
    try:
        for line in Path('/proc/meminfo').read_text().splitlines():
            if line.startswith('MemAvailable:'):
                limits.append(int(line.split()[1]) * 1024)
    except (OSError, ValueError):
        pass
    for folder in _cgroup_dirs():
        for cap, used in [('memory.max', 'memory.current'),
                          ('memory.limit_in_bytes', 'memory.usage_in_bytes')]:
            try:
                value = (folder / cap).read_text().strip()
                if value != 'max':
                    limits.append(max(0, int(value) - int((folder / used).read_text())))
            except (OSError, ValueError):
                pass
    if not limits:
        raise SystemExit('Available RAM cannot be detected; Linux/WSL is required.')
    return min(limits)

def available_cores():
    cores = len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else (os.cpu_count() or 1)
    for folder in _cgroup_dirs():
        try:
            quota, period = (folder / 'cpu.max').read_text().split()
            if quota != 'max':
                cores = min(cores, max(1, int(math.ceil(int(quota) / int(period)))))
        except (OSError, ValueError, ZeroDivisionError):
            pass
        try:
            quota = int((folder / 'cpu.cfs_quota_us').read_text())
            period = int((folder / 'cpu.cfs_period_us').read_text())
            if quota > 0 and period > 0:
                cores = min(cores, max(1, int(math.ceil(quota / period))))
        except (OSError, ValueError):
            pass
    return cores

def layout(args, available=None, cores=None):

    available = available_memory() if available is None else int(available)
    cores = available_cores() if cores is None else int(cores)
    requested = min(args.cores or cores, cores)
    if requested < 3:
        raise SystemExit('At least 3 available CPU cores are required.')
    automatic = not args.memory_gib
    memory = min(int(args.memory_gib * GIB) if not automatic else available * 85 // 100, available)

    coordinator = max(256 * MIB, min(4 * GIB, memory // 20))
    transfer = bool(getattr(args, 'corridor_caps', []))
    workers = requested - 1
    minimum_table = (min(128 * MIB, int(args.exact_job_gib * GIB))
                     if args.exact_job_gib else 128 * MIB)
    minimum_table = max(16 * MIB, minimum_table)
    while workers >= 2:
        exact = max(1, workers // 3)
        heavy = workers if transfer else exact
        light = workers - heavy

        address = (memory - coordinator - light * 128 * MIB) // heavy
        table = max(0, (address - 192 * MIB) * 85 // 100)
        if args.exact_job_gib:
            table = min(table, int(args.exact_job_gib * GIB))
        if table >= minimum_table:
            break
        workers -= 1
    else:
        raise SystemExit('Available RAM is insufficient for two concurrent native jobs.')

    address = min(address, (table * 100 + 84) // 85 + 192 * MIB)
    result = dict(cores=workers + 1, workers=workers, exact_slots=exact,
                search_slots=workers - exact, table_bytes=table,
                address_bytes=address, native_budget_bytes=memory,
                coordinator_cores=1, resource_policy=VERSION,
                detected_available_bytes=available, available_cpu_cores=cores,
                requested_cpu_cores=requested, ram_limited_workers=workers < requested - 1,
                automatic_memory_fraction=.85 if automatic else None,
                coordinator_reserve_bytes=coordinator, heavy_job_slots=heavy,
                light_job_reserve_bytes=128 * MIB,
                reserved_peak_bytes=coordinator + light * 128 * MIB + heavy * address)
    if transfer:
        nodes, edges = corridor_limits(args, result)
        result.update(corridor_effective_max_nodes=nodes,
                      corridor_effective_max_edges=edges,
                      corridor_edges_follow_time_budget=not args.corridor_max_edges)
    for option in ('max_slices', 'max_pending_boxes'):
        if hasattr(args, option):
            result[option] = getattr(args, option)
    return result

def corridor_limits(args, resources):

    nodes = args.corridor_max_nodes or max(128, resources['table_bytes'] // 256)

    edges = args.corridor_max_edges or (2 ** 64 - 1)
    return int(nodes), int(edges)

def summary(resources):
    text = ('RAM 배분: 가용 {:.2f} GiB / 전체 예산 {:.2f} GiB / '
            '메모리 작업 {}개 기준, table {:.2f} GiB·process {:.2f} GiB/job').format(
                resources['detected_available_bytes'] / GIB,
                resources['native_budget_bytes'] / GIB,
                resources['heavy_job_slots'], resources['table_bytes'] / GIB,
                resources['address_bytes'] / GIB)
    if 'corridor_effective_max_nodes' in resources:
        text += ' | transfer {:,} states/row, edge {}'.format(
            resources['corridor_effective_max_nodes'],
            '시간예산 적용' if resources['corridor_edges_follow_time_budget']
            else '{:,}'.format(resources['corridor_effective_max_edges']))
    return text



import math

VERSION = 'balanced_pattern_search_v1'

def initialize(campaign, mass_factor):
    campaign._search_mass_factor = mass_factor
    policy = campaign.m.get('search_allocation')
    if policy is None:
        policy = dict(version=VERSION, endpoint_turn=0, generation_turn=0,
                      lane_turn=0, next_registration=0, group_cursors={},
                      dispatches={}, generation_by_active={},
                      started_at_seconds=campaign.elapsed(),
                      legacy_searches=sum(p.get('searches', 0)
                                          for p in campaign.patterns.values()))
        campaign.m['search_allocation'] = policy
    if policy['version'] != VERSION:
        raise ValueError('Unknown search allocation state: ' + policy['version'])

    for pid in sorted(campaign.patterns):
        register(campaign, campaign.patterns[pid])

def register(campaign, pattern):
    if 'allocation_order' in pattern:
        return
    policy = campaign.m['search_allocation']
    pattern['allocation_order'] = policy['next_registration']
    policy['next_registration'] += 1
    pattern['allocation_searches'] = {'1': 0, '2': 0, '4': 0}
    pattern['allocation_last_turn'] = -1
    campaign.dp.add(pattern['id'])

def generation_active(campaign):
    turn = campaign.m['search_allocation']['generation_turn']
    return campaign.a.counts[turn % len(campaign.a.counts)]

def expansion_turn(campaign):
    return campaign.m['search_allocation']['lane_turn'] % 4 == 0

def _bucket(campaign, pattern):
    active = pattern['active']
    return str(active) if active in campaign.a.counts else 'outside_initial_counts'

def _group(campaign, pool, role):
    labels = [str(x) for x in campaign.a.counts] + ['outside_initial_counts']
    buckets = {}
    for pattern in pool:
        buckets.setdefault(_bucket(campaign, pattern), []).append(pattern)
    cursor = campaign.m['search_allocation']['group_cursors'].get(role, 0)
    for offset in range(len(labels)):
        index = (cursor + offset) % len(labels)
        if labels[index] in buckets:
            return buckets[labels[index]], (index + 1) % len(labels), labels[index]
    raise ArithmeticError('Empty endpoint-search group')

def _quality(campaign):
    scores = {}
    for case in campaign.cases.values():
        exact = case.get('exact') or {}
        numerator = int(case.get('partial_numerator', '0'))
        screen = case.get('screen') or {}
        if exact.get('F') is not None:
            score = exact['F']
        elif numerator:
            score = campaign._search_mass_factor * case['active'] - math.log2(numerator)
        elif screen.get('F') is not None:

            score = screen['F'] + math.log2(max(1, screen.get('paths', campaign.a.screen_paths) /
                                               max(1, screen.get('ess', 1))))
        else:
            score = float(case['weight'])
        pid = case['pid']
        scores[pid] = min(scores.get(pid, math.inf), score)
    return scores

def endpoint_job(campaign):
    running = {}
    for job in campaign.active.values():
        if job['kind'] == 'search':
            pid = job['pid']
            running[pid] = running.get(pid, 0) + 1
    choices = [p for p in campaign.patterns.values()
               if running.get(p['id'], 0) < 3 and
               (p['active'] in campaign.a.counts or p.get('states'))]
    if not choices:
        return None
    for pattern in choices:
        register(campaign, pattern)
    turn = campaign.m['search_allocation']['endpoint_turn']
    requested = (1, 2, 4)[turn % 3]
    witnessed = [p for p in choices if p.get('states')]
    copies = requested if requested == 1 or witnessed else 1
    if copies == 1:
        fresh = [p for p in choices if not p.get('searches', 0)]
        pool = fresh or choices
        role = 'breadth'
        reason = 'first_pattern_search' if fresh else 'breadth_revisit'
        group, cursor, bucket = _group(campaign, pool, role)
        pattern = min(group, key=lambda p: (p.get('searches', 0),
                      p['allocation_last_turn'], p['allocation_order']))
    else:
        role = 'replica_' + str(copies)
        pool = witnessed

        targeted = (turn // 3) % 3 != 2
        if targeted:
            scores = _quality(campaign)
            pool = sorted(pool, key=lambda p: (scores.get(p['id'], math.inf),
                          p['allocation_last_turn'], p['allocation_order']))[:32]
        group, cursor, bucket = _group(campaign, pool, role)
        key = str(copies)
        pattern = min(group, key=lambda p: (p['allocation_searches'][key],
                      p['allocation_last_turn'], p['allocation_order']))
        reason = 'promising_replica' if targeted else 'replica_coverage'
    return dict(kind='search', pid=pattern['id'], pattern=pattern['pattern'],
                states=pattern.get('states'), copies=copies, seconds=campaign.a.search_seconds,
                allocation=dict(version=VERSION, role=role, reason=reason,
                    requested_copies=requested, bucket=bucket, next_cursor=cursor,
                    first_pattern_search=not pattern.get('searches', 0)))

def committed(campaign, job):

    policy = campaign.m['search_allocation']
    if job['kind'] in ('pattern', 'search', 'expand', 'seed'):
        policy['lane_turn'] += 1
    if job['kind'] == 'pattern':
        policy['generation_turn'] += 1
        key = str(job['active'])
        counts = policy['generation_by_active']
        counts[key] = counts.get(key, 0) + 1
    if job['kind'] != 'search':
        return
    info = job['allocation']
    pattern = campaign.patterns[job['pid']]
    copies = str(job['copies'])
    pattern['allocation_searches'][copies] += 1
    pattern['allocation_last_turn'] = policy['endpoint_turn']
    if 'replica_searches' not in pattern:

        pattern['legacy_unclassified_searches'] = pattern.get('searches', 0)
        pattern['replica_searches'] = {'1': 0, '2': 0, '4': 0}
    pattern['replica_searches'][copies] = pattern['replica_searches'].get(copies, 0) + 1
    pattern['searches'] = pattern.get('searches', 0) + 1
    campaign.dp.add(pattern['id'])
    policy['endpoint_turn'] += 1
    policy['group_cursors'][info['role']] = info['next_cursor']
    counts = policy['dispatches']
    for key in (info['role'], info['reason']):
        counts[key] = counts.get(key, 0) + 1
    if not job.get('states'):
        counts['without_saved_witness'] = counts.get('without_saved_witness', 0) + 1

def status(campaign):
    policy = campaign.m['search_allocation']
    groups = {}
    for pattern in campaign.patterns.values():
        label = _bucket(campaign, pattern)
        group = groups.setdefault(label, dict(patterns=0, searched=0, untried=0, witnessed=0))
        group['patterns'] += 1
        group['searched' if pattern.get('searches', 0) else 'untried'] += 1
        group['witnessed'] += bool(pattern.get('states'))
    running = {}
    for job in campaign.active.values():
        if job['kind'] == 'search':
            role = job.get('allocation', {}).get('role', 'legacy')
            running[role] = running.get(role, 0) + 1
    return dict(version=VERSION, since_seconds=policy['started_at_seconds'],
                endpoint_dispatches=policy['endpoint_turn'], dispatches=dict(policy['dispatches']),
                generation_dispatches_by_active=dict(policy['generation_by_active']),
                groups=groups, running=running,
                total_untried=sum(g['untried'] for g in groups.values()),
                total_searched=sum(g['searched'] for g in groups.values()),
                allocation_basis='submitted endpoint jobs, not CPU time or probability',
                legacy_searches_before_revision=policy['legacy_searches'])

def summary(value):
    counts = value['dispatches']
    return ('pattern 탐색 배분(수정 후 제출): 첫 탐색 {} / breadth 재방문 {} / replica 2개 {}·4개 {}'
            ' | 미탐색 {}개').format(counts.get('first_pattern_search', 0),
                counts.get('breadth_revisit', 0), counts.get('replica_2', 0),
                counts.get('replica_4', 0), value['total_untried'])

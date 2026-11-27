
from collections import OrderedDict
import heapq
import math
import time

import search_policy
import corridor_policy

VERSION = 'present_adaptive_transfer_v4'
ROLES = ('ensemble', 'ensemble', 'breadth', 'ensemble', 'expand', 'ensemble', 'ensemble', 'ensemble')

def initialize(campaign):
    m=campaign.m.setdefault('joint_search', dict(version=VERSION, turn=0, jobs={}, endpoint_jobs={},
        diagnostics={}, phase_history=[], since_seconds=campaign.elapsed()))
    if m['version'] not in ('present_joint_patterns_v1','present_blocked_joint_v2','present_collapsed_transfer_v3',VERSION):
        raise ValueError('Unknown PRESENT joint search version')
    m['version']=VERSION
    config=dict(active_cap=campaign.a.ensemble_max_active, source_hash=campaign.protocol['source_hash'],
                ensemble_seconds=campaign.a.ensemble_seconds)
    if not m['phase_history'] or m['phase_history'][-1]['configuration']!=config:
        m['phase_history'].append(dict(seconds=campaign.elapsed(),configuration=config))
        m['phase_diagnostics']={}
    m.setdefault('phase_diagnostics',{})
    campaign._joint_group_cache=None
    campaign._joint_group_clock=-math.inf
    campaign._pending_groups={}
    campaign._pending_group_heap=[]
    for cid in sorted(campaign.speculative,key=lambda key:campaign.cases[key]['first_retained_seconds']):
        pending_add(campaign,campaign.cases[cid])

def pending_add(campaign,case):
    e=case['endpoint']; group=campaign._pending_groups.setdefault(e,OrderedDict())
    group[case['id']]=None
    heapq.heappush(campaign._pending_group_heap,(-len(group),e))
    _compact_heap(campaign)

def pending_remove(campaign,case):
    e=case['endpoint']; group=campaign._pending_groups.get(e)
    if group is None:return
    group.pop(case['id'],None)
    if group:heapq.heappush(campaign._pending_group_heap,(-len(group),e))
    else:campaign._pending_groups.pop(e,None)
    _compact_heap(campaign)

def _compact_heap(campaign):
    if len(campaign._pending_group_heap)>4*len(campaign._pending_groups)+1024:
        campaign._pending_group_heap=[(-len(g),e) for e,g in campaign._pending_groups.items()]
        heapq.heapify(campaign._pending_group_heap)

def eviction_candidate(campaign):

    busy={job.get('cid') for job in campaign.active.values()}
    parked=[]; selected=None; visited=set()
    while campaign._pending_group_heap:
        count,e=heapq.heappop(campaign._pending_group_heap)
        group=campaign._pending_groups.get(e)
        if not group or -count!=len(group) or e in visited:continue
        visited.add(e); parked.append((count,e))
        candidates=[]
        for cid in list(group)[:64]:
            case=campaign.cases.get(cid)
            if case is not None and cid in campaign.speculative and campaign.unstarted(case) and cid not in busy:
                candidates.append(case)
        if candidates:
            selected=max(candidates,key=lambda c:(campaign.priority_score(c),-c['first_retained_seconds']))
            break
    for item in parked:heapq.heappush(campaign._pending_group_heap,item)
    return selected

def _known_F(case):
    exact=case.get('exact') or {}
    if exact.get('F') is not None:return exact['F']
    numerator=int(case.get('partial_numerator','0'))
    partial=3*case['active']-math.log2(numerator) if numerator else math.inf

    return min(float(case['weight']),partial)

def groups(campaign):
    now=time.monotonic()
    if campaign._joint_group_cache is not None and now-campaign._joint_group_clock<2:
        return campaign._joint_group_cache
    result={}
    for case in campaign.cases.values():
        e=case['endpoint'];g=result.setdefault(e,dict(endpoint=e,cases=[],eligible=[],F=math.inf,first=math.inf))
        g['cases'].append(case);g['first']=min(g['first'],case['first_retained_seconds'])
        if case['active']<=campaign.a.ensemble_max_active:g['eligible'].append(case)
    for g in result.values():
        fs=[_known_F(c) for c in g['cases'] if not corridor_policy.covered(campaign,c)]
        certificate=campaign.m.get('corridor',{}).get('certificates',{}).get(g['endpoint'])
        if certificate:fs.append(certificate['mass']['F'])
        if fs:
            base=min(fs);g['F']=base-math.log2(math.fsum(2.0**(base-f) for f in fs))
    campaign._joint_group_cache=result;campaign._joint_group_clock=now
    return result

def _select(campaign,pool,role):
    if not pool:return None
    m=campaign.m['joint_search'];turn=m['jobs'].get(role,0)
    ordered=sorted(pool,key=lambda g:(g['F'],g['first'],g['endpoint']))

    coverage=turn%3==2
    if not coverage:ordered=ordered[:32]
    stats=m['endpoint_jobs']
    return min(ordered,key=lambda g:(stats.get(g['endpoint'],{}).get(role,0),
        stats.get(g['endpoint'],{}).get(role+'_last',-1),g['first'],g['endpoint']))

def ensemble_job(campaign):
    running={}
    for job in campaign.active.values():
        if job['kind']=='ensemble':running[job['endpoint']]=running.get(job['endpoint'],0)+1
    pool=[g for g in groups(campaign).values() if g['eligible'] and running.get(g['endpoint'],0)<2]
    selected=_select(campaign,pool,'ensemble')
    if selected is None:return None
    copies=(1,2,4)[campaign.m['joint_search']['jobs'].get('ensemble',0)%3]

    ordered=sorted(selected['eligible'],key=lambda c:(_known_F(c),c['first_retained_seconds'],c['id']))
    cases=ordered[:8]
    rest=[case for case in ordered if case not in cases]
    count=campaign.m['joint_search']['endpoint_jobs'].get(selected['endpoint'],{}).get('ensemble',0)
    if rest:
        offset=(8*count)%len(rest)
        cases+=(rest[offset:]+rest[:offset])[:16-len(cases)]
    return dict(kind='ensemble',endpoint=selected['endpoint'],initial_trails=[c['states'] for c in cases],
        copies=copies,active=campaign.a.ensemble_max_active,seconds=campaign.a.ensemble_seconds,
        keep=min(128,max(64,campaign.a.keep_per_job)),
        allocation=dict(version=VERSION,role='ensemble',fixed_activity_pattern=False))

def breadth_job(campaign):
    busy={job.get('pid') for job in campaign.active.values() if job['kind']=='search'}
    pool=[p for p in campaign.patterns.values() if p['id'] not in busy and
          (p['active'] in campaign.a.counts or p.get('states'))]
    fresh=[p for p in pool if not p.get('searches',0)]
    if not pool:return None
    group,cursor,bucket=search_policy._group(campaign,fresh or pool,'breadth')
    pattern=min(group,key=lambda p:(p.get('searches',0),p['allocation_last_turn'],p['allocation_order']))
    return dict(kind='search',pid=pattern['id'],pattern=pattern['pattern'],states=pattern.get('states'),
        copies=1,seconds=min(campaign.a.search_seconds,60),
        allocation=dict(version=search_policy.VERSION,role='breadth',requested_copies=1,bucket=bucket,
            next_cursor=cursor,first_pattern_search=not pattern.get('searches',0),
            reason='first_pattern_search' if fresh else 'breadth_revisit'))

def expansion_job(campaign):
    busy={job.get('endpoint') for job in campaign.active.values() if job['kind']=='expand'}
    selected=_select(campaign,[g for g in groups(campaign).values() if g['endpoint'] not in busy],'expand')
    if selected is None:return None
    count=campaign.m['joint_search']['endpoint_jobs'].get(selected['endpoint'],{}).get('expand',0)
    cases=sorted(selected['cases'],key=lambda c:(_known_F(c),c['first_retained_seconds']))
    base=cases[count%min(len(cases),32)]
    return dict(kind='expand',endpoint=selected['endpoint'],cid=base['id'],states=base['states'],
        center=(0,4,8,16,24,32)[count%6],seconds=min(campaign.a.expansion_seconds,60))

def search_job(campaign):
    m=campaign.m['joint_search'];turn=m['turn']
    if campaign.a.corridor_caps and turn%5!=4:
        job=corridor_policy.job(campaign)
        if job is not None:return job
    has_seed=any(g['eligible'] for g in groups(campaign).values())
    seed_limit=max(1,min(2,campaign.r['search_slots']//8))
    if (not has_seed or turn%24==23) and len(campaign.running('seed'))<seed_limit:
        return dict(kind='seed',active=campaign.a.ensemble_max_active,
                    seconds=min(30,campaign.a.pattern_seconds))
    untried=sum(not p.get('searches',0) for p in campaign.patterns.values())
    quota=max(32,4*campaign.r['search_slots'])
    if (not has_seed or turn%8==2) and untried<quota and len(campaign.patterns)<campaign.a.max_patterns and not campaign.running('pattern'):
        return dict(kind='pattern',active=search_policy.generation_active(campaign),
                    seconds=campaign.a.pattern_seconds,keep=8)
    role=ROLES[turn%len(ROLES)]
    functions={'ensemble':ensemble_job,'breadth':breadth_job,'expand':expansion_job}
    for candidate in (role,'ensemble','breadth'):
        job=functions[candidate](campaign)
        if job is not None:return job
    return corridor_policy.job(campaign)

def discovery_capacity(campaign,search_running):

    used=campaign.used_cores()
    return (used<campaign.r['workers'] and
            (search_running<campaign.r['search_slots'] or used<campaign.r['workers']-1))

def evaluation_order(campaign,pending):
    if not pending:return []
    available={c['endpoint'] for c in pending}
    all_groups=groups(campaign)
    pool=[g for e,g in all_groups.items() if e in available]
    selected=_select(campaign,pool,'evaluation')
    primary=selected['endpoint'] if selected else None
    return sorted(pending,key=lambda c:(c['endpoint']!=primary,
        all_groups.get(c['endpoint'],{}).get('F',float(c['weight'])),
        int(c.get('exact_completed_job_cpu_seconds',0)//campaign.a.exact_slice_cpu_seconds),
        math.log2(1+(c.get('plan') or {}).get('operation_score',0)),
        campaign.priority_score(c),c['first_retained_seconds']))

def committed(campaign,job):
    m=campaign.m['joint_search'];kind=job['kind']
    role='evaluation' if kind in ('plan','exact','screen') else kind
    m['jobs'][role]=m['jobs'].get(role,0)+1
    e=job.get('endpoint')
    if e is None and job.get('cid') in campaign.cases:e=campaign.cases[job['cid']]['endpoint']
    if e is not None:
        stats=m['endpoint_jobs'].setdefault(e,{})
        stats[role]=stats.get(role,0)+1;stats[role+'_last']=sum(m['jobs'].values())
    if kind in ('ensemble','search','expand','seed','pattern','corridor'):m['turn']+=1
    if kind=='corridor':corridor_policy.committed(campaign,job)

def consume_diagnostics(campaign,job,result):
    d=result.get('diagnostics') or {};kind=job['kind']
    if kind not in ('ensemble','search','expand'):return
    for field in ('diagnostics','phase_diagnostics'):
        target=campaign.m['joint_search'].setdefault(field,{}).setdefault(kind,{})
        target['completed_jobs']=target.get('completed_jobs',0)+1
        if kind=='ensemble':
            for key in ('sweeps','hard_observations','soft_observations','hard_move_attempts','hard_moves_changed',
                        'hard_pattern_changes','hard_endpoint_changes','hard_pattern_transitions','hard_endpoint_transitions',
                        'recombination_attempts','recombinations','selected_families',
                        'block_attempts','block_moves','block_pattern_changes','boundary_block_attempts',
                        'boundary_block_moves','boundary_block_pattern_changes','boundary_block_endpoint_changes'):
                target[key]=target.get(key,0)+int(d.get(key,0))
            for key in ('collective_attempts','collective_moves','collective_pattern_changes'):
                target[key]=target.get(key,0)+int(d.get(key,0))
            pairs=d.get('swaps',[])
            target['swap_attempts']=target.get('swap_attempts',0)+sum(pair[0] for pair in pairs)
            target['swap_accepts']=target.get('swap_accepts',0)+sum(pair[1] for pair in pairs)
            if pairs:
                index=int(d.get('target_lane',len(pairs)))-1
                target['hard_link_attempts']=target.get('hard_link_attempts',0)+pairs[index][0]
                target['hard_link_accepts']=target.get('hard_link_accepts',0)+pairs[index][1]
        elif kind=='search':
            target['hard_lane_jobs']=target.get('hard_lane_jobs',0)+int(d.get('hard_lane_activated',False))
            target['observations']=target.get('observations',0)+int(d.get('observations',0))
        else:
            for key in ('legal_observations','swaps','swap_attempts'):
                target[key]=target.get(key,0)+int(d.get(key,0))

def status(campaign):
    m=campaign.m['joint_search'];counts=m['jobs']
    return dict(version=VERSION,active_cap=campaign.a.ensemble_max_active,
        target='Z_cap(endpoint)^m; independent variable patterns per replica; repeated trails allowed',
        jobs=dict(counts),diagnostics=m['diagnostics'],phase_diagnostics=m['phase_diagnostics'],phase_history=m['phase_history'],
        endpoint_groups=len(groups(campaign)),
        backlog=sum(not p.get('searches',0) for p in campaign.patterns.values()),
        visits_are_probability=False,equilibrium_certified=False)

def summary(value):
    d=value['phase_diagnostics'].get('ensemble',{})
    return ('보조 total-cap 탐색: {}개 작업 | endpoint 변경 {:,}회 | '
            '내부 단일-state update는 activity를 바꾸지 않음').format(
        value['jobs'].get('ensemble',0),d.get('boundary_block_endpoint_changes',0))

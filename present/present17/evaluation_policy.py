
import ensemble_policy
import corridor_policy

VERSION = 'fair_exact_v2'

def configuration(a):
    return dict(version=VERSION, family_slots=a.exact_family_slots,
                endpoint_slots=a.exact_endpoint_slots,
                slice_cpu_seconds=a.exact_slice_cpu_seconds,
                cooldown_seconds=a.exact_cooldown_seconds)

def initialize(c):
    config=configuration(c.a)
    m=c.m.setdefault('exact_schedule',dict(configuration=config,yields=0,phase_history=[]))
    new_phase=not m['phase_history'] or m['phase_history'][-1]['configuration']!=config
    if new_phase:
        m['phase_history'].append(dict(seconds=c.elapsed(),configuration=config))
    m['configuration']=config

    c.m['focus']=[]
    for case in c.cases.values():
        if new_phase or 'fair_phase_start_cpu' not in case:
            case['fair_phase_start_cpu']=case.get('exact_completed_job_cpu_seconds',0)
            c.dc.add(case['id'])
        if 'fair_slice_start_cpu' not in case:
            case['fair_slice_start_cpu']=case.get('exact_completed_job_cpu_seconds',0)
            c.dc.add(case['id'])

def eligible(c,case,now):
    if case['exact'] is not None or corridor_policy.covered(c,case):return False
    start=case.setdefault('fair_slice_start_cpu',case.get('exact_completed_job_cpu_seconds',0))
    used=case.get('exact_completed_job_cpu_seconds',0)-start
    if used>=c.a.exact_slice_cpu_seconds:
        case['fair_slice_start_cpu']=case.get('exact_completed_job_cpu_seconds',0)
        case['fair_resume_at']=now+c.a.exact_cooldown_seconds
        c.m['exact_schedule']['yields']+=1;c.dc.add(case['id'])
    return max(case.get('next_exact_at',0),case.get('fair_resume_at',0))<=now

def next_job(c):
    now=c.elapsed()
    pending=[case for case in c.cases.values() if eligible(c,case,now)]
    if not pending:return None
    c.m['evaluation_counter']+=1;tick=c.m['evaluation_counter']
    active=list(c.active.values());by_case={};by_endpoint={}
    for job in active:
        if job['kind'] not in ('plan','screen','exact'):continue
        cid=job.get('cid');case=c.cases.get(cid)
        if case is None:continue
        by_case[cid]=by_case.get(cid,0)+1
        e=case['endpoint'];by_endpoint[e]=by_endpoint.get(e,0)+1
    pending=[case for case in pending
             if by_endpoint.get(case['endpoint'],0)<c.a.exact_endpoint_slots]
    if not pending:return None
    ranked=ensemble_policy.evaluation_order(c,pending)

    ready=[case for case in ranked if case.get('plan') and case.get('frontier')
           and by_case.get(case['id'],0)<c.a.exact_family_slots]
    unplanned=[case for case in ranked if not case.get('plan')
               and not by_case.get(case['id'],0) and case.get('plan_attempts',0)<2]
    plan_limit=max(1,c.r['exact_slots']//3)

    def plan_job():
        if not unplanned or len(c.running('plan'))>=plan_limit:return None
        case=unplanned[0]
        case['plan_attempts']=case.get('plan_attempts',0)+1;c.dc.add(case['id'])
        return dict(kind='plan',cid=case['id'],states=case['states'],
                    table_bytes=c.r['table_bytes'],address_bytes=c.r['address_bytes'],
                    leaf_operations=c.a.leaf_operations,max_frontier=c.a.chunk_frontier,
                    seconds=c.a.plan_seconds)

    if not ready or tick%4==1:
        job=plan_job()
        if job:return job

    if tick%16==0 and len(c.running('screen'))<max(1,c.r['exact_slots']//8):
        for case in ranked[:64]:
            if (case.get('screen_attempts',0)<2 and not by_case.get(case['id'],0)
                    and (case.get('screen') or {}).get('ess',0)<8):
                case['screen_attempts']=case.get('screen_attempts',0)+1;c.dc.add(case['id'])
                return dict(kind='screen',cid=case['id'],pattern=c.patterns[case['pid']]['pattern'],
                            states=case['states'],paths=c.a.screen_paths,steps=c.a.screen_steps,
                            seconds=c.a.screen_seconds)

    rank={case['id']:i for i,case in enumerate(ranked)}
    ready.sort(key=lambda case:(by_case.get(case['id'],0),
        rank[case['id']]))
    for case in ready:
        busy={j['box_id'] for j in c.running('exact',case['id'])}
        boxes=[(key,box) for key,box in case['frontier'].items() if key not in busy]
        if not boxes:continue
        if len(case['frontier'])>=c.a.max_pending_boxes:
            case['last_resource_reason']='FRONTIER_STORAGE_LIMIT'
            case['next_exact_at']=c.a.wall_hours*3600;c.dc.add(case['id']);continue
        key,box=max(boxes,key=lambda kv:(len(kv[1]),-sum(bin(int(m,16)).count('1') for v,m in kv[1])))
        attempts=case['chunk_attempts'].get(key,0)
        case['chunk_attempts'][key]=attempts+1;c.dc.add(case['id'])
        room=max(2,min(c.a.chunk_frontier,c.a.max_pending_boxes-len(case['frontier'])))
        return dict(kind='exact',cid=case['id'],states=case['states'],box_id=key,box=box,
                    table_bytes=c.r['table_bytes'],address_bytes=c.r['address_bytes'],
                    leaf_operations=max(1024,c.a.leaf_operations//(2**min(attempts,12))),
                    max_frontier=room,seconds=c.a.exact_seconds)
    return plan_job()

def status(c):
    now=c.elapsed();m=c.m['exact_schedule']
    unfinished=[case for case in c.cases.values() if case['exact'] is None]
    busy={};endpoints={}
    for j in c.active.values():
        if j['kind']!='exact':continue
        busy[j['cid']]=busy.get(j['cid'],0)+1
        e=c.cases[j['cid']]['endpoint'];endpoints[e]=endpoints.get(e,0)+1
    top=sorted(c.cases.values(),key=lambda case:case.get('exact_completed_job_cpu_seconds',0),reverse=True)[:2]
    def phase_cpu(case):
        return max(0,case.get('exact_completed_job_cpu_seconds',0)-case.get('fair_phase_start_cpu',0))
    current=sorted(c.cases.values(),key=phase_cpu,reverse=True)[:2]
    return dict(configuration=m['configuration'],yields=m['yields'],phase_history=m['phase_history'],
        phase_exact_cpu_seconds=sum(phase_cpu(case) for case in c.cases.values()),
        largest_phase_cpu=[dict(endpoint=case['endpoint'],pattern_id=case['pid'],
            cpu_seconds=phase_cpu(case),complete=case['exact'] is not None) for case in current],
        cooling_families=sum(case.get('fair_resume_at',0)>now for case in unfinished),
        max_running_per_family=max(busy.values(),default=0),
        max_running_per_endpoint=max(endpoints.values(),default=0),
        largest_cumulative_cpu=[dict(endpoint=case['endpoint'],pattern_id=case['pid'],
            cpu_seconds=case.get('exact_completed_job_cpu_seconds',0),complete=case['exact'] is not None)
            for case in top])

def summary(value):
    a=value['configuration']
    return ('exact 분산: family 최대 {}작업 / endpoints 최대 {}작업 | '
            '순서 양보 {}회 / 대기 중 {} families | 이번 설정 exact {:.2f} CPUh').format(
        a['family_slots'],a['endpoint_slots'],value['yields'],value['cooling_families'],
        value['phase_exact_cpu_seconds']/3600)

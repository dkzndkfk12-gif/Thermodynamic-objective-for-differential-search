
import math

VERSION = 'adaptive_activity_v2'

def key(n, k):
    return '{}:{}'.format(n, k)

def new_domain(n, k, initial=False):
    return dict(input_active=n, cap=k, initial=initial, submitted=0, finished=0,
                rows=0, positive_rows=0, seconds=0.0, best_F=None, improvements=0,
                failure_streak=0, retry_after=0, retry_wall=0, last_submitted=-1, stops={})

def permitted(c, d):
    return (c.a.corridor_input_active <= d['input_active'] <= min(c.a.corridor_max_input_active,4*d['cap'])
            and min(c.a.corridor_caps or [1]) <= d['cap'] <= c.a.corridor_max_cap)

def initialize(c):
    a=c.a; m=c.m['corridor']
    s=m.setdefault('adaptation', dict(version=VERSION, finished=0, expansions=0,
                                     next_expand=0, domains={}, events=[]))
    if s['version'] not in ('adaptive_activity_v1',VERSION):
        raise ValueError('Unknown adaptive activity ledger version')
    upgraded=s['version']!=VERSION;s['version']=VERSION
    if upgraded:
        for d in s['domains'].values():
            d['allocation_seconds']=0;d['novel_certificates']=0
    for k in a.corridor_caps:
        d=s['domains'].setdefault(key(a.corridor_input_active,k),
                                 new_domain(a.corridor_input_active,k,True))
        d['initial']=True

    s['next_expand']=max(s['next_expand'],s['finished']+a.corridor_adapt_every)

def expand(c):
    s=c.m['corridor']['adaptation'];a=c.a
    if s['finished'] < s['next_expand']:
        return
    candidates={}
    for d in s['domains'].values():
        if not permitted(c,d) or not d['finished']:
            continue
        n,k=d['input_active'],d['cap']
        for nn,kk,axis in ((n+1,k,'input'),(n,k+1,'internal')):
            candidate=new_domain(nn,kk)
            if key(nn,kk) not in s['domains'] and permitted(c,candidate):
                candidates[key(nn,kk)]=(candidate,axis)
    if not candidates:
        return

    preferred='input' if s['expansions']%2==0 else 'internal'
    def order(item):
        d,axis=item
        u=d['input_active']-a.corridor_input_active
        v=max(0,d['cap']-max(a.corridor_caps))
        return (max(u,v),axis!=preferred,u+v,d['input_active']+d['cap'])
    d,axis=min(candidates.values(),key=order)
    s['domains'][key(d['input_active'],d['cap'])]=d
    s['expansions']+=1
    s['next_expand']=s['finished']+a.corridor_adapt_every
    s['events']=(s['events']+[dict(seconds=c.elapsed(),axis=axis,
        input_active=d['input_active'],cap=d['cap'],after_jobs=s['finished'])])[-32:]

def select(c):
    expand(c)
    s=c.m['corridor']['adaptation'];turn=c.m['corridor']['jobs']
    busy={};risky_running=0
    for j in c.running('corridor'):
        name=key(j['input_active'],j['cap']);busy[name]=busy.get(name,0)+1
        risky_running+=bool(j.get('adaptive_probe'))
    probe_slots=max(1,int(math.ceil(c.r['search_slots']*c.a.corridor_expansion_share)))
    wide=[d for d in s['domains'].values() if d['cap']>3]
    failed_wide=sum(max(0,d['finished']-d.get('productive_jobs',0)) for d in wide)

    probe_slots=max(1,int(math.ceil(probe_slots/(1+failed_wide/16.0))))
    domain_slots=max(1,int(math.ceil(c.r['search_slots']/max(3,len(c.a.corridor_caps)))))
    pool=[]
    for name,d in s['domains'].items():
        if not permitted(c,d):
            continue

        probe=(not d['initial'] or d['cap']>3) and (not d['positive_rows'] or d['failure_streak']>0)
        cooling=d['retry_after']>s['finished'] and d.get('retry_wall',0)>c.elapsed()
        if cooling or busy.get(name,0)>=(1 if probe else domain_slots):
            continue
        if probe and risky_running>=probe_slots:
            continue
        pool.append((name,d,probe))
    if not pool:
        return None

    def breadth(item):
        name,d,probe=item
        return (d['submitted']+busy.get(name,0),d['last_submitted'],name)
    fresh=[item for item in pool if not item[1]['submitted']]
    if fresh:
        selected=min(fresh,key=breadth)
    elif turn%4==0:
        wider=[item for item in pool if not item[1]['initial']]
        selected=min(wider or pool,key=breadth)
    elif turn%4==1:
        selected=min(pool,key=breadth)
    else:
        best=min((d['best_F'] for _,d,_ in pool if d['best_F'] is not None),default=None)
        def utility(item):
            name,d,probe=item
            quality=1.0 if best is None or d['best_F'] is None else 2**max(-12,best-d['best_F'])
            throughput=(1+d.get('novel_certificates',0))/(c.a.corridor_probe_seconds+d.get('allocation_seconds',0))
            return (quality*throughput/(1+busy.get(name,0)), -d['last_submitted'])
        selected=max(pool,key=utility)
    name,d,probe=selected
    seconds=c.a.corridor_seconds
    if probe:
        seconds=min(seconds,c.a.corridor_probe_seconds*2**min(3,d['submitted']))

    force=(d['input_active']>c.a.corridor_input_active and d['submitted']%2==0)
    return dict(domain=name,cap=d['cap'],input_active=d['input_active'],
                adaptive_probe=probe,seconds=seconds,start_active=d['input_active'] if force else 0,
                domain_turn=d['submitted'],copies=(1,2,4)[d['submitted']%3])

def committed(c,j):
    s=c.m['corridor']['adaptation'];name=key(j['input_active'],j['cap'])
    d=s['domains'].setdefault(name,new_domain(j['input_active'],j['cap']))
    d['submitted']+=1;d['last_submitted']=c.m['corridor']['jobs']

def completed(c,j,result,improvements):
    s=c.m['corridor']['adaptation'];s['finished']+=1
    name=key(j.get('input_active',c.a.corridor_input_active),j['cap'])
    d=s['domains'].setdefault(name,new_domain(j.get('input_active',c.a.corridor_input_active),j['cap']))
    diag=result.get('diagnostics',{});rows=int(diag.get('completed_rows',0))
    positive=max(0,rows-int(diag.get('proved_zero_rows',0)))
    reason=diag.get('stop',result.get('reason','UNKNOWN'))
    d['finished']+=1;d['rows']+=rows;d['positive_rows']+=positive
    d['productive_jobs']=d.get('productive_jobs',0)+int(positive>0)
    d['seconds']+=max(0,float(result.get('cpu_seconds',result.get('job_wall_seconds',j.get('seconds',0)))))
    d['allocation_seconds']=d.get('allocation_seconds',0)+max(0,float(result.get('cpu_seconds',result.get('job_wall_seconds',j.get('seconds',0)))))
    d['improvements']+=improvements
    d['novel_certificates']=d.get('novel_certificates',0)+improvements

    unfinished=diag.get('unfinished_input')
    if unfinished and int(unfinished,16):
        failures=c.m['corridor'].setdefault('unfinished_rows',{})
        failures['{}:{}'.format(j['cap'],unfinished)]=c.elapsed()
        if len(failures)>512:
            for k in sorted(failures,key=failures.get)[:-512]:del failures[k]
    d['stops'][reason]=d['stops'].get(reason,0)+1
    fs=[x['mass']['F'] for x in result.get('corridor_records',[]) if x['mass']['F'] is not None]
    if fs:d['best_F']=min(fs+([] if d['best_F'] is None else [d['best_F']]))
    if not positive:
        d['failure_streak']+=1

        d['retry_after']=s['finished']+min(256,4*2**min(6,d['failure_streak']))
        d['retry_wall']=c.elapsed()+min(120,c.a.corridor_probe_seconds*2**min(3,d['failure_streak']))
    else:
        d['failure_streak']=0;d['retry_after']=0;d['retry_wall']=0

def summary(c):
    s=c.m['corridor']['adaptation'];domains=[d for d in s['domains'].values() if permitted(c,d)]
    attempted=[d for d in domains if d['submitted']]
    return dict(version=VERSION,finished_jobs=s['finished'],expansions=s['expansions'],
        largest_open_input=max((d['input_active'] for d in domains),default=0),
        largest_open_cap=max((d['cap'] for d in domains),default=0),
        largest_attempted_input=max((d['input_active'] for d in attempted),default=0),
        largest_attempted_cap=max((d['cap'] for d in attempted),default=0),
        limits=dict(input=c.a.corridor_max_input_active,internal=c.a.corridor_max_cap),
        domains=domains,events=s['events'],adaptive_allocation_is_heuristic=True,
        finite_budget_does_not_guarantee_all_domains_reached=True)

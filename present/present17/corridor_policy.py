
from fractions import Fraction
import math
import adaptive_scope
import resource_policy

from exact_plan import dyadic_record, sum_dyadics
from model import ROUNDS, parse_state, inverse, ddt

VERSION = 'collapsed_transfer_v3'

def add_arguments(p):
    p.set_defaults(corridor_caps='2,3,4')
    p.set_defaults(corridor_input_active=2)
    p.set_defaults(corridor_max_cap=16)
    p.set_defaults(corridor_max_input_active=16)
    p.set_defaults(corridor_adapt_every=12)
    p.set_defaults(corridor_expansion_share=0.25)
    p.set_defaults(corridor_probe_seconds=15)
    p.set_defaults(corridor_seconds=60)
    p.set_defaults(corridor_max_nodes=0)
    p.set_defaults(corridor_max_edges=0)

def validate(a,p):
    try:a.corridor_caps=[] if a.corridor_caps=='0' else sorted(set(map(int,a.corridor_caps.split(','))))
    except ValueError:p.error('Invalid --corridor-caps')
    if any(not 1<=k<=16 for k in a.corridor_caps):p.error('corridor caps must be in 1..16')
    if (max(a.corridor_caps or [1])>a.corridor_max_cap or
            a.corridor_input_active>a.corridor_max_input_active or
            a.corridor_adapt_every<1 or not 0<a.corridor_expansion_share<=.5 or
            not math.isfinite(a.corridor_probe_seconds) or a.corridor_probe_seconds<=0):
        p.error('Invalid adaptive corridor limits')
    if (not math.isfinite(a.corridor_seconds) or a.corridor_seconds<=0 or
            not (a.corridor_max_nodes==0 or 128<=a.corridor_max_nodes<2**63) or
            not (a.corridor_max_edges==0 or 128<=a.corridor_max_edges<2**64)):
        p.error('Invalid corridor budgets')

def configuration(a):
    return dict(version=VERSION,caps=list(a.corridor_caps),input_active=a.corridor_input_active,
        seconds=a.corridor_seconds,max_nodes=a.corridor_max_nodes,max_edges=a.corridor_max_edges,
        restriction='each X_1..X_(R-1) has <= k active nibbles; X_R is post-P and unrestricted',
        target='S_m(input)^gamma, S_m(input)=sum_output Z_k(input,output)^m; gamma=1 target',
        exact='integer transfer over every path in the declared corridor, no weight cutoff',
        arithmetic_bits=1024,maximum_denominator_power=3*a.corridor_max_input_active+3*a.corridor_max_cap*(ROUNDS-1),
        adaptive=dict(version=adaptive_scope.VERSION,max_cap=a.corridor_max_cap,
            max_input_active=a.corridor_max_input_active,expand_every=a.corridor_adapt_every,
            expansion_share=a.corridor_expansion_share,probe_seconds=a.corridor_probe_seconds,
            score='fixed-domain exact Z^m; no cross-domain Metropolis comparison',
            finite_budget_scope='opened and attempted domains reported; full scope not guaranteed'),
        stored_endpoints='top candidates from evaluated rows, NOT equilibrium samples',
        known_endpoints_injected=False,resource_failure='unfinished row UNKNOWN; expensive discovered pairs use separate persistent bidirectional transfer',
        input_proposal='activity-stratified independence MH with exact Hastings ratio; input ASN > 4*k is provably impossible and omitted')

def initialize(c):
    m=c.m.setdefault('corridor',dict(version=VERSION,jobs=0,certificates={},diagnostics={},history=[],discarded=0))
    if m['version'] not in ('collapsed_transfer_v1','collapsed_transfer_v2',VERSION):raise ValueError('Unsupported transfer ledger version')
    m['version']=VERSION
    conf=configuration(c.a)
    if not m['history'] or m['history'][-1]['configuration']!=conf:
        m['history'].append(dict(seconds=c.elapsed(),configuration=conf))
    c._corridor_start_pool=None
    adaptive_scope.initialize(c)

def input_activity(value):
    return sum(ch!='0' for ch in value)

def job(c):
    if not c.a.corridor_caps:return None
    m=c.m['corridor']
    selected=adaptive_scope.select(c)
    if selected is None:return None
    input_cap=selected['input_active']
    start=None

    turn=selected['domain_turn']
    warm=(turn%2==1 or (selected['adaptive_probe'] and turn==0))
    if warm and not selected['start_active']:
        if c._corridor_start_pool is None:
            old=sorted(c.cases.values(),key=lambda x:(x['weight'],x['first_retained_seconds']))
            c._corridor_start_pool=list(dict.fromkeys(x['input'] for x in old
                if input_activity(x['input'])<=c.a.corridor_max_input_active))[:128]
        cert=sorted(m['certificates'].values(),key=lambda x:x['mass']['F'])
        pool=list(dict.fromkeys([x['input'] for x in cert]+c._corridor_start_pool))
        failed=m.get('unfinished_rows',{})
        pool=[x for x in pool if input_activity(x)<=min(input_cap,4*selected['cap']) and
              c.elapsed()-failed.get('{}:{}'.format(selected['cap'],x),-1e100)>600]
        if pool:start=pool[(turn//2)%min(128,len(pool))]

    nodes,edges=resource_policy.corridor_limits(c.a,c.r)
    return dict(selected,kind='corridor',initial=start,keep=16,
        max_nodes=nodes,max_edges=edges,
        address_bytes=c.r['address_bytes'])

def committed(c,j):
    adaptive_scope.committed(c,j)
    c.m['corridor']['jobs']+=1

def fraction(mass):
    return Fraction(int(mass['numerator']),1<<mass['denominator_power'])

def validate_record(row):
    cap=int(row['cap']);states=row['states'].split(',')
    if not 1<=cap<=16 or len(states)!=ROUNDS+1:raise ArithmeticError('Bad transfer scope')
    if states[0]!=row['input'] or states[-1]!=row['output']:raise ArithmeticError('Transfer witness endpoints mismatch')
    x=[parse_state(s) for s in states]
    if not any(x[0]) or not any(x[-1]):raise ArithmeticError('Zero transfer endpoint')
    if any(sum(v!=0 for v in state)>cap for state in x[1:-1]):raise ArithmeticError('Transfer witness outside corridor')
    weight=0;table=ddt()
    for a,b in zip(x,x[1:]):
        for u,v in zip(a,inverse(b)):
            n=table[u][v]
            if n not in (2,4,16):raise ArithmeticError('Transfer witness has illegal DDT transition')
            weight+=3 if n==2 else 2 if n==4 else 0
    den=int(row['denominator_power']);num=int(row['numerator'])
    expected=3*sum(v!=0 for v in x[0])+3*cap*(ROUNDS-1)
    if den!=expected or den<weight or not (1<<(den-weight))<=num<=(1<<den):
        raise ArithmeticError('Transfer integer mass/denominator failure')
    if int(row['trail_weight'])!=weight:raise ArithmeticError('Transfer witness weight mismatch')
    return dict(input=states[0],output=states[-1],cap=cap,states=states,
        mass=dyadic_record(num,den),witness_weight=weight,
        scope='all trails with ASN(X_r)<=cap for r=1..R-1; final P included',
        exact=True,first_seconds=float(row['first_seconds']))

def consume(c,j,result):
    m=c.m['corridor'];d=result.get('diagnostics',{})
    for key in ('completed_rows','proved_zero_rows','proposals','accepted_inputs','cache_hits','swap_attempts','swaps'):
        m['diagnostics'][key]=m['diagnostics'].get(key,0)+int(d.get(key,0))
    for key in ('peak_nodes','peak_edges'):
        m['diagnostics'][key]=max(m['diagnostics'].get(key,0),int(d.get(key,0)))
    reason=d.get('stop',result.get('reason','UNKNOWN'))
    m['diagnostics'].setdefault('job_stops',{})[reason]=m['diagnostics'].get('job_stops',{}).get(reason,0)+1
    cap_stats=m['diagnostics'].setdefault('by_cap',{}).setdefault(str(j['cap']),dict(jobs=0,completed_rows=0,stops={}))
    cap_stats['jobs']+=1;cap_stats['completed_rows']+=int(d.get('completed_rows',0))
    cap_stats['stops'][reason]=cap_stats['stops'].get(reason,0)+1
    improvements=merge_records(c,j,result.get('corridor_records',[]),result)
    adaptive_scope.completed(c,j,result,improvements)

def merge_records(c,j,records,result,origin_kind='collapsed_transfer'):

    m=c.m['corridor'];changed=False;pending_witnesses={};improvements=0
    for record in records:
        record=dict(record);key=record['input']+'_'+record['output'];old=m['certificates'].get(key)
        improves=old is None or fraction(record['mass'])>fraction(old['mass'])
        if old:
            p0,p1=fraction(old['mass']),fraction(record['mass'])
            if ((old['cap']==record['cap'] and p0!=p1) or
                (old['cap']<record['cap'] and p0>p1) or (old['cap']>record['cap'] and p0<p1)):
                raise ArithmeticError('Nested corridor exact sums disagree')
            if old['cap']>=record['cap']:continue
            absorbed=[]
            for case in c.cases.values():
                if case['endpoint']!=key:continue
                activity=max(mask.count('1') for mask in c.patterns[case['pid']]['pattern'][1:])
                if old['cap']<activity<=record['cap']:
                    mass=case['exact'] or dyadic_record(case.get('partial_numerator','0'),3*case['active'])
                    absorbed.append(mass)
            if p1<p0+fraction(sum_dyadics(absorbed)):
                raise ArithmeticError('Expanded corridor omitted previously certified outside mass')
        record['first_seconds']=j['started']+record['first_seconds']
        record['completed_seconds']=c.elapsed()
        record['origin']=dict(kind=origin_kind,job=j['job_index'],seed=j['seed'],copies=j.get('copies'),
            initial_input=j.get('initial'),input_activity_limit=j.get('input_active'),
            forced_initial_activity=j.get('start_active',0),adaptive_probe=j.get('adaptive_probe',False),
            job_cpu_seconds=result.get('cpu_seconds',0))
        if old:record['first_seconds']=min(record['first_seconds'],old['first_seconds'])
        m['certificates'][key]=record
        improvements+=int(improves)
        pending_witnesses[key]=record
        changed=True

    if len(m['certificates'])>512:

        protected={row['endpoint'] for row in unions(c,True)[:10]}

        protected.update(e for e,r in m['certificates'].items()
                         if (r.get('origin') or {}).get('kind')=='endpoint_conditioned_transfer')
        order=sorted(m['certificates'],key=lambda e:fraction(m['certificates'][e]['mass']),reverse=True)
        for e in order[512:]:
            if e not in protected:del m['certificates'][e];m['discarded']+=1

    for e,record in pending_witnesses.items():
        if m['certificates'].get(e) is record:
            c.add_witness(record['states'],record['origin'],record['first_seconds'])
    if changed:
        c.union_cache=None;c.progress_cache=None;c._joint_group_cache=None
    return improvements

def covered(c,case):
    record=c.m.get('corridor',{}).get('certificates',{}).get(case['endpoint'])
    if record is None:return False

    return all(mask.count('1')<=record['cap'] for mask in c.patterns[case['pid']]['pattern'][1:])

def unions(c,partial=False):
    certificates=c.m['corridor']['certificates'];groups={e:[] for e in certificates};inside={}
    seen=set()
    for case in c.cases.values():
        key=(case['endpoint'],case['pid'])
        if key in seen:raise ArithmeticError('Duplicate activity mass')
        seen.add(key)
        mass=case['exact']
        if mass is None and partial:mass=dyadic_record(case.get('partial_numerator','0'),3*case['active'])
        if mass is None or not int(mass['numerator']):continue
        if covered(c,case):inside.setdefault(case['endpoint'],[]).append(mass)
        else:groups.setdefault(case['endpoint'],[]).append((case,mass))
    rows=[]
    for e,items in groups.items():
        record=certificates.get(e)
        if record and e in inside and fraction(sum_dyadics(inside[e]))>fraction(record['mass']):
            raise ArithmeticError('Covered pattern sum exceeds the complete corridor mass')
        parts=[mass for case,mass in items]
        if record:parts.append(record['mass'])
        if not parts:continue
        mass=sum_dyadics(parts);a,b=e.split('_')
        row=dict(endpoint=e,input=a,output=b,mass=mass,
            component_ids=sorted(case['id'] for case,m in items),patterns=len(items),

            complete_patterns=sum(case['exact'] is not None for case,m in items),
            unfinished_patterns=sum(case['exact'] is None for case,m in items))
        if record:
            row['corridor']=record
            row['pattern_count_scope']='explicit additional patterns OUTSIDE corridor; implicit corridor pattern count not enumerated'
        rows.append(row)
    rows.sort(key=lambda row:fraction(row['mass']),reverse=True)
    return rows

def status(c):
    m=c.m['corridor'];cert=list(m['certificates'].values())
    best=max(cert,key=lambda x:fraction(x['mass'])) if cert else None
    return dict(configuration=configuration(c.a),submitted_jobs=m['jobs'],running=len(c.running('corridor')),
        diagnostics=m['diagnostics'],adaptation=adaptive_scope.summary(c),certified_endpoints=len(cert),discarded_endpoints=m['discarded'],
        best=best,history=m['history'],equilibrium_certified=False,
        old_pattern_families_covered=sum(covered(c,case) for case in c.cases.values()))

def summary(value):
    d=value['diagnostics'];b=value['best']
    mass='2^-{:.9f}'.format(b['mass']['F']) if b else '미완료'
    limits=sum(n for reason,n in d.get('job_stops',{}).items() if reason in ('NODE_LIMIT','EDGE_LIMIT','MEMORY_LIMIT'))
    a=value['adaptation']
    return ('전체-pattern exact: 완료 input {}회(중복 포함) | 최고 {} (내부 cap {}) | '
            '시도한 입력 상한 {} / 내부 상한 {} (최대 {}/{}) | 확장 {}회 | 진행 {}개 | 자원 한도 {}작업').format(
        d.get('completed_rows',0),mass,b['cap'] if b else '-',a['largest_attempted_input'],
        a['largest_attempted_cap'],a['limits']['input'],a['limits']['internal'],a['expansions'],value['running'],limits)

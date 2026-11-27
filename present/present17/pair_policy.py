
from pathlib import Path
import math
import tempfile

from model import ROUNDS, digest
import corridor_policy

VERSION = 'independent_endpoint_transfer_v1'
WORK_FILES=('checkpoint.bin','checkpoint.bin.tmp','forward.bin','forward.bin.tmp',
            'backward.bin','backward.bin.tmp','progress.json','progress.json.tmp','worker.lock')

def remove_finished_work(directory):

    for name in WORK_FILES:
        try:(directory/name).unlink()
        except FileNotFoundError:pass
    try:directory.rmdir()
    except OSError:pass

def add_arguments(p):
    p.set_defaults(pair_threads=0)
    p.set_defaults(pair_seconds=90)
    p.set_defaults(pair_max_seconds=900)
    p.set_defaults(pair_share=0.75)
    p.set_defaults(pair_pending=64)
    p.set_defaults(pair_scratch=None)

def validate(a,p):
    if (not 0<=a.pair_threads<=100 or not 0<a.pair_share<=1 or a.pair_pending<1 or
        not all(math.isfinite(v) and v>0 for v in (a.pair_seconds,a.pair_max_seconds)) or
        a.pair_max_seconds<a.pair_seconds):
        p.error('Invalid endpoint-conditioned transfer settings')

def threads(c):
    reservation=max(1,int(c.r['exact_slots']*c.a.pair_share))
    return min(c.a.pair_threads or min(8,max(1,reservation//2)),reservation)

def configuration(a):
    return dict(version=VERSION,threads=a.pair_threads,seconds=a.pair_seconds,
        max_seconds=a.pair_max_seconds,share=a.pair_share,pending=a.pair_pending,
        discovery='only witnesses/certificates already discovered or explicitly reused by this campaign',
        exact_scope='all paths with ASN(X_1)..ASN(X_(R-1)) <= cap at the selected endpoints',
        method='bidirectional integer transfer; smaller integer width; parallel transactional batches',
        checkpoint='layer/source cursor plus accumulated integer frontier, checksummed atomic scratch files',
        probability_gate='F/count gate applies to per-pattern conditioning only; DP messages are not completed trail-family probabilities',
        known_endpoints_injected=False)

def initialize(c):
    m=c.m.setdefault('pair_transfer',dict(version=VERSION,tasks={},turn=0,history=[],completed=0))
    if m['version']!=VERSION:raise ValueError('Unknown endpoint-conditioned exact ledger')
    conf=configuration(c.a)
    if not m['history'] or m['history'][-1]['configuration']!=conf:
        m['history'].append(dict(seconds=c.elapsed(),configuration=conf))

    name=m.setdefault('scratch_id',digest([str(c.out),c.protocol['seed'],ROUNDS])[:24])
    if len(name)!=24 or set(name)-set('0123456789abcdef'):raise ValueError('Invalid scratch identity')
    base=(Path(c.a.pair_scratch).expanduser().resolve() if c.a.pair_scratch else
          Path(tempfile.gettempdir())/'present_exact_work')

    c.pair_scratch=base/name/digest(str(c.out))[:12]
    c.pair_scratch.mkdir(parents=True,exist_ok=True)
    for name,t in m['tasks'].items():
        if len(name)!=64 or set(name)-set('0123456789abcdef'):raise ValueError('Invalid exact task identity')
        if (c.pair_scratch/name).is_symlink():raise ValueError('Exact work directory must not be a symlink')
        if t.get('done'):remove_finished_work(c.pair_scratch/name)

def candidates(c):
    groups={}
    for case in c.cases.values():
        k=max(mask.count('1') for mask in c.patterns[case['pid']]['pattern'][1:])
        if k>c.a.corridor_max_cap:continue
        e=case['endpoint'];entry=groups.get(e)
        if entry is None or (k,case['weight'],case['id'])<(entry['witness_cap'],entry['weight'],entry['cid']):
            groups[e]=dict(endpoint=e,witness_cap=k,weight=case['weight'],cid=case['id'],
                states=case['states'],F=float(case['weight']))
    for e,record in c.m['corridor']['certificates'].items():
        if e not in groups:
            groups[e]=dict(endpoint=e,witness_cap=max(sum(ch!='0' for ch in x) for x in record['states'][1:-1]),
                weight=record['witness_weight'],cid=None,states=record['states'],F=record['mass']['F'])
        groups[e]['F']=min(groups[e]['F'],record['mass']['F'])

    for row in c.progress_unions():
        if row['endpoint'] in groups:groups[row['endpoint']]['F']=row['mass']['F']
    return groups

def next_job(c,available):
    if not c.a.corridor_caps:return None
    m=c.m['pair_transfer'];nt=threads(c)
    active=c.running('pair');reserved=max(nt,int(c.r['exact_slots']*c.a.pair_share))
    if available<nt or sum(j['cpu_cores'] for j in active)+nt>reserved:return None
    busy={j['endpoint'] for j in active};now=c.elapsed()
    for name,t in m['tasks'].items():
        record=c.m['corridor']['certificates'].get(t['endpoint'])
        if not t.get('done') and t['endpoint'] not in busy and record and record['cap']>=t['cap']:
            t['done']=True;t['superseded_by_larger_exact_cap']=record['cap']

            c.flush();remove_finished_work(c.pair_scratch/name)
    pool=candidates(c)

    order=sorted(pool,key=lambda e:(pool[e]['F'],e))
    is_coverage=m['turn']%4==3
    if not is_coverage:order=order[:min(c.a.pair_pending,max(16,2*len(active)+4))]
    else:

        spent={}
        for t in m['tasks'].values():spent[t['endpoint']]=spent.get(t['endpoint'],0)+t.get('cpu_seconds',0)
        order=sorted(order,key=lambda e:(spent.get(e,0),pool[e]['F'],e))[:c.a.pair_pending]
    eligible=[]
    for e in order:
        if e in busy:continue
        g=pool[e];record=c.m['corridor']['certificates'].get(e)
        cap=max(g['witness_cap'],(record['cap']+1 if record else min(c.a.corridor_caps)))
        if cap>c.a.corridor_max_cap:continue
        name=digest([ROUNDS,e,cap,VERSION]);task=m['tasks'].get(name)
        if task and (task.get('done') or task.get('retry_at',0)>now):continue
        spent=(task or {}).get('cpu_seconds',0)
        served=sum(t.get('cpu_seconds',0) for t in m['tasks'].values() if t['endpoint']==e)

        score=(served if is_coverage else spent, (task or {}).get('last',-1),g['F'],e)
        eligible.append((score,name,cap,g,task))
    if not eligible:return None
    _,name,cap,g,task=min(eligible,key=lambda x:x[0])
    stalled=(task or {}).get('stalls',0)
    seconds=min(c.a.pair_max_seconds,c.a.pair_seconds*2**min(4,stalled))
    return dict(kind='pair',pair_id=name,endpoint=g['endpoint'],states=list(g['states']),
        cap=cap,seconds=seconds,cpu_cores=nt,threads=nt,

        address_bytes=c.r['address_bytes']*nt,table_bytes=c.r['table_bytes']*nt,
        max_nodes=max(128,c.r['table_bytes']*nt//(96 if 3*sum(ch!='0' for ch in g['states'][0])+3*cap*(ROUNDS-1)<256 else
                         192 if 3*sum(ch!='0' for ch in g['states'][0])+3*cap*(ROUNDS-1)<512 else 384)),
        work=str(c.pair_scratch/name))

def committed(c,j):
    if j['kind']!='pair':return
    m=c.m['pair_transfer'];m['turn']+=1
    t=m['tasks'].setdefault(j['pair_id'],dict(endpoint=j['endpoint'],cap=j['cap'],
        jobs=0,cpu_seconds=0,done=False,first=c.elapsed(),stalls=0,progress=None))
    t['jobs']+=1;t['last']=m['turn'];t['work']=j['work']

def consume(c,j,result):
    m=c.m['pair_transfer'];t=m['tasks'][j['pair_id']]
    t['cpu_seconds']+=max(0,result.get('cpu_seconds',0))+max(0,result.get('python_thread_cpu_seconds',0))
    d=result.get('pair_progress') or {}
    old=t.get('progress') or {}
    t['stalls']=0 if d.get('generation',0)>old.get('generation',0) and (
        d.get('phase')!=old.get('phase') or d.get('round')!=old.get('round') or
        d.get('completed_sources',0)>old.get('completed_sources',0)) else t['stalls']+1
    t['progress']=d;t['last_reason']=result.get('reason',d.get('reason','UNKNOWN'))
    t['cpu_time_has_lower_bound_jobs']=t.get('cpu_time_has_lower_bound_jobs',False) or result.get('cpu_time_is_lower_bound',False)

    t['retry_at']=c.elapsed()+(min(120,5*2**min(4,t['stalls'])) if t['stalls'] else 1)
    if result.get('corridor_records'):

        merge_job=dict(j,input_active=sum(ch!='0' for ch in j['states'][0]),copies=1,
            job_index=j['job_index'],domain=None)
        for record in result['corridor_records']:
            record['scope_cpu_seconds']=t['cpu_seconds']
            record['scope_cpu_time_has_lower_bound_jobs']=t['cpu_time_has_lower_bound_jobs']
            record['scope_first_submitted_seconds']=t['first']
        corridor_policy.merge_records(c,merge_job,result['corridor_records'],result,
                                      origin_kind='endpoint_conditioned_transfer')
        t['done']=True;t['completed_seconds']=c.elapsed();m['completed']+=1

        c.flush()

        remove_finished_work(c.pair_scratch/j['pair_id'])
    elif result.get('status')=='PAIR_DONE':
        raise ArithmeticError('Positive witnessed endpoint received no exact certificate')

def status(c):
    m=c.m['pair_transfer'];unfinished=[t for t in m['tasks'].values() if not t.get('done')]
    active=[]
    for j in c.running('pair'):
        d=(m['tasks'].get(j['pair_id']) or {}).get('progress')
        try:
            import json
            live=json.loads((Path(j['work'])/'progress.json').read_text())
            if live.get('input')+'_'+live.get('output')==j['endpoint'] and live.get('cap')==j['cap']:d=live
        except (OSError,ValueError,TypeError):pass
        active.append(dict(endpoint=j['endpoint'],cap=j['cap'],threads=j['threads'],progress=d))
    return dict(configuration=configuration(c.a),submitted=m['turn'],completed=m['completed'],
        unfinished=len(unfinished),running=active,scratch=str(c.pair_scratch),
        cpu_seconds=sum(t['cpu_seconds'] for t in m['tasks'].values()),
        latest=[dict(endpoint=t['endpoint'],cap=t['cap'],progress=t.get('progress'),
                     reason=t.get('last_reason'),cpu_seconds=t['cpu_seconds']) for t in
                sorted(unfinished,key=lambda t:t.get('last',0),reverse=True)[:3]],
        completed_scopes_are_exact=True,unfinished_scope_is_unknown=True,
        copied_results_without_scratch='completed proofs retained; unfinished pair frontier recomputes if scratch is absent')

def summary(value):
    return '자체 발견 후보의 endpoint exact: 완료 {}개 / 진행 {}개 / 대기 {}개 | CPU {:.2f}h'.format(
        value['completed'],len(value['running']),value['unfinished'],value['cpu_seconds']/3600)


import csv
from functools import lru_cache
import json
import math
import os
import signal
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import corridor_policy

from model import (catalog, decode_trail, domain_reduction, domains_text,
                   encode_trail, native_text, terminal_weight, ddt, ROUNDS, pattern_from_states)
from exact_plan import adaptive_text, prepare_family, verify_partition

ROOT = Path(__file__).resolve().parent
STOP = threading.Event()
PROCESSES = set()
LOCK = threading.Lock()
USAGE = threading.local()

def stop_all():
    STOP.set()
    with LOCK:
        for p in list(PROCESSES):
            if p.returncode is None:
                try:os.kill(p.pid,signal.SIGTERM)
                except ProcessLookupError:pass

def invoke(command, seconds, deadline=None, grace=10):
    env = dict(os.environ, OMP_NUM_THREADS='1', OPENBLAS_NUM_THREADS='1', MKL_NUM_THREADS='1')
    if STOP.is_set():
        return None, '', 'STOP'
    if deadline is not None:
        seconds = min(seconds, deadline-time.monotonic())
    if seconds<=0:
        return None, '', 'CAMPAIGN_TIME_LIMIT'
    command = list(map(str, command))
    if '--seconds' in command:
        command[command.index('--seconds')+1] = str(seconds)
    else:
        command[7] = str(seconds)
    if hasattr(os,'wait4'):

        with tempfile.TemporaryFile(mode='w+t') as output, tempfile.TemporaryFile(mode='w+t') as errors:
            p=subprocess.Popen(command,stdout=output,stderr=errors,text=True,env=env)
            with LOCK:
                PROCESSES.add(p)
                if STOP.is_set():
                    try:os.kill(p.pid,signal.SIGTERM)
                    except ProcessLookupError:pass
            hard_end=time.monotonic()+seconds+grace;killed=False
            try:
                while True:
                    with LOCK:
                        pid,status,usage=os.wait4(p.pid,os.WNOHANG)
                        if pid:p.returncode=os.WEXITSTATUS(status) if os.WIFEXITED(status) else -os.WTERMSIG(status)
                    if pid:
                        USAGE.cpu_seconds=usage.ru_utime+usage.ru_stime
                        USAGE.peak_rss_kib=usage.ru_maxrss
                        break
                    if time.monotonic()>=hard_end and not killed:
                        try:os.kill(p.pid,signal.SIGKILL)
                        except ProcessLookupError:pass
                        killed=True
                    time.sleep(.02)
            finally:
                with LOCK:PROCESSES.discard(p)
            output.seek(0);errors.seek(0);stdout=output.read();stderr=errors.read()[-2000:]
            if killed:return None,stdout,'NATIVE_TIMEOUT: '+stderr[-1000:]
            if STOP.is_set() and p.returncode!=0:return None,stdout,'STOP'
            return p.returncode,stdout,stderr
    p = subprocess.Popen(command, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, env=env)
    with LOCK:
        PROCESSES.add(p)
        if STOP.is_set():
            p.terminate()
    try:
        stdout, stderr = p.communicate(timeout=seconds+grace)
    except subprocess.TimeoutExpired:
        p.kill()
        stdout, stderr = p.communicate()
        return None, stdout, 'NATIVE_TIMEOUT: ' + stderr[-1000:]
    finally:
        with LOCK:
            PROCESSES.discard(p)
    if STOP.is_set() and p.returncode != 0:
        return None, stdout, 'STOP'
    return p.returncode, stdout, stderr[-2000:]

@lru_cache(maxsize=96)
def compiled_family(states):
    return prepare_family(states)

def singleton_plan(compiled):

    if any(len(domain)!=1 for domain in compiled['domains']):return None
    values=[domain[0] for domain in compiled['domains']]
    numerator=1
    table=ddt()
    for factor in compiled['factors']:
        a,b=factor['a'],factor['b']
        for v,src,dst in factor['av']:a^=((values[v]>>src)&1)<<dst
        for v,src,dst in factor['bv']:b^=((values[v]>>src)&1)<<dst
        count=table[a][b]
        if not a or not b or count not in (2,4):
            raise ArithmeticError('Singleton domain contradicts its verified active witness')
        numerator*=count//2
    if numerator!=int(compiled['witness_numerator']):
        raise ArithmeticError('Singleton exact product disagrees with verified witness')
    proof=dict(status='EXACT_DONE',numerator=str(numerator),
        denominator_power=compiled['denominator_power'],initial_volume='1',closed_volume='1',
        frontier=[],solved_nodes=1,zero_nodes=0,component_cache_hits=0,reason='SINGLE_ASSIGNMENT')
    verify_partition(compiled,[],proof)
    return dict(status='PLAN',operation_score=0,estimated_table_bytes=0,initial_volume='1',
        cell_bytes=0,branch=-1,components=0,fits_leaf_budget=True,unit_exact=proof,
        witness_weight=compiled['witness_weight'])

def execute(job):
    start = time.monotonic()
    USAGE.cpu_seconds=None;USAGE.peak_rss_kib=None
    thread_start = time.thread_time() if hasattr(time, 'thread_time') else 0
    result = dict(kind=job['kind'], status='INCOMPLETE', cpu_seconds=0)
    try:
        with tempfile.TemporaryDirectory(prefix='present_exact17_') as folder:
            temp = Path(folder)
            kind = job['kind']
            if kind=='pair':
                output=temp/'pair.json';a,b=job['endpoint'].split('_')
                command=[ROOT/'bin/pair','--input',a,'--output',b,'--cap',job['cap'],
                    '--threads',job['threads'],'--seconds',job['seconds'],'--nodes',job['max_nodes'],
                    '--memory',job['address_bytes'],'--token',job['job_index'],
                    '--work',job['work'],'--out',output]
                rc,stdout,err=invoke(command,job['seconds'],job.get('deadline_monotonic'),grace=120)
                progress=output if output.exists() else Path(job['work'])/'progress.json'
                data=json.loads(progress.read_text()) if progress.exists() else {}
                resource_error=err.startswith(('SAVED_FRONTIER_EXCEEDS_RAM_BUDGET','JOIN_RAM_LIMIT',
                    'std::bad_alloc','MERGE_ABORT_RELOAD_CHECKPOINT','WORK_DIRECTORY_BUSY'))
                if data and data.get('token')==job['job_index']:
                    if (data.get('input')!=a or data.get('output')!=b or data.get('cap')!=job['cap'] or
                            data.get('denominator_power')!=3*sum(ch!='0' for ch in a)+3*job['cap']*(ROUNDS-1)):
                        raise ArithmeticError('Stale/mismatched endpoint exact progress')
                    result.update(status=data['status'],pair_progress=data,cpu_seconds=data['cpu_seconds'])
                    if data.get('probability_complete'):

                        weight=encode_trail(catalog([pattern_from_states(job['states'])]),0,job['states'])['witness_weight']
                        row=dict(input=a,output=b,cap=job['cap'],states=','.join(job['states']),
                            numerator=data['numerator'],denominator_power=data['denominator_power'],
                            trail_weight=weight,first_seconds=time.monotonic()-job['submitted_monotonic'])
                        result['corridor_records']=[corridor_policy.validate_record(row)]
                    result['cpu_time_is_lower_bound']=bool(rc is None or not output.exists())
                    if rc is not None and rc>0 and not resource_error:
                        raise RuntimeError('Endpoint exact engine: '+err)
                    if rc not in (0,None) and not data.get('probability_complete'):
                        result.update(reason=err,status='INCOMPLETE')
                elif rc==0:
                    raise ArithmeticError('Exact pair returned without this job\'s progress')
                elif rc is None or rc<0 or resource_error:
                    result.update(status='INCOMPLETE',reason=err or 'PAIR_RELOAD_DURABLE_CHECKPOINT')
                else:
                    raise RuntimeError('Endpoint exact engine: '+err)
            elif kind in ('exact', 'plan'):
                compiled = compiled_family(tuple(job['states']))
                if kind=='plan':
                    quick=singleton_plan(compiled)
                    if quick is not None:
                        result.update(quick)
                        result['job_wall_seconds']=time.monotonic()-start
                        result['python_thread_cpu_seconds']=(time.thread_time()-thread_start) if thread_start else 0
                        return result
                parent = job.get('box', [])
                model_file, checkpoint = temp/'exact.txt', temp/'checkpoint.json'
                model_file.write_text(adaptive_text(compiled, [parent]), encoding='ascii')
                command = [ROOT/'bin/exact', 'plan' if kind=='plan' else 'eval', model_file,
                           checkpoint, job['address_bytes'], job['table_bytes'],
                           job['leaf_operations'], job['seconds'], job['max_frontier'], 2]
                rc, stdout, err = invoke(command, job['seconds'], job.get('deadline_monotonic'))
                if rc == 0:
                    result.update(json.loads(stdout))
                elif kind=='exact' and checkpoint.exists() and (rc is None or (rc is not None and rc<0)):
                    result.update(json.loads(checkpoint.read_text(encoding='ascii')))
                    result['reason'] = err or 'RECOVERED_CHECKPOINT'
                elif rc is None or (kind=='plan' and err.strip() in ('CHUNK_TIME_LIMIT','std::bad_alloc')):
                    result.update(status='INCOMPLETE', reason=err)
                else:
                    raise RuntimeError('Exact engine: '+err)
                if result['status'] in ('EXACT_PROGRESS', 'EXACT_DONE'):
                    verify_partition(compiled, parent, result)
                    solved=result['solved_nodes'];zeros=result['zero_nodes']
                    if (not isinstance(solved,int) or not isinstance(zeros,int) or
                            not 0<=zeros<=solved):
                        raise ArithmeticError('Invalid exact subproblem completion count')
                result['witness_weight'] = compiled['witness_weight']
            else:
                out = temp/'result.tsv'
                command = [ROOT/'bin/search', kind if kind != 'screen' else 'ais', '--out', out,
                           '--seconds', job['seconds'], '--seed', job['seed'], '--limit', job.get('keep', 16)]
                cat = witness = None
                if kind == 'corridor':
                    command += ['--center',job['cap'],'--width',job['input_active'],'--copies',job['copies'],
                                '--corridor-nodes',job['max_nodes'],'--sweeps',job['max_edges'],
                                '--start-active',job.get('start_active',0),'--memory-bytes',job['address_bytes']]
                    if job.get('initial'):command += ['--initial',job['initial']]
                elif kind == 'seed':
                    command += ['--active', job['active'], '--sweeps', 1000000000]
                elif kind == 'ensemble':
                    seeds=job['initial_trails']
                    if not 1<=len(seeds)<=16:raise ValueError('Invalid joint-search seed count')
                    f=temp/'ensemble.txt'
                    lines=['PRESENT_ENSEMBLE_V1 {} {}'.format(ROUNDS,len(seeds))]
                    for states in seeds:
                        if len(states)!=ROUNDS+1:raise ValueError('Wrong-round ensemble seed')
                        lines.extend(states)
                    f.write_text('\n'.join(lines)+'\n',encoding='ascii')
                    command+=['--bridge',f,'--copies',job['copies'],'--active',job['active'],'--sweeps',1000000000]
                elif kind == 'pattern':
                    command += ['--active', job['active'], '--sweeps', 1000000000]
                elif kind in ('search', 'screen'):
                    cat = catalog([job['pattern']])
                    f = temp/'model.txt'
                    f.write_text(native_text(cat), encoding='ascii')
                    command += ['--model', f]
                    if job.get('states'):
                        witness = encode_trail(cat, 0, job['states'])
                    if kind == 'search':
                        command += ['--copies', job['copies'], '--sweeps', 1000000000]
                        if witness:
                            command += ['--initial', witness['coordinates']+witness['witness_internal']]
                    else:
                        reduced = domain_reduction(cat, 0, witness['coordinates'], job.get('deadline_monotonic'))
                        if reduced['proven_zero']:
                            raise ArithmeticError('A witnessed family cannot have zero mass')
                        f = temp/'domains.txt'
                        f.write_text(domains_text(cat, 0, reduced), encoding='ascii')
                        command += ['--endpoint', witness['coordinates'], '--pattern', 0, '--domains', f,
                                    '--paths', job['paths'], '--steps', job['steps']]
                elif kind == 'expand':
                    f = temp/'bridge.txt'
                    f.write_text('PRESENT_BRIDGE17_V2\n'+'\n'.join(job['states'])+'\n', encoding='ascii')
                    command += ['--bridge', f, '--center', job['center'], '--width', 8, '--sweeps', 1000000000]
                else:
                    raise ValueError('Unknown job kind')
                launch_offset = time.monotonic()-job['submitted_monotonic']
                rc, stdout, err = invoke(command, job['seconds'], job.get('deadline_monotonic'))
                if kind=='corridor' and (rc is None or rc<0) and Path(str(out)+'.checkpoint').exists():

                    out=Path(str(out)+'.checkpoint');rc=0
                    result['reason']='RECOVERED_COMPLETED_ROW_ARCHIVE'
                    result['cpu_time_is_lower_bound']=True
                metrics = Path(str(out)+'.metrics.json')
                if metrics.exists():
                    result.update(json.loads(metrics.read_text()))
                if rc == 1:
                    raise RuntimeError('Search engine: ' + err)
                if rc != 0 or not out.exists():
                    result.update(status='INCOMPLETE', reason=err or 'native incomplete')
                else:
                    with out.open(encoding='ascii', newline='') as stream:
                        rows = list(csv.DictReader(stream, delimiter='\t'))
                    result['status'] = 'COMPLETE'
                    if kind == 'corridor':
                        result['corridor_records']=[corridor_policy.validate_record(row) for row in rows]
                        for record in result['corridor_records']:
                            if record['cap']!=job['cap'] or corridor_policy.input_activity(record['input'])>job['input_active']:
                                raise ArithmeticError('Native transfer record differs from dispatched activity domain')
                            record['first_seconds']+=launch_offset
                    elif kind == 'pattern':

                        result['patterns'] = [r['pattern'].split(',') for r in rows]
                    elif kind == 'search':
                        hits = []
                        for row in rows:
                            trail = decode_trail(cat, int(row['pattern']), row['endpoint'], row['internal'])
                            if not math.isclose(trail['weight'], float(row['trail_weight']), rel_tol=1e-12, abs_tol=1e-8):
                                raise ArithmeticError('Search witness weight mismatch')
                            hits.append(dict(states=trail['states'], hard=int(row['hard_visits']),
                                             soft=int(row['soft_visits']), first_seconds=launch_offset+float(row['first_seconds'])))
                        result['hits'] = hits
                    elif kind in ('expand','seed','ensemble'):
                        result['hits'] = [dict(states=r['states'].split(','),
                                              first_seconds=launch_offset+float(r['first_seconds'])) for r in rows]
                    else:
                        logs = []
                        if len(rows) != job['paths']:
                            raise ArithmeticError('Incomplete AIS batch')
                        for index, row in enumerate(rows):
                            w = float(row['log_weight'])
                            tw = terminal_weight(cat, 0, row['endpoint'], row['internal'])
                            if (int(row['path_id']) != index or row['endpoint'] != witness['coordinates'] or
                                int(row['pattern']) != 0 or not math.isclose(float(row['trail_weight']), (-1 if tw is None else tw), rel_tol=1e-12, abs_tol=1e-8) or
                                (tw is None) != (w == -math.inf) or math.isnan(w) or w == math.inf):
                                raise ArithmeticError('Invalid AIS path')
                            logs.append(w)
                        finite = [w for w in logs if math.isfinite(w)]
                        if finite:
                            top = max(finite)
                            weights = [math.exp(w-top) if math.isfinite(w) else 0 for w in logs]
                            total = math.fsum(weights)
                            result.update(F=-(top+math.log(total/len(logs)))/math.log(2),
                                          ess=total*total/math.fsum(w*w for w in weights), positive=len(finite),
                                          max_weight_fraction=max(weights)/total, paths=len(logs))
                        else:
                            result.update(F=None, ess=0, positive=0)
                        result['paths'] = len(logs)
                        result.setdefault('max_weight_fraction', 0)
                        result['sampling_is_only_screening'] = True
                    diag = Path(str(out)+('.corridor.json' if kind=='corridor' else '.ensemble.json' if kind=='ensemble' else '.search.json' if kind=='search' else '.expand.json'))
                    if diag.exists():
                        result['diagnostics'] = json.loads(diag.read_text())
                    elif kind=='corridor':
                        result['diagnostics']=dict(stop='HARD_TIMEOUT_ARCHIVE_RECOVERED')
    except TimeoutError:
        result.update(status='RESOURCE_DEFERRED' if job['kind']=='exact' else 'INCOMPLETE', reason='CAMPAIGN_TIME_LIMIT')
    except Exception as exc:
        result.update(status='ERROR', reason='{}: {}'.format(type(exc).__name__, exc))
    result['job_wall_seconds'] = time.monotonic()-start
    result['python_thread_cpu_seconds'] = (time.thread_time()-thread_start) if thread_start else 0
    if USAGE.cpu_seconds is not None:
        result['cpu_seconds']=USAGE.cpu_seconds
        result['peak_rss_kib']=USAGE.peak_rss_kib
        result['cpu_time_is_lower_bound']=False
        result['cpu_accounting']='wait4 per child, including all OpenMP threads'
    return result

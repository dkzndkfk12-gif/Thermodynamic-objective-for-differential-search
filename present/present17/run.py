#!/usr/bin/env python3

import argparse
from concurrent.futures import ThreadPoolExecutor
from fractions import Fraction
import hashlib
import json
import math
import os
import platform
from pathlib import Path
import secrets
import shlex
import signal
import sqlite3
import subprocess
import sys
import time

from model import ROUNDS, catalog, digest, encode_trail, file_hash, save, pattern_from_states
from exact_plan import dyadic_record, sum_dyadics
import workers
import resource_policy
import search_policy
import ensemble_policy
import evaluation_policy
import corridor_policy
import pair_policy

ROOT = Path(__file__).resolve().parent
GIB = 1024**3

def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))

def source_hash():
    names = ['resource_policy.py', 'run.py', 'search_policy.py', 'workers.py', 'model.py', 'exact_plan.py', 'src/search.cpp', 'src/exact.cpp', 'src/adaptive_exact.hpp', 'src/rounds.hpp', 'src/ensemble.hpp', 'ensemble_policy.py', 'src/blocked_gibbs.hpp', 'evaluation_policy.py', 'corridor_policy.py', 'src/corridor_integer.hpp', 'src/sparse_corridor.hpp', 'adaptive_scope.py']
    names += ['pair_policy.py','src/pair.cpp','src/pair_transfer.hpp']
    return digest({p: file_hash(ROOT/p) for p in names})

def available_memory():
    return resource_policy.available_memory()

def arguments():
    p = argparse.ArgumentParser(description='PRESENT differential search')
    p.add_argument('out', nargs='?', default='runs/run')
    p.add_argument('--cores', type=int, default=0, help='number of CPU cores; 0 selects available cores')
    p.set_defaults(memory_gib=0)
    p.set_defaults(exact_job_gib=0)
    p.add_argument('--hours','--wall-hours',dest='wall_hours',type=float,default=24,help='Cumulative maximum hours; probability never triggers campaign stopping')
    p.set_defaults(active_counts=','.join(map(str, range(2 * ROUNDS, 4 * ROUNDS + 1))))
    p.set_defaults(ensemble_max_active=None)
    p.set_defaults(ensemble_seconds=60)
    p.add_argument('--seed', type=int)
    p.set_defaults(pattern_seconds=60)
    p.set_defaults(search_seconds=120)
    p.set_defaults(screen_seconds=120)
    p.set_defaults(exact_seconds=20)
    p.set_defaults(plan_seconds=10)
    p.set_defaults(leaf_operations=5000000)
    p.set_defaults(chunk_frontier=64)
    p.set_defaults(max_pending_boxes=10000)
    p.set_defaults(focus_families=2)
    p.set_defaults(exact_family_slots=1)
    p.set_defaults(exact_endpoint_slots=4)
    p.set_defaults(exact_slice_cpu_seconds=60)
    p.set_defaults(exact_cooldown_seconds=120)
    p.set_defaults(expansion_seconds=120)
    p.set_defaults(screen_paths=32)
    p.set_defaults(screen_steps=256)
    p.set_defaults(reuse_run=[])
    p.set_defaults(max_patterns=4096)
    p.set_defaults(max_candidates=20000)
    p.set_defaults(keep_per_job=12)
    p.set_defaults(import_witnesses=[])
    corridor_policy.add_arguments(p)
    pair_policy.add_arguments(p)
    a = p.parse_args()
    corridor_policy.validate(a,p)
    pair_policy.validate(a,p)
    if not sys.platform.startswith('linux'):
        p.error('This program requires Linux or WSL.')
    try:
        a.counts = sorted(set(int(x) for x in a.active_counts.split(',')))
    except (ValueError, ZeroDivisionError):
        p.error('Invalid active counts')
    if not a.counts or min(a.counts)<ROUNDS or max(a.counts)>16*ROUNDS:
        p.error('Active counts must be in ROUNDS..16*ROUNDS')
    a._ensemble_active_arg=a.ensemble_max_active
    a.ensemble_max_active=max(a.counts) if a.ensemble_max_active is None else (16*ROUNDS if a.ensemble_max_active==0 else a.ensemble_max_active)
    if not ROUNDS<=a.ensemble_max_active<=16*ROUNDS:
        p.error('ensemble-max-active must be 0 or ROUNDS..16*ROUNDS')
    for name in ('wall_hours','pattern_seconds','search_seconds','screen_seconds',
                 'exact_seconds','plan_seconds','expansion_seconds','ensemble_seconds','exact_slice_cpu_seconds','exact_cooldown_seconds'):
        if not math.isfinite(getattr(a,name)) or getattr(a,name)<=0:
            p.error(name+' must be finite and positive')
    for name in ('screen_paths','screen_steps','leaf_operations','chunk_frontier','max_pending_boxes','focus_families','max_patterns','max_candidates','keep_per_job','exact_family_slots','exact_endpoint_slots'):
        if getattr(a,name)<1:
            p.error(name+' must be positive')
    if not 2<=a.screen_steps<=32768 or a.screen_paths>1000000 or a.keep_per_job>128 or a.exact_seconds>300 or a.plan_seconds>60:
        p.error('Native size limit exceeded')
    if not 2<=a.chunk_frontier<=256 or a.max_pending_boxes<a.chunk_frontier or a.focus_families>16:
        p.error('Invalid adaptive frontier/focus limits')
    if not 1<=a.exact_family_slots<=a.exact_endpoint_slots<=100:
        p.error('Require 1 <= exact-family-slots <= exact-endpoint-slots <= 100')
    if a.cores<0 or (a.seed is not None and not 0<=a.seed<2**64):
        p.error('Invalid cores or seed')
    if any(not math.isfinite(x) or x<0 for x in (a.memory_gib,a.exact_job_gib)):
        p.error('Invalid memory budget')
    return a

def layout(a):
    return resource_policy.layout(a)

def build(out):
    (ROOT/'bin').mkdir(exist_ok=True)
    import fcntl
    import resource
    started=time.monotonic()
    before=resource.getrusage(resource.RUSAGE_CHILDREN)

    build_lock=(ROOT/'bin/build.lock').open('a')
    fcntl.flock(build_lock,fcntl.LOCK_EX)
    compiler = shlex.split(os.environ.get('CXX','g++'))
    flags = ['-O3','-std=c++17','-Wall','-Wextra','-fno-fast-math','-ffp-contract=off','-fopenmp']
    stamp = dict(source=source_hash(), compiler=compiler, flags=flags)
    old = ROOT/'bin/build.json'
    if old.exists() and read(old)==stamp and all((ROOT/'bin'/s).exists() for s in ('search','exact','pair')):
        build_lock.close()
        return dict(reused=True,wall_seconds=time.monotonic()-started,cpu_seconds=0,flags=flags,compiler=compiler)
    if (out/'build.log').exists():
        import shutil
        backup=out/('build.before.'+file_hash(out/'build.log')[:12]+'.log')
        if not backup.exists():shutil.copy2(out/'build.log',backup)
    with (out/'build.log').open('w',encoding='utf-8') as f:
        for name in ('search','exact','pair'):
            cmd = compiler + flags + [str(ROOT/'src'/(name+'.cpp')),'-o',str(ROOT/'bin'/name)]
            f.write(' '.join(shlex.quote(x) for x in cmd)+'\n'); f.flush()
            try:
                subprocess.run(cmd,stdout=f,stderr=f,check=True)
            except subprocess.CalledProcessError as exc:
                raise RuntimeError('Build failed. See build.log; a C++17 compiler is required.') from exc
    save(old,stamp)
    after=resource.getrusage(resource.RUSAGE_CHILDREN)
    build_lock.close()
    return dict(reused=False,wall_seconds=time.monotonic()-started,
                cpu_seconds=after.ru_utime+after.ru_stime-before.ru_utime-before.ru_stime,
                flags=flags,compiler=compiler)

def witness_arrays(value):

    if isinstance(value,list):
        if len(value)==ROUNDS+1 and all(isinstance(s,str) and len(s)==16 and not set(s.upper())-set('0123456789ABCDEF') for s in value):
            yield [s.upper() for s in value]
        else:
            for v in value:
                yield from witness_arrays(v)
    elif isinstance(value,dict):
        for v in value.values():
            if isinstance(v,(dict,list)):
                yield from witness_arrays(v)

class Campaign:
    def __init__(self,a):
        self.a=a;self.out=Path(a.out).resolve();self.out.mkdir(parents=True,exist_ok=True)
        import fcntl
        self.lock=(self.out/'run.lock').open('a')
        try: fcntl.flock(self.lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError: raise SystemExit('This run is already active.')
        self.r=layout(a);self.started=time.monotonic();self.interrupted=False
        self.db=sqlite3.connect(str(self.out/'state.sqlite3'))
        self.db.execute('PRAGMA journal_mode=DELETE');self.db.execute('PRAGMA synchronous=FULL')
        for name in ('patterns','cases','meta'):
            self.db.execute('CREATE TABLE IF NOT EXISTS '+name+' (id TEXT PRIMARY KEY, data TEXT NOT NULL)')
        self.patterns={k:json.loads(v) for k,v in self.db.execute('SELECT id,data FROM patterns')}
        self.cases={k:json.loads(v) for k,v in self.db.execute('SELECT id,data FROM cases')}
        item=self.db.execute("SELECT data FROM meta WHERE id='state'").fetchone()
        self.m=json.loads(item[0]) if item else dict(elapsed=0,counter=0,cpu={},completed={},expansions={},
            admitted=0,dropped=0,invalid_patterns=0,first_certified=None,closed=True,imports=[],errors=[])
        self.base=self.m['elapsed'];self.dp=set();self.dc=set();self.active={};self.stop_reason='RUNNING'
        self.union_cache=None;self.progress_cache=None

        self.speculative={cid for cid,c in self.cases.items() if self.unstarted(c)}
        if not self.m.get('closed',True):self.m['timing_has_unclean_session']=True
        protocol_path=self.out/'PROTOCOL.json'
        old=read(protocol_path) if protocol_path.exists() else None
        if a._ensemble_active_arg is None and (old or {}).get('joint_patterns_search'):
            a.ensemble_max_active=int(old['joint_patterns_search']['active_cap'])
        if not ROUNDS<=a.ensemble_max_active<=16*ROUNDS:
            raise ValueError('Invalid saved joint-search active cap')
        seed=old['seed'] if old and a.seed is None else (a.seed if a.seed is not None else secrets.randbits(64))
        protocol=dict(schema='PRESENT_STATMECH_EXACT17_LITERATURE_V1',seed=seed,source_hash=source_hash(),rounds=ROUNDS,
            cipher='PRESENT-64',cell_bits=4,exact_numerator_bits=4096,proved_intermediate_bits_below=4*16*(ROUNDS-1)+16*ROUNDS,
            model='independent_round_key_DDT_product',scope='exact sum of completed disjoint activity families; certified full-hull lower bound',
            execution_mode='search',stop_on_probability=False,
            active_counts=a.counts,replicas=[1,2,4],
            candidate_queue_limit=a.max_candidates,
            candidate_queue_policy='bounded pending families; endpoint-group balancing and turnover; preserve proofs/frontiers/active jobs',

            search='adaptive input/internal activity domains; fixed-domain collapsed exact Z^m replica/PT jobs; auxiliary ensemble; disjoint exact union',
            corridor_search=corridor_policy.configuration(a),
            endpoint_exact=pair_policy.configuration(a),
            evaluation_schedule=evaluation_policy.configuration(a),
            joint_patterns_search=dict(version=ensemble_policy.VERSION,active_cap=a.ensemble_max_active,
                target='Z_cap(endpoint)^m',patterns_independent_per_copy=True,repeated_trails_allowed=True,
                cap_affects='joint search only; exact family model unchanged',equilibrium_certified=False,
                target_beta=1,legal_betas=[1,1.5,2,3],ensemble_seconds=a.ensemble_seconds,
                internal_blocks='single-state value Gibbs plus two-state collective MH with full proposal normalization',
                endpoint_blocks='independent per-copy conditional sums'),
            evaluation='cost-planned adaptive conditioning; exact support propagation, lazy DDT factors, cached independent components, bounded integer contractions, disjoint checkpoint frontier',
            screen_paths=a.screen_paths,screen_steps=a.screen_steps,leaf_operations=a.leaf_operations,

            chunk_frontier=a.chunk_frontier,known_endpoints_injected_by_default=False,
            reference='https://doi.org/10.13154/tosc.v2018.i3.265-289',
            graph_reference='https://doi.org/10.13154/tosc.v2018.i3.265-289',
            exact_method='sum-product integer variable elimination with disjoint adaptive conditioning',
            initial_seed_choices_are_not_target_constraints=True,sampling_values_are_certificates=False,
            seed_activity_cap=a.ensemble_max_active,midpoint_activity_cap=None,trail_weight_cutoff=None,
            active_counts_apply_to_pattern_proposals_only=True,all_original_factors_retained=True)
        if old is not None and (old.get('rounds')!=ROUNDS or old.get('source_hash')!=protocol['source_hash'] or old.get('seed')!=seed):
            raise ValueError('Source or model changed; use a new output directory')
        save(protocol_path,protocol)
        self.protocol=protocol
        self.m['closed']=False;self.m['resources']=self.r
        self.m.setdefault('focus',[])
        self.m.setdefault('reuse_runs',[])
        self.m.setdefault('evaluation_counter',0)
        for c in self.cases.values():
            if c.get('last_resource_reason')=='FRONTIER_STORAGE_LIMIT' and len(c['frontier'])<a.max_pending_boxes:
                c['next_exact_at']=0;c['last_resource_reason']=None;self.dc.add(c['id'])

        corridor_policy.initialize(self)
        search_policy.initialize(self, 3)
        ensemble_policy.initialize(self)
        evaluation_policy.initialize(self)
        pair_policy.initialize(self)

    def elapsed(self):return self.base+time.monotonic()-self.started

    def flush(self,closed=False):
        for name,dirty,items in (('patterns',self.dp,self.patterns),('cases',self.dc,self.cases)):
            for k in dirty:self.db.execute('INSERT OR REPLACE INTO '+name+' VALUES (?,?)',(k,json.dumps(items[k],separators=(',',':'),allow_nan=False)))
            dirty.clear()
        self.m['elapsed']=self.elapsed();self.m['closed']=closed;self.m['last_heartbeat_unix']=time.time()
        self.db.execute('INSERT OR REPLACE INTO meta VALUES (?,?)',('state',json.dumps(self.m,separators=(',',':'),allow_nan=False)))
        self.db.commit()

    @staticmethod
    def unstarted(c):
        return (not c.get('plan') and c.get('exact') is None
                and not int(c.get('partial_numerator','0')) and not c.get('frontier')
                and not c.get('solved_nodes') and c.get('exact_first_submitted_seconds') is None)

    def pattern(self,pattern,allow_overflow=False):
        pid=digest(list(pattern))
        if pid in self.patterns:return self.patterns[pid]
        if len(self.patterns)>=self.a.max_patterns and not allow_overflow:return None
        try:m=catalog([pattern])['patterns'][0]
        except ValueError:
            self.m['invalid_patterns']+=1;return None
        p=dict(id=pid,pattern=list(pattern),active=m['active'],dimension=m['d'],searches=0,states=None)
        self.patterns[pid]=p;self.dp.add(pid);search_policy.register(self,p);return p

    def add_witness(self,states,origin,when):
        if len(states)!=ROUNDS+1:raise ValueError('Wrong number of complete PRESENT round states')
        pattern=pattern_from_states(states)
        cat=catalog([pattern]);w=encode_trail(cat,0,states)
        e=w['endpoints'];endpoint=e['input']+'_'+e['output'];cid=digest([endpoint,pattern])
        if cid not in self.cases and len(self.speculative)>=self.a.max_candidates:
            self.m['candidate_admission_attempts']=self.m.get('candidate_admission_attempts',0)+1
            victim=ensemble_policy.eviction_candidate(self)

            if victim is None:
                self.m['dropped']+=1;return
            ensemble_policy.pending_remove(self,victim)
            old_id=victim['id']
            self.speculative.remove(old_id)
            del self.cases[old_id];self.dc.discard(old_id)
            self.db.execute('DELETE FROM cases WHERE id=?',(old_id,))
            self.m['archive_discarded_entries']=self.m.get('archive_discarded_entries',0)+1
            self.m['candidate_queue_evictions']=self.m.get('candidate_queue_evictions',0)+1
        p=self.pattern(pattern,True)
        if p is None:raise ArithmeticError('Legal witness pattern failed compilation')
        if not p['states']:p['states']=states;self.dp.add(p['id'])
        if cid not in self.cases:
            self.cases[cid]=dict(id=cid,pid=p['id'],endpoint=endpoint,input=e['input'],output=e['output'],
                states=states,weight=w['witness_weight'],witness_numerator=w['witness_numerator'],active=p['active'],first_retained_seconds=when,
                origins=[origin],screen=None,screen_attempts=0,plan=None,
                frontier={},partial_numerator='0',closed_volume='0',root_volume=None,
                chunk_attempts={},solved_nodes=0,zero_nodes=0,cache_hits=0,exact=None,next_exact_at=0,
                exact_completed_job_cpu_seconds=0,exact_sum_job_wall_seconds=0,
                fair_slice_start_cpu=0,fair_phase_start_cpu=0,
                exact_peak_rss_kib=0,exact_first_submitted_seconds=None)
            self.m['admitted']+=1
            self.speculative.add(cid)
            ensemble_policy.pending_add(self,self.cases[cid])
        else:
            c=self.cases[cid]
            if origin not in c['origins'] and len(c['origins'])<8:c['origins'].append(origin)
            c['first_retained_seconds']=min(c['first_retained_seconds'],when)
            if int(w['witness_numerator'])>int(c['witness_numerator']):
                c['states']=states;c['weight']=w['witness_weight'];c['witness_numerator']=w['witness_numerator']
        self.dc.add(cid)

    def unions(self):
        if self.union_cache is None:self.union_cache=corridor_policy.unions(self)
        return self.union_cache

    def screen_score(self,c):
        s=c.get('screen') or {}
        return s['F'] if s.get('F') is not None else float(c['weight'])

    def running(self,kind,key=None):
        return [j for j in self.active.values() if j['kind']==kind and (key is None or j.get('cid',j.get('pid'))==key)]

    def progress_unions(self):
        if self.progress_cache is None:self.progress_cache=corridor_policy.unions(self,partial=True)
        return self.progress_cache

    def priority_score(self,c):
        s=c.get('screen') or {}
        if s.get('F') is None:return float(c['weight'])

        return s['F']+math.log2(max(1,s.get('paths',self.a.screen_paths)/max(1,s.get('ess',1))))

    def evaluation_job(self):
        evaluation_slots=self.r['exact_slots']
        available=min(self.r['workers']-self.used_cores(),evaluation_slots-self.evaluation_cores())
        job=pair_policy.next_job(self,available)
        return job if job is not None else evaluation_policy.next_job(self)

    def used_cores(self):
        return sum(j.get('cpu_cores',1) for j in self.active.values())

    def evaluation_cores(self):
        return sum(j.get('cpu_cores',1) for j in self.active.values() if j['kind'] in ('pair','exact','plan','screen'))

    def search_job(self):
        return ensemble_policy.search_job(self)

    def submit(self,pool,job):
        if self.used_cores()+job.get('cpu_cores',1)>self.r['workers']:return False
        deadline_elapsed=self.a.wall_hours*3600
        remaining=deadline_elapsed-self.elapsed()
        if remaining<=0:return False
        self.m['counter']+=1
        job.update(seed=int(digest([self.protocol['seed'],self.protocol['schema'],self.m['counter'],job['kind']])[:16],16),
                   started=self.elapsed(),submitted_monotonic=time.monotonic(),job_index=self.m['counter'],
                   keep=job.get('keep',self.a.keep_per_job))
        if job['kind']=='exact':
            c=self.cases[job['cid']]
            if c['exact_first_submitted_seconds'] is None:
                c['exact_first_submitted_seconds']=job['started'];self.dc.add(c['id'])
        job['seconds']=min(job['seconds'],remaining)
        job['deadline_monotonic']=self.started+deadline_elapsed-self.base
        self.active[pool.submit(workers.execute,job)]=job
        search_policy.committed(self,job)
        ensemble_policy.committed(self,job)
        pair_policy.committed(self,job)
        return True

    def consume(self,job,result):
        kind=job['kind'];self.m['completed'][kind]=self.m['completed'].get(kind,0)+1
        cpu=result.get('cpu_seconds',0)+result.get('python_thread_cpu_seconds',0)
        self.m['cpu'][kind]=self.m['cpu'].get(kind,0)+cpu
        ensemble_policy.consume_diagnostics(self,job,result)
        if result['status']=='ERROR':
            self.m['errors']=(self.m['errors']+[dict(kind=kind,cid=job.get('cid'),reason=result.get('reason'))])[-12:]
            raise RuntimeError(result.get('reason','worker error'))
        if kind=='pair':
            pair_policy.consume(self,job,result)
        elif kind=='corridor':
            corridor_policy.consume(self,job,result)
        elif kind=='pattern':
            for p in result.get('patterns',[]):self.pattern(p)
        elif kind in ('search','expand','seed','ensemble'):
            origin=dict(kind=kind,seed=job['seed'],job_index=job['job_index'],replicas=job.get('copies'),
                        pattern_id=job.get('pid'),allocation=job.get('allocation'),warm_started=bool(job.get('states') or job.get('initial_trails')),
                        starting_endpoint=job.get('endpoint'),active_cap=job.get('active') if kind=='ensemble' else None,
                        generated_legal_chain=(kind=='seed'))
            for hit in result.get('hits',[]):
                self.add_witness(hit['states'],origin,
                                 job['started']+hit['first_seconds'])
            self.m['archive_discarded_entries']=self.m.get('archive_discarded_entries',0)+result.get('diagnostics',{}).get('archive_discarded_entries',0)
        else:
            c=self.cases[job['cid']];self.dc.add(c['id'])
            if kind=='exact':
                c['exact_completed_job_cpu_seconds']+=cpu
                c['exact_sum_job_wall_seconds']+=result['job_wall_seconds']
                c['exact_peak_rss_kib']=max(c['exact_peak_rss_kib'],result.get('peak_rss_kib',0))
            if kind=='screen':
                current={k:result[k] for k in ('status','F','ess','positive','reason','paths','max_weight_fraction') if k in result}
                old=c.get('screen')
                if old and old.get('status')=='COMPLETE' and current.get('status')=='COMPLETE':

                    n0=old.get('paths',self.a.screen_paths);n1=current.get('paths',self.a.screen_paths)
                    finite=[x['F'] for x in (old,current) if x.get('F') is not None]
                    top=min(finite) if finite else 0
                    a0=n0*2**(top-old['F']) if old.get('F') is not None else 0
                    a1=n1*2**(top-current['F']) if current.get('F') is not None else 0
                    if a0+a1:
                        current['F']=top-math.log2((a0+a1)/(n0+n1))
                        current['ess']=(a0+a1)**2/(a0*a0/max(old.get('ess',1),1e-300)+a1*a1/max(current.get('ess',1),1e-300))
                        current['max_weight_fraction']=max(a0*old.get('max_weight_fraction',1),a1*current.get('max_weight_fraction',1))/(a0+a1)
                    current['paths']=n0+n1
                    current['positive']=old.get('positive',0)+current.get('positive',0)
                if current.get('status')=='COMPLETE' or old is None:c['screen']=current
            elif kind=='plan':
                if result['status']=='PLAN':
                    c['plan']={k:result[k] for k in ('operation_score','estimated_table_bytes','initial_volume','cell_bytes','branch','components','fits_leaf_budget')}
                    c['root_volume']=result['initial_volume'];c['frontier']={digest([]):[]}
                    c['last_resource_reason']=None
                    self.speculative.discard(c['id'])
                    ensemble_policy.pending_remove(self,c)
                    proof=result.get('unit_exact')
                    if proof is not None:
                        if c['root_volume']!='1' or int(proof['numerator'])!=int(c['witness_numerator']):
                            raise ArithmeticError('Invalid single-assignment certificate')
                        c['partial_numerator']=str(proof['numerator']);c['closed_volume']='1';c['frontier']={}
                        c['solved_nodes']=1;c['zero_nodes']=0;c['last_chunk_reason']='SINGLE_ASSIGNMENT'
                        c['exact']=dyadic_record(c['partial_numerator'],3*c['active'])
                        c['exact_completed_seconds']=self.elapsed()
                        c['exact_first_submitted_seconds']=job['started']
                        c['exact_completed_job_cpu_seconds']+=cpu
                        c['exact_sum_job_wall_seconds']+=result['job_wall_seconds']
                        c['plan']['single_assignment_proof']=True
                        self.m['singleton_certifications']=self.m.get('singleton_certifications',0)+1
                else:c['last_resource_reason']=result.get('reason','PLAN_INCOMPLETE')
            elif result['status'] in ('EXACT_PROGRESS','EXACT_DONE'):
                key=job['box_id']
                if c['frontier'].get(key)!=job['box']:raise ArithmeticError('Duplicate/stale adaptive result')
                del c['frontier'][key]
                for box in result['frontier']:
                    child=digest(box)
                    if child in c['frontier']:raise ArithmeticError('Duplicate adaptive box')
                    c['frontier'][child]=box
                c['partial_numerator']=str(int(c['partial_numerator'])+int(result['numerator']))
                self.progress_cache=None
                c['closed_volume']=str(int(c['closed_volume'])+int(result['closed_volume']))
                c['solved_nodes']+=result['solved_nodes'];c['zero_nodes']+=result['zero_nodes']
                c['cache_hits']+=result['component_cache_hits']
                c['last_chunk_reason']=result.get('reason')
                c['last_resource_reason']=result.get('reason') if result.get('reason')=='LEAF_MEMORY_LIMIT' else None
                for stale in list(c['chunk_attempts']):
                    if stale not in c['frontier']:del c['chunk_attempts'][stale]
                if not c['frontier']:
                    if c['closed_volume']!=c['root_volume']:raise ArithmeticError('Family partition not closed')
                    n=int(c['partial_numerator'])
                    if n<int(c['witness_numerator']):raise ArithmeticError('Exact family omitted its verified witness')
                    old_best=self.unions()[0]['mass'] if self.unions() else None
                    c['exact']=dyadic_record(n,3*c['active']);c['exact_completed_seconds']=self.elapsed()
                    self.progress_cache=None
            else:
                c['last_resource_reason']=result.get('reason','EXACT_INCOMPLETE')
                c['next_exact_at']=self.elapsed()+1

    def publish(self,terminal=False):
        unions=self.unions()
        progress=self.progress_unions()
        best=progress[0] if progress else (unions[0] if unions else None)
        state='FINISHED' if terminal else 'SEARCH_AND_EXACT'
        status=dict(status=state,reason=self.stop_reason,elapsed_seconds=self.elapsed(),best=best,
            partial_lower_bound_including_unfinished_patterns=best,errors=self.m['errors'])
        save(self.out/'STATUS.json',status)
        exported=[]
        eligible=unions
        for u in eligible[:10]:
            item=dict(u);item['components']=[]
            for cid in u['component_ids']:
                c=self.cases[cid]
                item['components'].append(dict(id=cid,pattern=self.patterns[c['pid']]['pattern'],states=c['states'],
                    active=c['active'],mass=c['exact'],screen=c['screen'],origins=c['origins'],
                    first_retained_seconds=c['first_retained_seconds'],exact_completed_seconds=c['exact_completed_seconds'],
                    exact_first_submitted_seconds=c['exact_first_submitted_seconds'],
                    exact_completed_job_cpu_seconds=c['exact_completed_job_cpu_seconds'],
                    exact_sum_job_wall_seconds=c['exact_sum_job_wall_seconds'],
                    exact_peak_rss_kib=c['exact_peak_rss_kib'],solved_nodes=c['solved_nodes'],component_cache_hits=c['cache_hits']))
            item['discovered_but_unfinished_patterns']=sum(c['endpoint']==u['endpoint'] and c['exact'] is None for c in self.cases.values())
            exported.append(item)
        exported_progress=progress
        result_status=dict(status)
        for field in ('partial_lower_bound_including_unfinished_patterns','certified_lower_bound_best'):
            if field in result_status:result_status[field]=exported_progress[0] if exported_progress else None
        save(self.out/'RESULTS.json',dict(protocol=self.protocol,status=result_status,exact_unions=exported,
            certified_partial_unions=exported_progress[:3],
            scope='Nested corridors: largest exact cap only, plus disjoint OUTSIDE patterns/partial boxes. Certified DDT full-hull lower bound.',
            search_archive_is_exhaustive=False,prior_imports=self.m['imports'],
            omitted_candidates_due_to_ledger_cap=self.m['dropped'],
            archive_discarded_entries=self.m.get('archive_discarded_entries',0)))
        probability='pending' if best is None else '2^-{:.9f}'.format(best['mass']['F'])
        text='elapsed={:.1f} min | best={}'.format(self.elapsed()/60,probability)
        if best is not None:text+=' | {} -> {}'.format(best['input'],best['output'])
        if terminal:text+=' | '+self.stop_reason
        save_path=self.out/'REPORT_KO.md'
        save_path.write_text(text+'\n',encoding='utf8')
        print(text,flush=True)

    def import_runs(self):
        for name in self.a.reuse_run:
            path=Path(name).resolve();dbpath=path/'state.sqlite3'
            if path==self.out:raise ValueError('--reuse-run requires a different, NEW output directory')
            old=read(path/'PROTOCOL.json')
            if (old.get('cipher')!='PRESENT-64' or old.get('rounds')!=ROUNDS or
                old.get('model')!='independent_round_key_DDT_product' or
                not old.get('schema','').startswith('PRESENT_STATMECH_EXACT')):
                raise ValueError('Only compatible PRESENT runs with the same round count can be reused')
            con=sqlite3.connect(dbpath.as_uri()+'?mode=ro',uri=True)
            try:
                con.execute('BEGIN')
                records=[json.loads(row[0]) for row in con.execute('SELECT data FROM cases')]
                sha=file_hash(dbpath)
                meta=json.loads(con.execute("SELECT data FROM meta WHERE id='state'").fetchone()[0])
            finally:con.close()
            if any(r['database_sha256']==sha for r in self.m['reuse_runs']):continue
            records.sort(key=lambda c:self.priority_score(c))
            count=0
            for c in records:
                origin=dict(kind='reused_candidate',source_database=sha,original_first_retained_seconds=c.get('first_retained_seconds'))
                self.add_witness(c['states'],origin,self.elapsed())
                cid=digest([c['endpoint'],pattern_from_states(c['states'])])
                if cid not in self.cases:continue
                new=self.cases[cid]
                if new['screen'] is None and (c.get('screen') or {}).get('F') is not None:
                    new['screen']=dict(c['screen']);new['screen']['paths']=new['screen'].get('paths',old.get('screen_paths',32));new['screen_attempts']=1
                self.dc.add(cid);count+=1
            self.m['reuse_runs'].append(dict(path=str(path),database_sha256=sha,candidates_imported=count,
                prior_elapsed_seconds=meta['elapsed'],prior_completed_cpu_seconds=meta.get('cpu',{}),
                scope='Candidate witnesses and diagnostic screens only; exact frontier starts anew'))

    def execute(self):
        self.m.setdefault('builds',[]).append(build(self.out))
        model_name='unknown'
        for line in Path('/proc/cpuinfo').read_text().splitlines():
            if line.startswith('model name'):
                model_name=line.split(':',1)[1].strip();break
        self.m['environment']=dict(cpu_model=model_name,platform=platform.platform(),python=sys.version,
                                   affinity_cores=self.r['cores'],memory_budget_bytes=self.r['native_budget_bytes'])
        self.import_runs()
        for filename in self.a.import_witnesses:
            path=Path(filename).resolve();sha=file_hash(path)
            if any(r['sha256']==sha for r in self.m['imports']):continue
            count=0
            for states in witness_arrays(read(path)):
                self.add_witness(states,'prior_import:'+sha[:12],self.elapsed());count+=1
            if not count:raise ValueError('No complete-round witnesses found in '+str(path))
            self.m['imports'].append(dict(path=str(path),sha256=sha,witnesses=count))
        self.flush();self.publish()
        last_report=last_save=time.monotonic()
        def interrupt(signum,frame):
            self.interrupted=True;workers.stop_all()
        signal.signal(signal.SIGINT,interrupt);signal.signal(signal.SIGTERM,interrupt)
        pool=ThreadPoolExecutor(max_workers=self.r['workers'])
        try:
            while True:
                for future in list(self.active):
                    if future.done():
                        job=self.active.pop(future);self.consume(job,future.result())
                now=self.elapsed()
                if self.interrupted or (self.out/'STOP').exists():self.stop_reason='USER_STOP';break
                if now>=self.a.wall_hours*3600:self.stop_reason='TIME_BUDGET';break
                neval=self.evaluation_cores()
                evaluation_slots=self.r['exact_slots']
                while neval<evaluation_slots and self.used_cores()<self.r['workers']:
                    job=self.evaluation_job()
                    if job is None:break
                    if not self.submit(pool,job):break
                    neval+=job.get('cpu_cores',1)
                ns=self.used_cores()-neval
                while not False and self.used_cores()<self.r['workers'] and ensemble_policy.discovery_capacity(self,ns):
                    job=self.search_job()
                    if job is None:break
                    if ns>=self.r['search_slots']:
                        job['seconds']=min(10,job['seconds']);job['borrowed_evaluation_slot']=True
                    if not self.submit(pool,job):break
                    ns+=1
                if time.monotonic()-last_save>=3:self.flush();last_save=time.monotonic()
                if time.monotonic()-last_report>=20:self.publish();last_report=time.monotonic()
                time.sleep(.05)
        except Exception as exc:
            self.stop_reason='ERROR: '+str(exc)
            raise
        finally:
            workers.stop_all()
            pool.shutdown(wait=True)

            for future,job in list(self.active.items()):
                try:
                    result=future.result()
                    if result.get('status')!='INCOMPLETE' or job['kind']=='pair':self.consume(job,result)
                except Exception as exc:
                    self.stop_reason='ERROR: '+str(exc)
                    self.m['errors']=(self.m['errors']+[dict(kind=job['kind'],reason=str(exc))])[-12:]
            self.active.clear();self.flush(closed=True);self.publish(terminal=True);self.db.close()
        return 1 if self.stop_reason.startswith('ERROR:') else 0

if __name__=='__main__':
    try:sys.exit(Campaign(arguments()).execute())
    except KeyboardInterrupt:sys.exit(130)
    except Exception as exc:
        print('ERROR: '+str(exc),file=sys.stderr);sys.exit(1)


import hashlib
import itertools
import json
import os
import time
from functools import lru_cache
from pathlib import Path

ROUNDS = 16
SBOX = (12, 5, 6, 11, 9, 0, 10, 13, 3, 14, 15, 8, 4, 7, 1, 2)
PBOX = tuple(63 if i == 63 else 16*i % 63 for i in range(64))
_DDT = None

def digest(obj):
    return hashlib.sha256(json.dumps(obj, sort_keys=True, separators=(',', ':')).encode()).hexdigest()

def file_hash(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1048576), b''):
            h.update(block)
    return h.hexdigest()

def save(path, obj):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name+'.tmp')
    with tmp.open('w', encoding='utf-8', newline='\n') as f:
        json.dump(obj, f, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False)
        f.write('\n')
    os.replace(str(tmp), str(path))

def parse_state(text):
    if len(text) != 16 or set(text)-set('0123456789ABCDEF'):
        raise ValueError('Expected 16 uppercase hex digits, nibble 0 at right')
    value = int(text, 16)
    return [(value >> (4*c)) & 15 for c in range(16)]

def encode_state(values):
    if len(values) != 16 or any(not 0 <= v < 16 for v in values):
        raise ValueError('Invalid PRESENT state')
    return '{:016X}'.format(sum(v << (4*c) for c, v in enumerate(values)))

def forward(x):
    y = [0]*16
    for i, dst in enumerate(PBOX):
        y[dst//4] ^= ((x[i//4] >> (i % 4)) & 1) << (dst % 4)
    return y

def inverse(x):
    y = [0]*16
    for i, src in enumerate(PBOX):
        y[i//4] ^= ((x[src//4] >> (src % 4)) & 1) << (i % 4)
    return y

def ddt():
    global _DDT
    if _DDT is None:
        _DDT = [[sum((SBOX[x] ^ SBOX[x ^ a]) == b for x in range(16))
                 for b in range(16)] for a in range(16)]
    return _DDT

def expression(terms, x):
    value = 0
    for v, src, dst in terms:
        value ^= ((x[v] >> src) & 1) << dst
    return value

def prepare(pattern):
    pattern = tuple(pattern)
    if len(pattern) != ROUNDS or any(len(s) != 16 or set(s)-{'0', '1'} or '1' not in s for s in pattern):
        raise ValueError('Expected {} nonempty S-box INPUT activity masks'.format(ROUNDS))
    labels = [(tag, c) for tag, mask in ((0, pattern[0]), (ROUNDS, pattern[-1]))
              for c in range(16) if mask[c] == '1']
    ne = len(labels)
    labels += [(r, c) for r in range(1, ROUNDS) for c in range(16) if pattern[r][c] == '1']
    index = {label: i for i, label in enumerate(labels)}
    domains = []
    for r, c in labels:
        allowed = 15
        if 0 < r < ROUNDS:
            allowed = 0
            for src, dst in enumerate(PBOX):
                if dst//4 == c and pattern[r-1][src//4] == '1':
                    allowed |= 1 << (dst % 4)
        domain = [v for v in range(1, 16) if not v & ~allowed]
        if not domain:
            raise ValueError('Active nibble has no incoming bit from an active S-box')
        domains.append(domain)
    factors = []
    for r in range(ROUNDS):
        for c in range(16):
            if pattern[r][c] == '0':
                continue
            a = [[index[r, c], k, k] for k in range(4)]
            if r == ROUNDS-1:
                b = [[index[ROUNDS, c], k, k] for k in range(4)]
            else:
                b = [[index[r+1, PBOX[4*c+k]//4], PBOX[4*c+k] % 4, k]
                     for k in range(4) if (r+1, PBOX[4*c+k]//4) in index]
            if not b:
                raise ValueError('Active S-box output is forced zero by the next input activity')
            factors.append(dict(round=r, cell=c, a=a, b=b))
    return dict(id=digest(pattern), activity=list(pattern), labels=labels, nend=ne,
                domains=domains, d=len(labels)-ne, active=len(factors), factors=factors,
                cut_dimensions=[sum(tag == r for tag, _ in labels) for r in range(1, ROUNDS)])

def catalog(patterns):
    return _catalog_cached(tuple(sorted(set(tuple(p) for p in patterns))))

@lru_cache(maxsize=128)
def _catalog_cached(patterns):
    unique = sorted(set(tuple(p) for p in patterns))
    if len(unique) != 1:
        raise ValueError('CPU proposal catalog requires one activity pattern')
    m = prepare(unique[0])
    ne, n = m['nend'], len(m['labels'])
    pairs, triples = set(), set()
    for f in m['factors']:
        scope = sorted({t[0] for t in f['a']+f['b'] if t[0] >= ne})
        pairs.update(itertools.combinations(scope, 2))
        triples.update(itertools.combinations(scope, 3))
    model = {k:m[k] for k in ('id', 'activity', 'active', 'd', 'factors', 'cut_dimensions')}
    model.update(used=list(range(ne, n)),
                 blocks=[list(b) for b in sorted(pairs)[:256]]+[list(b) for b in sorted(triples)[:16]])
    obj = dict(schema='PRESENT_MF16_V2', rounds=ROUNDS, nend=ne, padded_dimension=n-ne,
               labels=m['labels'], domains=m['domains'], patterns=[model],
               model='independent_round_key_DDT_product',
                output_activity_fixed_during_search=False, original_DDT_factors_preserved=True)
    reduced = support_domains(obj)
    if reduced is None:
        raise ValueError('Activity pattern has no support under exact local DDT propagation')
    obj['domains'] = reduced
    obj['support_propagation'] = 'full-factor generalized arc consistency; legal target unchanged'
    obj['id'] = digest(obj)
    return obj

def native_text(cat):
    lines = ['PRESENT_MF16_V2 {} {} {}'.format(cat['nend'], cat['padded_dimension'], len(cat['patterns']))]
    lines += [' '.join(map(str, [len(d)]+d)) for d in cat['domains']]
    for m in cat['patterns']:
        lines.append('{} {} {} {} {}'.format(m['id'], m['active'], m['d'], len(m['factors']), len(m['blocks'])))
        lines.append(' '.join(map(str, m['used'])))
        for f in m['factors']:
            row = [f['round'], f['cell']]
            for side in ('a', 'b'):
                row += [len(f[side])]+[z for t in f[side] for z in t]
            lines.append(' '.join(map(str, row)))
        lines += [' '.join(map(str, [len(b)]+b)) for b in m['blocks']]
    return '\n'.join(lines)+'\n'

def endpoints(cat, text):
    if len(text) != cat['nend'] or set(text)-set('123456789ABCDEF'):
        raise ValueError('Invalid endpoint coordinates')
    xx, yy = [0]*16, [0]*16
    for (tag, cell), ch in zip(cat['labels'][:cat['nend']], text):
        (xx if tag == 0 else yy)[cell] = int(ch, 16)
    return dict(input=encode_state(xx), output=encode_state(forward(yy)), last_sbox_output=encode_state(yy))

def terminal_weight(cat, k, endpoint, internal):
    x = [int(c, 16) for c in endpoint+internal]
    if len(x) != len(cat['domains']) or any(v not in d for v, d in zip(x, cat['domains'])):
        raise ValueError('Malformed terminal coordinates or forced-zero-bit violation')
    table, weight = ddt(), 0
    for f in cat['patterns'][k]['factors']:
        a, b = expression(f['a'], x), expression(f['b'], x)
        count = table[a][b] if a and b else 0
        if not count:
            return None
        if count not in (2, 4):
            raise ValueError('Unexpected active DDT count')
        weight += 2 if count == 4 else 3
    return weight

@lru_cache(maxsize=2048)
def factor_relations(a_terms, b_terms):

    rows = []
    a_mask = sum(1 << t[2] for t in a_terms)
    b_mask = sum(1 << t[2] for t in b_terms)
    for a in range(1, 16):
        if a & ~a_mask:
            continue
        for b in range(1, 16):
            if b & ~b_mask or not ddt()[a][b]:
                continue
            required, valid = {}, True
            for terms, value in ((a_terms, a), (b_terms, b)):
                for var, src, dst in terms:
                    mask, bits = required.get(var, (0, 0))
                    bit = ((value >> dst) & 1) << src
                    if mask & (1 << src) and (bits & (1 << src)) != bit:
                        valid = False
                    required[var] = (mask | (1 << src), bits | bit)
            if valid:
                rows.append(tuple((v, sum(1 << x for x in range(1, 16)
                                           if x & mask == bits))
                                  for v, (mask, bits) in sorted(required.items())))
    return tuple(rows)

def support_domains(cat, endpoint=None, deadline=None):
    domains = [sum(1 << x for x in d) for d in cat['domains']]
    if endpoint is not None:
        if len(endpoint) != cat['nend']:
            raise ValueError('Wrong endpoint length')
        for v, char in enumerate(endpoint):
            domains[v] &= 1 << int(char, 16)
    if not all(domains):
        return None
    relations = [factor_relations(tuple(map(tuple, f['a'])), tuple(map(tuple, f['b'])))
                 for f in cat['patterns'][0]['factors']]
    changed = True
    while changed:
        check_deadline(deadline)
        changed = False
        for rows in relations:
            if not rows:
                return None
            supports = {v:0 for v, _ in rows[0]}
            for row in rows:
                if all(domains[v] & allowed for v, allowed in row):
                    for v, allowed in row:
                        supports[v] |= domains[v] & allowed
            for v, supported in supports.items():
                if not supported:
                    return None
                if supported != domains[v]:
                    domains[v] = supported
                    changed = True
    return [[x for x in range(1, 16) if bits & (1 << x)] for bits in domains]

def domain_reduction(cat, k, endpoint, deadline=None):
    endpoints(cat, endpoint)
    if k != 0:
        raise ValueError('One pattern per exact stratum is required')
    domains = support_domains(cat, endpoint, deadline)
    if domains is None:
        return dict(proven_zero=True, reason='empty exact DDT support relation')
    return dict(proven_zero=False,
                domains={str(v):domains[v] for v in cat['patterns'][k]['used']},
                catalog_id=cat['id'], pattern=k, endpoint=endpoint,
                reason='full-factor DDT support propagation; all original factors retained')

def domains_text(cat, k, reduction):
    if reduction['proven_zero']:
        raise ValueError('Cannot sample a proved-empty family')
    m = cat['patterns'][k]
    lines = ['PRESENT_DOMAINS_V2 {} {}'.format(k, m['d'])]
    for v in m['used']:
        d = reduction['domains'][str(v)]
        lines.append(' '.join(map(str, [v, len(d)]+d)))
    return '\n'.join(lines)+'\n'

def decode_trail(cat, k, endpoint, internal):
    weight = terminal_weight(cat, k, endpoint, internal)
    if weight is None:
        raise ValueError('Expected a legal DDT witness')
    x = [int(c, 16) for c in endpoint+internal]
    xx, yy = [[0]*16 for _ in range(ROUNDS)], [[0]*16 for _ in range(ROUNDS)]
    for f in cat['patterns'][k]['factors']:
        xx[f['round']][f['cell']] = expression(f['a'], x)
        yy[f['round']][f['cell']] = expression(f['b'], x)
    xx.append(forward(yy[-1]))
    if any(forward(yy[r]) != xx[r+1] for r in range(ROUNDS)):
        raise ValueError('Trail violates PRESENT bit permutation')
    states = [encode_state(row) for row in xx]
    ep = endpoints(cat, endpoint)
    if states[0] != ep['input'] or states[-1] != ep['output']:
        raise ValueError('Endpoint mismatch')
    return dict(states=states, weight=weight,
                pattern=[''.join('1' if v else '0' for v in row) for row in xx[:-1]])

def encode_trail(cat, k, states):
    if len(states) != ROUNDS+1:
        raise ValueError('Expected {} complete PRESENT difference states'.format(ROUNDS+1))
    xx = [parse_state(s) for s in states]
    pattern = [''.join('1' if v else '0' for v in row) for row in xx[:-1]]
    if pattern != cat['patterns'][k]['activity']:
        raise ValueError('Witness has a different input activity pattern')
    last = inverse(xx[-1])
    values = [(last if r == ROUNDS else xx[r])[c] for r, c in cat['labels']]
    if any(v not in domain for v, domain in zip(values, cat['domains'])):
        raise ValueError('Witness outside exact bit support')
    text = ''.join('{:X}'.format(v) for v in values)
    e, inside = text[:cat['nend']], text[cat['nend']:]
    checked = decode_trail(cat, k, e, inside)
    if checked['states'] != list(states):
        raise ValueError('Coordinate encoding changed the trail')
    return dict(coordinates=e, witness_internal=inside, witness_pattern=k,
                witness_weight=checked['weight'], endpoints=endpoints(cat, e),
                witness_numerator=str(1 << (3*cat['patterns'][k]['active']-checked['weight'])))

def check_deadline(deadline):
    if deadline is not None and time.monotonic() >= deadline:
        raise TimeoutError('CAMPAIGN_TIME_LIMIT')

def pattern_from_states(states):
    if len(states) != ROUNDS+1:
        raise ValueError('Wrong number of complete-round states')
    return [''.join('1' if v else '0' for v in parse_state(s)) for s in states[:-1]]


import math
from fractions import Fraction
from model import ROUNDS, catalog, digest, domain_reduction, encode_trail, pattern_from_states, check_deadline

def product(values):
    out = 1
    for v in values:
        out *= v
    return out

def prepare_family(states, deadline=None):
    pattern = pattern_from_states(states)
    cat = catalog([pattern])
    witness = encode_trail(cat, 0, states)
    reduced = domain_reduction(cat, 0, witness['coordinates'], deadline)
    if reduced['proven_zero']:
        raise ArithmeticError('Domain propagation eliminated a legal witness')
    m = cat['patterns'][0]
    used = m['used']
    remap = {v: j for j, v in enumerate(used)}
    domains = [reduced['domains'][str(v)] for v in used]
    digits = [int(x,16) for x in witness['coordinates'] + witness['witness_internal']]
    if any(digits[v] not in domains[remap[v]] for v in used):
        raise ArithmeticError('Domain propagation excluded a witness coordinate')
    factors = []
    for f in m['factors']:
        constants, forms = [], []
        for side in ('a', 'b'):
            constant, form = 0, []
            for v, src, dst in f[side]:
                if v < cat['nend']:
                    constant ^= ((digits[v] >> src) & 1) << dst
                else:
                    form.append([remap[v], src, dst])
            constants.append(constant)
            forms.append(sorted(form))
        factors.append(dict(a=constants[0], b=constants[1], av=forms[0], bv=forms[1]))
    return dict(nv=len(used), active=m['active'], denominator_power=3*m['active'],
                domains=domains, factors=factors, witness=[digits[v] for v in used],
                witness_weight=witness['witness_weight'], witness_numerator=witness['witness_numerator'])

def adaptive_text(model, boxes):
    lines = ['PRESENT_ADAPTIVE16_V1 {} {} {}'.format(model['nv'], model['active'], len(boxes))]
    for domain in model['domains']:
        lines.append(' '.join(map(str, [len(domain)] + domain)))
    lines.append(' '.join(map(str, model['witness'])))
    for f in model['factors']:
        lines.append(' '.join(map(str, [f['a'], f['b'], len(f['av'])] + [x for t in f['av'] for x in t] +
                                  [len(f['bv'])] + [x for t in f['bv'] for x in t])))
    for box in boxes:
        fields = [str(len(box))]
        for v, mask in box:
            fields.extend([str(v), mask])
        lines.append(' '.join(fields))
    return '\n'.join(lines) + '\n'

def box_domains(model, box):
    masks = [sum(1 << x for x in d) for d in model['domains']]
    seen = set()
    for v, text in box:
        if not isinstance(v, int) or v in seen or not 0 <= v < len(masks):
            raise ArithmeticError('Invalid adaptive variable')
        seen.add(v)
        m = int(text, 16)
        if not m or m & ~masks[v]:
            raise ArithmeticError('Invalid adaptive subdomain')
        masks[v] = m
    return masks

def popcount(n):
    return bin(n).count('1')

def verify_partition(model, parent, result):

    original = box_domains(model, parent)
    volume = product(popcount(m) for m in original)
    children = [box_domains(model, b) for b in result['frontier']]
    for i, child in enumerate(children):
        if any(m & ~old for m, old in zip(child, original)):
            raise ArithmeticError('Checkpoint escaped its input box')
        for other in children[:i]:
            if all(a & b for a, b in zip(child, other)):
                raise ArithmeticError('Overlapping adaptive checkpoints')
    closed = int(result['closed_volume'])
    if closed < 0 or int(result['initial_volume']) != volume or closed + sum(
            product(popcount(m) for m in d) for d in children) != volume:
        raise ArithmeticError('Checkpoint assignment accounting failed')
    if int(result['numerator']) < 0 or result['denominator_power'] != model['denominator_power']:
        raise ArithmeticError('Invalid exact checkpoint mass')
    if result['status'] == 'EXACT_DONE' and children:
        raise ArithmeticError('Incomplete exact checkpoint marked complete')
    if not closed and int(result['numerator']):
        raise ArithmeticError('Positive mass without a closed assignment')

def dyadic_record(numerator, exponent):
    n = int(numerator)
    if n < 0 or n > (1 << exponent):
        raise ArithmeticError('Probability outside [0,1]')
    if not n:
        return dict(numerator='0', denominator_power=0, F=None)
    shift = min(exponent, (n & -n).bit_length()-1)
    n >>= shift
    exponent -= shift
    return dict(numerator=str(n), denominator_power=exponent,
                F=exponent-math.log2(n))

def sum_dyadics(records):
    exponent = max((r['denominator_power'] for r in records), default=0)
    n = sum(int(r['numerator']) << (exponent-r['denominator_power']) for r in records)
    return dyadic_record(n, exponent)


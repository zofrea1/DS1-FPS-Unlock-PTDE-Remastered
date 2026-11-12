# ticksim.py: SpEffect tick rates of the game's node update (PTDE 0xDBE7B0), original vs the fix, across frame rates.
import numpy as np
f32 = np.float32
rng = np.random.default_rng(7)
def native_frames(life, rate=30.0):
    step = f32(1.0 / rate); t = f32(life); n = 0
    while t > f32(0) and n < 4096:
        t = f32(t - step); n += 1
    return n
def fixed_timer(old, I, dt, rate=30.0):
    if not (I > 0) or dt >= 0.8 / rate: return f32(I)
    P = native_frames(I, rate) / rate
    late = min(max(-float(old), 0.0), dt)
    return f32(P - late)
def period(I, fps, fixed, seconds=40.0, jitter=0.0):
    t = f32(0); times = []; tt = 0.0
    while tt < seconds:
        dt = 1.0 / fps
        if jitter: dt *= float(rng.uniform(1 - jitter, 1 + jitter))
        if not (t > f32(0)):
            times.append(tt)
            t = fixed_timer(t, I, dt) if fixed else f32(I)
        if t > f32(0):
            t = f32(float(t) - float(f32(dt)))
        tt += dt
    return (times[-1] - times[0]) / (len(times) - 1)
fpss = (30, 60, 75, 90, 120, 144, 165, 240, 360, 1000)
print('             ' + ' '.join('%6s' % f for f in fpss) + '  144j   240j  (tick rate vs the original at 30 FPS)')
for I in (0.03, 0.1, 0.15, 0.2, 0.5, 0.8, 1.0, 3.0):
    base = period(I, 30, False)
    for fixed in (False, True):
        row = [base / period(I, f, fixed) for f in fpss] + [base / period(I, 144, fixed, jitter=0.25), base / period(I, 240, fixed, jitter=0.25)]
        print('I=%.2f %s ' % (I, 'fix ' if fixed else 'orig') + ' '.join('%+5.1f%%' % (100 * (x - 1)) for x in row))

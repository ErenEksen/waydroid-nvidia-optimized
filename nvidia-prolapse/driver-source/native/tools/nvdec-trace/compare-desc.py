import sys, struct
def descs(path):
  out, cur = [], None
  for line in open(path):
    if line.startswith('DESC'): cur = []; out.append(cur); continue
    if cur is not None and line.startswith('  '): cur.extend(int(w, 16) for w in line.split()); continue
    cur = None
  return [struct.pack('<%dI' % len(d), *d) for d in out]
stock = descs(sys.argv[1]); fx = open(sys.argv[2], 'rb').read()
ours = [fx[i:i+0x300] for i in range(0, len(fx), 0x300)]
n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x180
for si, s in enumerate(stock):
  best = min(range(len(ours)), key=lambda k: sum(a != b for a, b in zip(s[:n], ours[k][:n])))
  diff = [i for i in range(0, n, 4) if s[i:i+4] != ours[best][i:i+4]]
  print(f"stock desc {si}: closest fixture frame {best}, differing dwords in first {n:#x} bytes: {len(diff)} at {[hex(d) for d in diff[:12]]}")

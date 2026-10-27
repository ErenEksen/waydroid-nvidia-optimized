import sys, re
# Decode NVDEC (subchannel 4) methods of the largest PB per video channel in an rmtrace file.
lines = open(sys.argv[1]).read().split('\n')
pbs = []
for i, l in enumerate(lines):
  m = re.match(r'T \d+ GPFIFO chan (\S+) class \S+ engine (0x1[3-6]) .* words (\d+)', l)
  if m and int(m[3]) > 50:
    words = []
    j = i + 2
    while j < len(lines) and lines[j].startswith('  '): words += [int(w, 16) for w in lines[j].split()]; j += 1
    pbs.append(words)
w = pbs[0]; i = 0; out = []
while i < len(w):
  h = w[i]; op, n, sc, m = h >> 29, (h >> 16) & 0x1fff, (h >> 13) & 7, (h & 0xfff) << 2
  if op == 4: out.append(f"sc{sc} {m:#05x} = imm {n:#x}"); i += 1; continue
  vals = w[i+1:i+1+n]
  out.append(f"sc{sc} {m:#05x} {'NONINC' if op == 3 else 'INC' if op == 1 else 'op'+str(op)} " + ' '.join(f"{v:08x}" for v in vals))
  i += 1 + n
print(f"{len(pbs)} large video PBs; first one decoded:"); print('\n'.join(out))

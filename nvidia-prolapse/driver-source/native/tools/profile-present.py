#!/usr/bin/env python3
"""Bounded live presentation timing; always remove the temporary perf probes."""
import argparse,json,re,statistics,subprocess,struct,os
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('pid',type=int);p.add_argument('stage',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
def run(*args,**kw):return subprocess.run(list(map(str,args)),check=True,**kw)
events=[]
try:
 for name,lib,symbol in [('release','hw/vulkan.nvidia.so','nvhal_QueueSignalReleaseImageANDROID'),('submit','libvulkan_nouveau.so','vk_common_QueueSubmit'),('fencefd','libvulkan_nouveau.so','vk_common_GetFenceFdKHR')]:
  path=a.stage/'vendor_extra/lib64'/lib
  for suffix,where in [('',symbol),('_ret',symbol+'%return')]:
   event='vvfix:'+name+suffix
   symbols=subprocess.check_output(['nm','-P',str(path)],text=True)
   address=int(next(line.split()[2] for line in symbols.splitlines() if line.split()[0]==symbol),16)
   elf=path.read_bytes(); phoff=struct.unpack_from('<Q',elf,32)[0]; ents,num=struct.unpack_from('<HH',elf,54)
   offset=None
   for i in range(num):
    pos=phoff+i*ents
    kind=struct.unpack_from('<I',elf,pos)[0]; fileoff,vaddr=struct.unpack_from('<QQ',elf,pos+8); size=struct.unpack_from('<Q',elf,pos+32)[0]
    if kind==1 and vaddr<=address<vaddr+size:offset=fileoff+address-vaddr;break
   if offset is None:raise RuntimeError('symbol lacks file-backed load segment')
   trace=os.open('/sys/kernel/tracing/uprobe_events',os.O_WRONLY)
   try:os.write(trace,(('r' if suffix else 'p')+':'+event.replace(':','/')+' '+str(path)+':'+hex(offset)+'\n').encode())
   finally:os.close(trace)
   events.append(event)
 data=Path('/tmp')/(a.output.name+'.data')
 run('perf','record','-e','vvfix:*','-p',a.pid,'-o',data,'--','sleep','3')
 output=subprocess.check_output(['perf','script','-i',str(data)],text=True)
 a.output.with_suffix('.events.txt').write_text(output)
 start={};inside=set();values={}
 for line in output.splitlines():
  m=re.search(r'\s(\d+)\s+(?:\[\d+\]\s+)?(\d+\.\d+):\s+vvfix:(\w+):',line)
  if not m:continue
  tid,t,event=int(m[1]),float(m[2]),m[3]
  name=event.split('_ret')[0]
  if '_ret' not in event:
   if name=='release':inside.add(tid)
   start[(tid,name)]=(t,'present' if tid in inside else 'other')
  elif (tid,name) in start:
   begin,kind=start.pop((tid,name));values.setdefault(name+'/'+kind,[]).append((t-begin)*1000)
   if name=='release':inside.discard(tid)
 summary={k:dict(count=len(v),mean_ms=statistics.mean(v),median_ms=statistics.median(v),p95_ms=sorted(v)[int(.95*(len(v)-1))]) for k,v in values.items()}
 a.output.with_suffix('.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))
finally:
 for event in reversed(events):subprocess.run(['perf','probe','--del',event+'*'],check=False)

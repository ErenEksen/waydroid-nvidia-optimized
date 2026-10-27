#!/usr/bin/env python3
"""Independent exact-tag public-header C oracle for CPU-unmap and CE payloads.
Offline only: reads prefetched git blobs, compiles/runs pure m32/m64 C printers.
"""
import concurrent.futures, hashlib, json, os, re, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[4]
REPO=ROOT/'build/open-gpu-kernel-modules'
OUT=ROOT/'build/rm-abi-stability/lifecycle'
INCLUDES=['src/common/sdk/nvidia/inc','src/common/inc','src/nvidia/arch/nvalloc/unix/include','kernel-open/common/inc']
ENV=dict(os.environ,GIT_NO_LAZY_FETCH='1')
def git(*args):return subprocess.check_output(['git','-C',str(REPO),*args],env=ENV)
def one(tag):
 out=OUT/tag;out.mkdir(parents=True,exist_ok=True)
 tree={}
 for rec in git('ls-tree','-r','-z',tag,'--',*INCLUDES).split(b'\0'):
  if not rec:continue
  meta,path=rec.split(b'\t',1)
  if path.endswith(b'.h'):tree[path.decode()]=meta.split()[-1].decode()
 used={}
 with tempfile.TemporaryDirectory(prefix='rm-lifecycle-') as tmp:
  tmp=Path(tmp)
  def copy(name,parent=''):
   choices=[parent+'/'+name]+[p+'/'+name for p in INCLUDES]
   path=next((p for p in choices if p in tree),None)
   if path is None or path in used:return
   raw=git('cat-file','blob',tree[path]);used[path]={'blob':tree[path],'sha256':hashlib.sha256(raw).hexdigest()}
   dest=tmp/path;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(raw)
   for inc in re.findall(rb'^\s*#\s*include\s*[<"]([^>"\n]+)',raw,re.M):copy(inc.decode(),str(Path(path).parent))
  headers=['nv-ioctl.h','nv-ioctl-numbers.h','class/cl0005.h','ctrl/ctrl2080/ctrl2080event.h','nvmisc.h','ctrl/ctrl2080/ctrl2080perf.h','nvos.h','nv_escape.h','class/clb0b5sw.h','class/clc0b5sw.h','class/clc1b5sw.h','ctrl/ctrl2080/ctrl2080gpu.h','class/cl2080.h']
  if any(p+'/class/cl2080_notification.h' in tree for p in INCLUDES):headers.append('class/cl2080_notification.h')
  for stem in ['a06f','a16f','b06f','c06f','c36f','c46f','c56f','c86f','c96f','ca6f']:
   header='class/cl'+stem+'sw.h'
   if any(p+'/'+header in tree for p in INCLUDES):headers.append(header)
  for h in headers:copy(h)
  body=['#include <stdio.h>','#include <stddef.h>','#include <sys/ioctl.h>']+['#include <'+h+'>' for h in headers]
  body+=['#define S(T) printf(#T "\\tsize=%zu align=%zu\\n",sizeof(T),_Alignof(T))', '#define F(T,f) printf(#T "." #f "\\toff=%zu len=%zu\\n",offsetof(T,f),sizeof(((T*)0)->f))','#define K(k) printf(#k "\\t0x%llx\\n",(unsigned long long)(k))','int main(void) {']
  for typ,fields in [('nv_ioctl_alloc_os_event_t',['hClient','hDevice','fd','Status']),('nv_ioctl_free_os_event_t',['hClient','hDevice','fd','Status']),('NV0005_ALLOC_PARAMETERS',['hParentClient','hSrcResource','hClass','notifyIndex','data']),('NV2080_CTRL_EVENT_SET_NOTIFICATION_PARAMS',['event','action','bNotifyState','info32','info16']),('NV2080_CTRL_EVENT_SET_TRIGGER_FIFO_PARAMS',['hEvent']),('NV2080_CTRL_PERF_BOOST_PARAMS',['flags','duration']),('NV2080_CTRL_GPU_GET_ENGINE_CLASSLIST_PARAMS',['engineType','numClasses','classList']),('NV2080_CTRL_GPU_GET_ENGINE_PARTNERLIST_PARAMS',['engineType','partnershipClassId','runqueue','numPartners','partnerList']),('NVOS34_PARAMETERS',['hClient','hDevice','hMemory','pLinearAddress','status','flags']),('NVB0B5_ALLOCATION_PARAMETERS',['version','engineType']),('NVC0B5_ALLOCATION_PARAMETERS',['version','engineType']),('NVC1B5_ALLOCATION_PARAMETERS',['version','engineType'])]:
   body += [f'S({typ});']+[f'F({typ},{f});' for f in fields]
  body+=['K(NV_ESC_RM_UNMAP_MEMORY);','K(NVB0B5_ALLOCATION_PARAMETERS_VERSION_0);','K(NVB0B5_ALLOCATION_PARAMETERS_VERSION_1);','K(NV2080_CTRL_CMD_GPU_GET_ENGINE_PARTNERLIST);','K(NV2080_CTRL_CMD_GPU_GET_ENGINE_CLASSLIST);','K(NV2080_ENGINE_TYPE_GRAPHICS);','K(NV2080_CTRL_GPU_MAX_ENGINE_PARTNERS);',*[f'#ifdef NV2080_ENGINE_TYPE_COPY{i}\nK(NV2080_ENGINE_TYPE_COPY{i});\n#endif' for i in range(20)],'printf("ioctl_rm_unmap_memory\\t0x%lx\\n",(unsigned long)_IOWR(\'F\',NV_ESC_RM_UNMAP_MEMORY,NVOS34_PARAMETERS));','return 0; }']
  body[-1:-1] = [f'#ifdef {name}\nK({name});\n#endif' for name in [
   'NV_ESC_ALLOC_OS_EVENT','NV_ESC_FREE_OS_EVENT','NV01_EVENT','NV01_EVENT_OS_EVENT','NV01_EVENT_WITHOUT_EVENT_DATA','NV01_EVENT_SUBDEVICE_SPECIFIC','NV01_EVENT_NONSTALL_INTR','NV2080_NOTIFIERS_FIFO_EVENT_MTHD','NV2080_CTRL_CMD_EVENT_SET_NOTIFICATION','NV2080_CTRL_CMD_EVENT_SET_TRIGGER_FIFO','NV2080_CTRL_EVENT_SET_NOTIFICATION_ACTION_DISABLE','NV2080_CTRL_EVENT_SET_NOTIFICATION_ACTION_REPEAT',*[f'NV{stem.upper()}_NOTIFIERS_RC' for stem in ['a06f','a16f','b06f','c06f','c36f','c46f','c56f','c86f','c96f','ca6f']],
   'NV2080_CTRL_CMD_PERF_BOOST','NV2080_CTRL_PERF_BOOST_FLAGS_CMD_CLEAR',
   'NV2080_CTRL_PERF_BOOST_FLAGS_CMD_BOOST_TO_MAX','NV2080_CTRL_PERF_BOOST_FLAGS_ASYNC_YES',
   'NV2080_CTRL_PERF_BOOST_DURATION_MAX']]
  body[-1:-1] = ['#ifdef NV2080_CTRL_PERF_BOOST_FLAGS_ASYNC_YES\nprintf("PERF_BOOST_ASYNC_FLAG\\t0x%llx\\n", (unsigned long long)DRF_DEF(2080,_CTRL_PERF_BOOST_FLAGS,_ASYNC,_YES));\n#endif']
  source='\n'.join(body)+'\n';src=tmp/'probe.c';src.write_text(source)
  for abi in ['m32','m64']:
   exe=tmp/abi
   cmd=['cc','-'+abi,'-std=c11',*[f'-I{tmp/p}' for p in INCLUDES],str(src),'-o',str(exe)]
   run=subprocess.run(cmd,capture_output=True,text=True)
   if run.returncode:raise RuntimeError(tag+' '+run.stderr)
   (out/f'probe-{abi}.tsv').write_bytes(subprocess.check_output([str(exe)]))
  assert (out/'probe-m32.tsv').read_bytes()==(out/'probe-m64.tsv').read_bytes(),tag
  (out/'source.c').write_text(source)
 (out/'provenance.json').write_text(json.dumps({'tag':tag,'commit':git('rev-parse',tag+'^{commit}').decode().strip(),'headers':used},indent=1)+'\n')
 return tag
if __name__=='__main__':
 tags=sorted(p.name for p in (ROOT/'build/rm-abi-stability/ranges/probe').iterdir() if (p/'probe-m64.tsv').exists())
 assert tags==sorted(git('tag').decode().split()),'public tag census differs from the OGKM tag list'
 OUT.mkdir(parents=True,exist_ok=True)
 with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
  for tag in pool.map(one,tags):print(tag,flush=True)

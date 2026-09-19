#!/usr/bin/env python3
"""A/B CPU bookkeeping microbenchmark, not an Android FPS measurement.
Uses actual old/new vn_ring functions and Mesa's real list implementation.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
tree = Path(os.environ.get('MESA_TREE', repo/'.work/mesa'))
old = subprocess.check_output(['git','-C',str(tree),'show','HEAD:src/virtio/vulkan/vn_ring.c'],text=True)
new = (tree/'src/virtio/vulkan/vn_ring.c').read_text()

def function(source, name):
    index=source.index(name+'(')
    start=source.rfind('\nstatic ',0,index)+1
    end=source.index('{',index)+1
    depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start:end]

def program(source, fixed):
    constants='\n'.join(re.findall(r'^#define VN_RING_SUBMIT_POOL_.*$',source,re.M))
    struct=re.search(r'struct vn_ring_submit \{.*?\n\};',source,re.S).group()
    members=('struct list_head free_submits[VN_RING_SUBMIT_POOL_CLASSES]; uint32_t free_submit_count[VN_RING_SUBMIT_POOL_CLASSES];'
             if fixed else 'struct list_head free_submits;')
    init=('for(unsigned i=0;i<VN_RING_SUBMIT_POOL_CLASSES;i++)list_inithead(&r.free_submits[i]);'
          if fixed else 'list_inithead(&r.free_submits);')
    count=('for(unsigned i=0;i<VN_RING_SUBMIT_POOL_CLASSES;i++) { list_for_each_entry_safe(struct vn_ring_submit,s,&r.free_submits[i],head) { cached++;free(s); } }'
           if fixed else 'list_for_each_entry_safe(struct vn_ring_submit,s,&r.free_submits,head) { cached++;free(s); }')
    names=['vn_ring_ge_seqno']
    if fixed:names+=['vn_ring_submit_pool_class','vn_ring_recycle_submit']
    names+=['vn_ring_retire_submits','vn_ring_get_submit']
    return '''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "util/list.h"
#define MAX2(a,b) ((a)>(b)?(a):(b))
'''+constants+'''
struct vn_renderer_shmem { unsigned refs; };
struct vn_renderer { int unused; };
struct vn_instance { struct vn_renderer *renderer; };
'''+struct+'''
struct vn_ring {struct vn_instance *instance;uint32_t cur;struct list_head submits;'''+members+'''};
static size_t allocs;
static void *count_malloc(size_t n) {allocs++;return malloc(n);}
static void vn_renderer_shmem_unref(struct vn_renderer *r,struct vn_renderer_shmem *s){(void)r;assert(s->refs);s->refs--;}
#define malloc count_malloc
'''+ '\n'.join(function(source,n) for n in names)+'''
#undef malloc
static double now(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char **argv) {
    unsigned rounds=argc>1?atoi(argv[1]):10000;
    struct vn_renderer renderer;struct vn_instance instance={&renderer};
    struct vn_ring r={.instance=&instance};list_inithead(&r.submits);
'''+init+'''
    struct vn_renderer_shmem shmem={0};
    double start=now();
    for(unsigned i=0;i<rounds;i++)for(unsigned refs=0;refs<2;refs++) {
        struct vn_ring_submit *s=vn_ring_get_submit(&r,refs);assert(s);
        s->shmem_count=refs;s->seqno=++r.cur;
        if(refs){shmem.refs++;s->shmems[0]=&shmem;}
        list_addtail(&s->head,&r.submits);vn_ring_retire_submits(&r,r.cur);
    }
    double elapsed=now()-start;unsigned cached=0;
    assert(shmem.refs==0&&list_is_empty(&r.submits));
'''+count+'''
    printf("{\\"rounds\\":%u,\\"elapsed_ms\\":%.6f,\\"allocations\\":%zu,\\"cached_records\\":%u}\\n",rounds,elapsed*1000,allocs,cached);
    return 0;
}
'''
results=[]
with tempfile.TemporaryDirectory(prefix='nvwd-ring-bench-') as tmp:
    tmp=Path(tmp)
    for name,source,fixed in [('before',old,False),('after',new,True)]:
        src=tmp/(name+'.c');exe=tmp/name;src.write_text(program(source,fixed))
        subprocess.run(['cc','-O2','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(tree/'src'),str(src),'-o',str(exe)],check=True)
        for rounds in (1000,10000,30000):
            for repeat in range(3):
                row=json.loads(subprocess.check_output([str(exe),str(rounds)],text=True,timeout=30))
                results.append({'version':name,'repeat':repeat,**row})
print(json.dumps({'measurement':'CPU submit bookkeeping only; no GPU/Android/FPS claim','results':results},indent=2))

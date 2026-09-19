#!/usr/bin/env python3
"""Compile actual vn_ring pool/retirement functions with ASan/UBSan and fake shmem.
No GPU or installed service access. Missing source/compiler is a failure.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
tree = Path(os.environ.get('MESA_TREE', repo/'.work/mesa'))
source = (tree/'src/virtio/vulkan/vn_ring.c').read_text()
def function(name):
    end_name = source.index(name+'(')
    start = source.rfind('\nstatic ', 0, end_name)+1
    brace = source.index('{', end_name)
    depth = 1
    end = brace+1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end += 1
    return source[start:end]+'\n'
struct = re.search(r'struct vn_ring_submit \{.*?\n\};', source, re.S).group()
constants = '\n'.join(re.findall(r'^#define VN_RING_SUBMIT_POOL_.*$', source, re.M))
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include "util/list.h"
'''+constants+r'''
struct vn_renderer_shmem { unsigned refs; };
struct vn_renderer { int unused; };
struct vn_instance { struct vn_renderer *renderer; };
'''+struct+r'''
struct vn_ring {
    struct vn_instance *instance;
    uint32_t cur;
    struct list_head submits;
    struct list_head free_submits[VN_RING_SUBMIT_POOL_CLASSES];
    uint32_t free_submit_count[VN_RING_SUBMIT_POOL_CLASSES];
};
static unsigned unrefs;
static void vn_renderer_shmem_unref(struct vn_renderer *r, struct vn_renderer_shmem *s) {
    (void)r; assert(s->refs); s->refs--; unrefs++;
}
static bool fail_alloc;
static size_t allocations;
static void *test_malloc(size_t n) {
    if (fail_alloc) return NULL;
    allocations++;
    return malloc(n);
}
#define malloc test_malloc
'''
body=''.join(function(n) for n in ('vn_ring_ge_seqno','vn_ring_submit_pool_class',
     'vn_ring_recycle_submit','vn_ring_retire_submits','vn_ring_get_submit'))
tests=r'''
#undef malloc
static void init(struct vn_ring *r) {
    static struct vn_renderer renderer;
    static struct vn_instance instance = { &renderer };
    *r = (struct vn_ring){.instance=&instance};
    list_inithead(&r->submits);
    for(unsigned i=0;i<VN_RING_SUBMIT_POOL_CLASSES;i++)list_inithead(&r->free_submits[i]);
}
static void destroy(struct vn_ring *r) {
    assert(list_is_empty(&r->submits));
    for(unsigned i=0;i<VN_RING_SUBMIT_POOL_CLASSES;i++) {
        unsigned count=0;
        list_for_each_entry_safe(struct vn_ring_submit,s,&r->free_submits[i],head) {
            count++;free(s);
        }
        assert(count==r->free_submit_count[i]);
        assert(count<=VN_RING_SUBMIT_POOL_LIMIT);
    }
}
static void retire(struct vn_ring *r, struct vn_ring_submit *s, unsigned used) {
    static struct vn_renderer_shmem shmem;
    assert(s && s->shmem_capacity>=used);
    shmem.refs=used;
    s->shmem_count=used;
    for(unsigned i=0;i<used;i++)s->shmems[i]=&shmem;
    s->seqno=++r->cur;
    list_addtail(&s->head,&r->submits);
    vn_ring_retire_submits(r,r->cur);
    assert(shmem.refs==0);
}
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static struct vn_ring shared;
static void *worker(void *unused) {
    (void)unused;
    for(unsigned i=0;i<20000;i++) {
        pthread_mutex_lock(&mutex);
        unsigned n=i%130;
        retire(&shared,vn_ring_get_submit(&shared,n),n);
        pthread_mutex_unlock(&mutex);
    }
    return NULL;
}
int main(void) {
    struct vn_ring r;init(&r);
    // Allocation capacity must survive a zero-reference payload.
    struct vn_ring_submit *s=vn_ring_get_submit(&r,2);
    retire(&r,s,0);
    assert(vn_ring_get_submit(&r,2)==s);
    retire(&r,s,2);
    for(unsigned n=0;n<260;n++) {
        s=vn_ring_get_submit(&r,n);
        assert(s && s->shmem_capacity>=n);
        retire(&r,s,n);
    }
    // In-flight records cannot be reused; cache size is bounded after a burst.
    for(unsigned i=0;i<10000;i++) {
        s=vn_ring_get_submit(&r,0);s->shmem_count=0;s->seqno=r.cur+10;
        list_addtail(&s->head,&r.submits);
    }
    unsigned before=unrefs;
    r.cur+=10;
    vn_ring_retire_submits(&r,r.cur-10);
    assert(unrefs==before && !list_is_empty(&r.submits));
    vn_ring_retire_submits(&r,r.cur);
    assert(list_is_empty(&r.submits));
    assert(r.free_submit_count[0]==VN_RING_SUBMIT_POOL_LIMIT);
    // Warm mixed capacities remain allocation-free over a million submissions.
    size_t start_alloc=allocations;
    for(unsigned i=0;i<1000000;i++)retire(&r,vn_ring_get_submit(&r,i%129),i%129);
    assert(allocations==start_alloc);
    fail_alloc=true;
    assert(vn_ring_get_submit(&r,1000)==NULL);
    fail_alloc=false;
    destroy(&r);
    // Sequence wrap and ref release still follow the actual retirement code.
    init(&r);r.cur=UINT32_MAX-2;
    retire(&r,vn_ring_get_submit(&r,1),1);
    retire(&r,vn_ring_get_submit(&r,1),1);
    retire(&r,vn_ring_get_submit(&r,1),1);
    assert(r.cur==0);destroy(&r);
    init(&shared);pthread_t threads[4];
    for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,worker,NULL));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    destroy(&shared);
    puts("PASS: capacity reuse, bounds, retirement, wrap, OOM, 1M reuse, 4 locked threads");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nvwd-pool-') as tmp:
    path=Path(tmp);(path/'test.c').write_text(preamble+body+tests)
    subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',
                    '-I'+str(tree/'src'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=45)

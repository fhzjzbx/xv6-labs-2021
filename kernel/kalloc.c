// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

#define NPHYPAGES ((PHYSTOP - KERNBASE) / PGSIZE)

struct {
  struct spinlock lock;
  int count[NPHYPAGES];
} pageref;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&pageref.lock, "pageref");
  freerange(end, (void*)PHYSTOP);
}

static int
paindex(uint64 pa)
{
  if(pa < KERNBASE || pa >= PHYSTOP || pa % PGSIZE != 0)
    panic("paindex");

  return (pa - KERNBASE) / PGSIZE;
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    acquire(&pageref.lock);
    pageref.count[paindex((uint64)p)] = 1;
    release(&pageref.lock);

    kfree(p);
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int refs;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&pageref.lock);
  pageref.count[paindex((uint64)pa)]--;
  refs = pageref.count[paindex((uint64)pa)];
  release(&pageref.lock);

  if(refs > 0)
    return;

  if(refs < 0)
    panic("kfree ref");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r){
    acquire(&pageref.lock);
    pageref.count[paindex((uint64)r)] = 1;
    release(&pageref.lock);

    memset((char*)r, 5, PGSIZE); // fill with junk
  }

  return (void*)r;
}

void
krefinc(uint64 pa)
{
  acquire(&pageref.lock);
  pageref.count[paindex(pa)]++;
  release(&pageref.lock);
}

int
krefcnt(uint64 pa)
{
  int n;

  acquire(&pageref.lock);
  n = pageref.count[paindex(pa)];
  release(&pageref.lock);

  return n;
}
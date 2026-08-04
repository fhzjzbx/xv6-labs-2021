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
} kmem[NCPU];


void
kinit()
{
  for(int i = 0; i < NCPU; i++){
    initlock(&kmem[i].lock, "kmem");
    kmem[i].freelist = 0;
  }
  
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int id;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  /*
   * 取得 CPU 编号并使用 per-CPU 数据期间禁止中断
   * 避免当前执行流迁移到其他 CPU
   */
  push_off();
  id = cpuid();

  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);

  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  struct run *batch;
  struct run *tail;
  struct run *p;
  int id;
  int i;
  int j;
  int count;
  int steal_count;

  r = 0;

  /*
   * 关闭中断，保证 cpuid() 的结果在整个 per-CPU 操作期间有效
   */
  push_off();
  id = cpuid();


  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);

  /*
   * 本地链表为空时，依次尝试从其他 CPU 窃取页面
   */
  if(r == 0){
    for(i = 0; i < NCPU; i++){
      /*
       * 不需要从自己的空链表窃取。
       */
      if(i == id)
        continue;
      
      batch = 0;
      tail = 0;

      /*
       * 只持有被窃取 CPU 的锁
       * 统计其链表长度并摘下一半页面
       */
      acquire(&kmem[i].lock);

      if(kmem[i].freelist != 0){
        count = 0;

        for(p = kmem[i].freelist; p != 0; p = p->next)
          count++;
        
        /*
         * 至少窃取一个页面
         */
        steal_count = count / 2;
        if(steal_count < 1)
          steal_count = 1;

        batch = kmem[i].freelist;
        tail = batch;

        /*
         * 找到被窃取批次的最后一个节点
         */
        for(j = 1; j < steal_count; j++)
          tail = tail->next;

        /*
         * 将批次从目标 CPU 的链表中摘下
         */
        kmem[i].freelist = tail->next;
        tail->next = 0;
      }

      release(&kmem[i].lock);

      /*
       * 已经成功窃取一批页面。
       */
      if(batch != 0){
        /*
         * 此时没有持有其他 CPU 的锁
         * 因而不会形成两把 kmem 锁相互等待的死锁
         */
        acquire(&kmem[id].lock);

        /*
         * 将窃取到的批次接到本地链表表头
         */
        tail->next = kmem[id].freelist;
        kmem[id].freelist = batch;

        /*
         * 立即从新获得的本地页面中取出一个作为本次结果
         */
        r = kmem[id].freelist;
        kmem[id].freelist = r->next;

        release(&kmem[id].lock);

        break;
      }
    }
  }

  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

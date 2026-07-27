#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "date.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;


  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}


#ifdef LAB_PGTBL
uint64
sys_pgaccess(void)
{
  uint64 start;
  uint64 user_mask;
  int npages;
  uint mask = 0;
  struct proc *p = myproc();

  // 第 0 个参数：开始虚拟地址
  argaddr(0, &start);

  // 第 1 个参数：要检查的页面数量
  argint(1, &npages);

  // 第 2 个参数：用户空间结果地址
  argaddr(2, &user_mask);

  /*
   * 测试程序使用 unsigned int 保存结果，
   * 因此最多使用 32 个 bit 表示 32 个页面。
   */
  if(npages < 0 || npages > 32)
    return -1;

  for(int i = 0; i < npages; i++){
    uint64 va = start + (uint64)i * PGSIZE;

    /*
     * 查找该虚拟地址对应的页表项。
     * alloc=0，表示不存在时不创建页表。
     */
    pte_t *pte = walk(p->pagetable, va, 0);

    // 页面不存在或页表项无效，跳过。
    if(pte == 0 || ((*pte & PTE_V) == 0))
      continue;

    // 检查硬件设置的 Accessed 位。
    if(*pte & PTE_A){
      // 第 i 个页面被访问，设置结果中的第 i 位。
      mask |= (1U << i);

      // 清除 Accessed 位，供下一次 pgaccess 重新检测。
      *pte &= ~PTE_A;
    }
  }

  /*
   * 将内核中的 mask 复制到用户空间。
   */
  if(copyout(p->pagetable,
             user_mask,
             (char *)&mask,
             sizeof(mask)) < 0){
    return -1;
  }

  return 0;
}
#endif

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

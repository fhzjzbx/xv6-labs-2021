#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "fcntl.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

/*
 * 处理由 mmap 映射区域引起的用户态页面故障。
 *
 * p       ：当前进程
 * faultva ：发生页面故障的用户虚拟地址
 * scause  ：页面故障类型，13 表示读取故障，15 表示写入故障
 *
 * 成功建立映射返回 0，无法处理时返回 -1
 */
static int
mmap_page_fault(struct proc *p, uint64 faultva, uint64 scause)
{
  uint64 va, file_offset, offset_in_vma, remain;
  uint read_len;
  char * mem;
  int perm;
  struct vma *v = 0;
  pte_t *pte;
  int n, i;

  /*
   * 首先检查故障地址是否属于当前进程的某个有效 VMA
   * 使用 faultva - addr < length 的形式可以避免 addr + length 发生整数溢出
   */
  for(i = 0; i < NVMA; i++){
    if(p->vmas[i].used && faultva >= p->vmas[i].addr && faultva - p->vmas[i].addr < p->vmas[i].length){
      v = &p->vmas[i];
      break;
    }
  }

  /*
   * 如果故障地址不属于任何 mmap 区域
   * 说明这不是可以恢复的 mmap 页面故障
   */
  if(v == 0)
    return -1;

  /*
   * Load Page Fault 只能发生在具有读权限的 VMA 中
   */
  if(scause == 13 && (v->prot & PROT_READ) == 0)
    return -1;

  /*
   * Store/AMO Page Fault 只能发生在具有写权限的 VMA 中
   */
  if(scause == 15 && (v->prot & PROT_WRITE) == 0)
    return -1;

  /*
   * 页表映射必须以页面为单位
   * 因此将发生故障的地址向下取整到所在虚拟页的起始地址
   */
  va = PGROUNDDOWN(faultva);

  /*
   * 防止对已经存在的页面重复建立映射。
   */
  pte = walk(p->pagetable, va, 0);

  if(pte != 0 && (*pte & PTE_V))
    return -1;

  /*
   * 计算当前页面相对于 VMA 起始地址的偏移
   * 该值同时决定应该读取文件中的哪一部分
   */
  offset_in_vma = va - v->addr;
  file_offset = v->offset + offset_in_vma;

  /*
   * 为当前映射页分配一个新的物理页面。
   */
  mem = kalloc();
  if(mem == 0)
    return -1;
  
  /*
   * 先把整个页面清零
   * 如果文件在这一页中途结束，readi() 只会读取实际存在的文件内容
   * 其余部分应保持为零
   */
  memset(mem, 0, PGSIZE);

  /*
   * 最后一页可能只属于 VMA 的一部分
   * 因此最多读取 VMA 剩余的有效字节数
   */
  remain = v->length - offset_in_vma;

  if(remain > PGSIZE)
    read_len = PGSIZE;
  else
    read_len = (uint)remain;

  /*
   * readi() 要求调用者持有 inode 锁
   *
   * 第二个参数为 0，表示目标地址 mem 是内核地址
   * 而不是用户虚拟地址
   */
  ilock(v->file->ip);

  n = readi(v->file->ip, 0, (uint64)mem, (uint)file_offset, read_len);

  iunlock(v->file->ip);

  /*
   * 读取失败时释放刚刚分配的物理页面
   * readi() 在到达文件末尾时可能返回小于 read_len 的值
   * 这种情况是正常的，页面剩余部分已经保持为零
   */
  if(n < 0){
    kfree(mem);
    return -1;
  }

  /*
   * 根据 mmap() 的 prot 参数构造用户页表权限。
   */
  perm = PTE_U;

  if(v->prot & PROT_READ)
    perm |= PTE_R;

  /*
   * RISC-V 不允许只有 PTE_W 而没有 PTE_R 的叶子页表项，
   * 因此可写页面同时设置读权限。
   */
  if(v->prot & PROT_WRITE)
    perm |= PTE_R | PTE_W;

  /*
   * 将新物理页面映射到发生页面故障的用户虚拟页
   * mappages() 会自行加入 PTE_V，不需要手动设置
   */
  if(mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) != 0){
    kfree(mem);
    return -1;
  }

  return 0;
}

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  }else if(r_scause() == 13 || r_scause() == 15){
    /*
     * mmap 区域可能产生的用户态页面故障
     *
     * scause == 13：Load Page Fault
     * scause == 15：Store/AMO Page Fault
     */
    uint64 faultva = r_stval();
    int is_store = (r_scause() == 15);

    /*
     * mmapfault() 只有在故障地址属于合法 VMA
     * 且访问权限正确时才会建立页面映射
     *
     * 其他非法页面访问仍然终止当前进程
     */
    if(mmap_page_fault(p, faultva, is_store) < 0)
      p->killed = 1;
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}


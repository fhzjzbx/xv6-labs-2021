//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

/*
 * 将一个已经实际映射的 mmap 页面写回对应文件。
 *
 * 该函数主要用于 MAP_SHARED 映射。
 *
 * p  ：当前进程
 * v  ：页面所属的 VMA
 * va ：需要写回的页对齐用户虚拟地址
 *
 * 成功返回 0，写回失败返回 -1。
 */
static int
mmap_writeback_page(struct proc *p, struct vma *v, uint64 va){
  uint64 pa, offset_in_vma, file_offset, remain;
  uint done, n;
  int max;
  pte_t *pte;
  int r;

  /*
   * 查找当前用户虚拟页对应的页表项。
   *
   * mmap 采用懒加载，因此该 VMA 中的某些页面
   * 可能从未建立过实际映射。
   */
  pte = walk(p->pagetable, va, 0);

  if(pte == 0 || (*pte & PTE_V) == 0)
    return 0;

  /*
   * 得到该用户页面所对应的物理页面地址。
   *
   * xv6 在内核页表中对物理内存进行了直接映射，
   * 因此内核可以直接使用该物理地址访问页面内容。
   */
  pa = PTE2PA(*pte);

  /*
   * 计算当前页面在 VMA 中的偏移
   */
  offset_in_vma = va - v->addr;

  if(offset_in_vma >= v->length)
    return -1;

  /*
   * 根据 VMA 的文件偏移计算该页面对应的文件位置
   */
  file_offset = v->offset + offset_in_vma;

  /*
   * VMA 最后一页可能不足一个完整页面
   * 因此只写回真正属于映射区域的字节
   */
  remain = v->length - offset_in_vma;

  if(remain > PGSIZE)
    n = PGSIZE;
  else
    n = (uint)remain;

  /*
   * 参考 filewrite() 的实现
   * 把一次页面写回拆成若干较小的日志事务
   * 避免单个事务占用过多日志块
   */
  max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
  done = 0;

  while(done < n){
    uint n1 = n - done;

    if(n1 > (uint)max)
      n1 = max;

    begin_op();

    ilock(v->file->ip);

    /*
     * 这里的源地址是内核能够直接访问的物理内存
     * 因此 writei() 的 user_src 必须为 0
     */
    r = writei(v->file->ip, 0, pa + done, (uint)(file_offset + done), n1);

    iunlock(v->file->ip);

    end_op();

    if(r != (int)n1)
      return -1;

    done += r;
  }

  return 0;  
}

/*
 * 建立文件到用户虚拟地址空间的映射
 * 当前函数只创建 VMA 元数据，不分配物理页面，也不读取文件内容
 * 映射页面将在用户首次访问时，由页面故障处理程序按需建立
 */
uint64
sys_mmap(void)
{
  int length, prot, flags, offset, slot;
  uint64 requested_addr, map_length, map_top, map_addr;
  struct file *file;
  struct proc *p;
  struct vma *v;
  int i;

  /*
   * 读取 mmap(addr, length, prot, flags, fd, offset)的六个系统调用参数
   */
  argaddr(0, &requested_addr);
  argint(1, &length);
  argint(2, &prot);
  argint(3, &flags);

  if(argfd(4, 0, &file) < 0)
    return (uint64)-1;

  argint(5, &offset);

  /*
   * 假定 addr 和 offset 均为零
   */
  if(requested_addr != 0 || offset != 0)
    return (uint64)-1;

  /*
   * 映射长度必须为正数
   */
  if(length <= 0)
    return (uint64)-1;

  /*
   * 只支持 MAP_SHARED 和 MAP_PRIVATE
   */
  if(flags != MAP_SHARED && flags != MAP_PRIVATE)
    return (uint64)-1;

  /*
   * 只要求支持可读、可写或同时可读可写的映射
   */
  if((prot & ~(PROT_READ | PROT_WRITE)) != 0)
    return (uint64)-1;

  if((prot & (PROT_READ | PROT_WRITE)) == 0)
    return (uint64)-1;

  /*
   * 页面故障处理时需要从文件中读取数据
   * 因此被映射文件必须具有读取权限
   */
  if(file->readable == 0)
    return (uint64)-1;

  /*
   * MAP_SHARED 的可写映射最终需要把修改写回文件
   * 因此底层文件必须以可写方式打开
   *
   * MAP_PRIVATE 的修改不写回原文件，所以不要求文件本身具有写权限
   */
  if(flags == MAP_SHARED && (prot & PROT_WRITE) && file->writable == 0)
    return (uint64)-1;

  /*
   * 当前实验只处理 inode 文件映射
   * 不对管道或设备文件建立 mmap 映射
   */
  if(file->type != FD_INODE)
    return (uint64)-1;
  
  p = myproc();
  slot = -1;

  /*
   * 从进程的固定 VMA 数组中寻找空闲槽位
   */
  for(i = 0; i < NVMA; i++){
    if(p->vmas[i].used == 0){
      slot = i;
      break;
    }
  }

  if(slot < 0)
    return (uint64)-1;

  /*
   * 物理页面按整页建立映射，因此地址空间占用长度需要向上取整到页面边界
   */
  map_length = PGROUNDUP((uint64)length);

  if(map_length == 0)
    return (uint64)-1;

  /*
   * mmap 区域从 TRAPFRAME 下方开始向低地址增长
   * map_top 表示当前能够使用的最高边界
   */
  map_top = TRAPFRAME;

  for(i = 0; i < NVMA; i++){
    if(p->vmas[i].used && p->vmas[i].addr < map_top)
      map_top = p->vmas[i].addr;
  }

  /*
   * 检查减法是否会下溢
   */
  if(map_length > map_top)
    return (uint64)-1;

  map_addr = map_top - map_length;

  /*
   * mmap 区域不能向下覆盖进程已有的代码、数据、用户栈或由 sbrk() 扩展的堆区域
   */
  if(map_addr < PGROUNDUP(p->sz))
    return (uint64)-1;

  v = &p->vmas[slot];

  /*
   * 填写 VMA 元数据
   */
  v->used = 1;
  v->addr = map_addr;
  v->length = (uint64)length;
  v->prot = prot;
  v->flags = flags;
  v->offset = (uint64)offset;

  /*
   * VMA 必须独立持有文件引用
   * 即使用户随后关闭原文件描述符，映射也应继续有效
   */
  v->file = filedup(file);

  /*
   * 此处只返回预留的虚拟地址，不建立任何页表项
   */
  return map_addr;
}

/*
 * 解除用户虚拟地址空间中的文件映射
 */
uint64
sys_munmap(void)
{
  uint64 addr, len, end, vma_end, unmap_end, va;
  int length;
  pte_t *pte;
  struct proc *p;
  struct vma *v;
  int i;

  /*
   * 读取 munmap(addr, length) 的两个参数
   */
  argaddr(0, &addr);
  argint(1, &length);

  if(length <= 0)
    return -1;

  /*
   * munmap 的起始地址必须按页面对齐
   */
  if(addr % PGSIZE != 0)
    return -1;

  len = (uint64)length;

  /*
   * 防止 addr + len 发生无符号整数溢出
   */
  if(addr + len < addr)
    return -1;

  end = addr + len;
  p = myproc();
  v = 0;

  /*
   * 在当前进程的 VMA 表中寻找包含整个 munmap 请求范围的映射区域
   */
  for(i = 0; i < NVMA; i++){
    if(p->vmas[i].used == 0)
      continue;

    /*
     * addr 必须位于当前 VMA 内
     */
    if(addr < p->vmas[i].addr)
      continue;

    if(addr - p->vmas[i].addr >= p->vmas[i].length)
      continue;

    /*
     * munmap 的整个范围必须包含在同一个 VMA 中
     * 使用减法判断可以减少地址加法溢出的风险
     */
    if(len > p->vmas[i].length - (addr - p->vmas[i].addr))
      continue;

    v = &p->vmas[i];
    break;
  }

  /*
   * 没有找到对应 VMA，说明参数无效
   */
  if(v == 0)
    return -1;

  if(length <= 0)
    return -1;
  
  /*
   * munmap解除范围必须完全位于一个VMA内部
   * 防止用户请求解除超过映射区域的地址
   */
  if(addr + length > v->addr + v->length)
      return -1;

  vma_end = v->addr + v->length;

  /*
   * 如果解除的是中间区域，需要拆分VMA。
   */
  if(addr != v->addr && end != vma_end){

    int newslot = -1;

    for(i = 0; i < NVMA; i++){
        if(p->vmas[i].used == 0){
            newslot = i;
            break;
        }
    }

    if(newslot < 0)
        return -1;


    /*
     * 创建右侧新的VMA。
     */
    p->vmas[newslot] = *v;

    p->vmas[newslot].addr = end;

    p->vmas[newslot].offset += end - v->addr;

    p->vmas[newslot].length =
        vma_end - end;


    /*
     * 原VMA缩小为左侧部分。
     */
    v->length = addr - v->addr;


    /*
     * 如果左侧为空，删除原VMA。
     */
    if(v->length == 0){

        fileclose(v->file);

        memset(v,0,sizeof(*v));
    }


    /*
     * 解除页面已经完成，直接返回。
     */
    return 0;
  }

  /*
   * 实际页表操作必须以整页为单位。
   */
  unmap_end = PGROUNDUP(end);

  /*
   * 逐页处理需要解除的地址范围
   *
   * 不能一次性 uvmunmap 整个 VMA
   * 因为 lazy mmap 中可能存在从未装入的页面
   */
  for(va = addr; va < unmap_end; va += PGSIZE){
    pte = walk(p->pagetable, va, 0);

    /*
     * 页面从未发生过缺页加载时
     * 页表项可能不存在或无效
     *
     * 这种情况本身是合法的，只需跳过
     */
    if(pte == 0 || (*pte & PTE_V) == 0)
      continue;

    /*
     * MAP_SHARED 且具有写权限的页面
     * 在解除映射前将当前内存内容写回文件
     *
     * 本阶段暂时不检查 PTE_D
     * 因此已经实际加载的共享可写页面统一写回
     */
    if(v->flags == MAP_SHARED && (v->prot & PROT_WRITE)){
      if(mmap_writeback_page(p, v, va) < 0)
        return -1;
    }

    /*
     * 写回完成后解除这一页映射
     *
     * npages == 1： 每次只处理一个页面
     * do_free == 1： 同时释放该页对应的物理内存
     */
    uvmunmap(p->pagetable, va, 1, 1);
  }

  /*
   * 情况一：整个 VMA 被解除
   */
  if(addr == v->addr && end == vma_end){
    /*
     * mmap() 时通过 filedup() 增加了文件引用
     * 整个 VMA 消失后必须归还这一引用
     */
    fileclose(v->file);

    /*
     * 清空槽位，使其以后能够重新用于新的 mmap
     */
    memset(v, 0, sizeof(*v));

    return 0;
  }
  
  /*
   * 情况二：从 VMA 开头解除
   */
  if(addr == v->addr){

    /*
    * 实际解除的大小按页计算。
    */
    uint64 removed = PGROUNDUP(end) - v->addr;


    /*
    * 如果整个 VMA 都被解除，
    * 则释放文件引用并清空 VMA。
    */
    if(removed >= v->length){

      fileclose(v->file);

      memset(v,
            0,
            sizeof(*v));

      return 0;
    }


    /*
    * VMA 起始地址向后移动。
    */
    v->addr += removed;


    /*
    * 文件偏移同步增加。
    *
    * 因为新的虚拟地址对应文件中的后续位置。
    */
    v->offset += removed;


    /*
    * 剩余映射长度减少。
    */
    v->length -= removed;


    return 0;
  }

  // 正常情况下不会执行到这里
  return -1;
}

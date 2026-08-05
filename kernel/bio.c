// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

/*
 * 使用质数个哈希桶，以降低块号发生哈希冲突的概率
 */
#define NBUCKET 13

extern uint ticks;
extern struct spinlock tickslock;

struct bucket{
  struct spinlock lock;
  /*
   * 当前桶的双向循环链表哨兵节点
   */
  struct buf head;
};


struct {
  struct spinlock evict_lock;
  struct buf buf[NBUF];
  /*
   * 哈希桶数组
   */
  struct bucket bucket[NBUCKET];
} bcache;

/*
 * 根据设备号和磁盘块号计算哈希桶
 */
static int
bhash(uint dev, uint blockno)
{
  return (blockno + dev) % NBUCKET;
}

/*
 * 将缓冲区从当前双向链表中删除
 */
static void
bucket_remove(struct buf *b)
{
  b->prev->next = b->next;
  b->next->prev = b->prev;
  b->prev = 0;
  b->next = 0;
}

/*
 * 将缓冲区插入指定哈希桶的链表头部
 */
static void
bucket_insert(int index, struct buf *b)
{
  struct buf *head = &bcache.bucket[index].head;

  b->next = head->next;
  b->prev = head;
  head->next->prev = b;
  head->next = b;
}

void
binit(void)
{
  struct buf *b;
  int i;
  int index;

  /*
   * 初始化缓存回收锁。
   */
  initlock(&bcache.evict_lock, "bcache.evict");

  /*
   * 初始化每个哈希桶的锁和哨兵节点。
   */
  for(i = 0; i < NBUCKET; i++){
    initlock(&bcache.bucket[i].lock, "bcache.bucket");
    // Create linked list of buffers
    bcache.bucket[i].head.prev = &bcache.bucket[i].head;
    bcache.bucket[i].head.next = &bcache.bucket[i].head;
  }
  
  for(b = bcache.buf, i = 0; b < bcache.buf + NBUF; b++, i++){
    initsleeplock(&b->lock, "buffer");
    
    b->valid = 0;
    b->disk = 0;
    b->dev = (uint)-1;
    b->blockno = (uint)-1;
    b->refcnt = 0;
    b->timestamp = 0;

    index = i % NBUCKET;
    bucket_insert(index, b);
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  struct buf *victim;
  uint oldest;
  int target;
  int victim_bucket;
  int i;
  int found_victim;

  target = bhash(dev, blockno);

  /*
   * 快速路径：只锁目标哈希桶
   */
  acquire(&bcache.bucket[target].lock);

  // Is the block already cached?
  for(b = bcache.bucket[target].head.next; b != &bcache.bucket[target].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bucket[target].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bucket[target].lock);

  acquire(&bcache.evict_lock);

  /*
   * 为了简化回收和跨桶移动过程，缓存未命中时按照固定顺序获得全部桶锁
   */
  acquire(&bcache.bucket[target].lock);

   for(b = bcache.bucket[target].head.next; b != &bcache.bucket[target].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;

      release(&bcache.bucket[target].lock);
      release(&bcache.evict_lock);

      acquiresleep(&b->lock);
      return b;
    }
  }

  release(&bcache.bucket[target].lock);


  /*
   * 如果选中的 victim 在重新加锁前被其他 CPU 使用
   * 就重新扫描一次所有哈希桶
   */
  found_victim = 0;

  while(!found_victim){
    victim = 0;
    victim_bucket = -1;
    oldest = (uint)-1;

    /*
     * 逐桶寻找 refcnt 为零且 timestamp 最小的缓冲区
     * 每次只持有一把桶锁，避免一次缓存未命中长时间阻塞所有哈希桶
     */
    for(i = 0; i < NBUCKET; i++){
      acquire(&bcache.bucket[i].lock);

      for(b = bcache.bucket[i].head.next;
          b != &bcache.bucket[i].head;
          b = b->next){

        if(b->refcnt == 0 && (victim == 0 || b->timestamp < oldest)){
          victim = b;
          victim_bucket = i;
          oldest = b->timestamp;
        }
      }

      release(&bcache.bucket[i].lock);
    }

  
    if(victim == 0){
      release(&bcache.evict_lock);
      panic("bget: no buffers");
    }

    acquire(&bcache.bucket[victim_bucket].lock);

    if(victim->refcnt == 0)
      found_victim = 1;
    else 
      release(&bcache.bucket[victim_bucket].lock);
  }
  /*
   * 将 victim 从原桶中删除
   */
  bucket_remove(victim);

  /*
   * 为 victim 设置新的缓存身份
   */
  victim->dev = dev;
  victim->blockno = blockno;
  victim->valid = 0;
  victim->disk = 0;
  victim->refcnt = 1;
  victim->timestamp = 0;

  /*
   * 将 victim 插入目标哈希桶
   */
  if(victim_bucket == target){
    /*
     * victim 原来就在目标桶中。
     * 此时已经持有目标桶锁，直接重新插入即可。
     */
    bucket_insert(target, victim);

    release(&bcache.bucket[victim_bucket].lock);
  } else {
    /*
     * victim 需要从旧桶移动到目标桶。
     *
     * 先释放旧桶锁，再获得目标桶锁；
     * evict_lock 保证没有其他回收线程同时移动缓冲区。
     */
    release(&bcache.bucket[victim_bucket].lock);

    acquire(&bcache.bucket[target].lock);
    bucket_insert(target, victim);
    release(&bcache.bucket[target].lock);
  }

  release(&bcache.evict_lock);

  /*
   * 所有自旋锁释放后，再获得缓冲区睡眠锁
   */
  acquiresleep(&victim->lock);
  return victim;  
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  uint now;
  int index;

  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  /*
   * 读取系统时钟，记录最后使用时间。
   */
  acquire(&tickslock);
  now = ticks;
  release(&tickslock);

  index = bhash(b->dev, b->blockno);

  acquire(&bcache.bucket[index].lock);

  if(b->refcnt < 1)
    panic("brelse refcnt");

  b->refcnt--;
  if (b->refcnt == 0) 
    b->timestamp = now;
  
  release(&bcache.bucket[index].lock);
}

void
bpin(struct buf *b) {
  int index;
  index = bhash(b->dev, b->blockno);

  acquire(&bcache.bucket[index].lock);
  b->refcnt++;
  release(&bcache.bucket[index].lock);
}

void
bunpin(struct buf *b) {
  uint now;
  int index;

  acquire(&tickslock);
  now = ticks;
  release(&tickslock);

  index = bhash(b->dev, b->blockno);
  
  acquire(&bcache.bucket[index].lock);

  if(b->refcnt < 1)
    panic("bunpin refcnt");

  b->refcnt--;

  if(b->refcnt == 0)
    b->timestamp = now;

  release(&bcache.bucket[index].lock);
}



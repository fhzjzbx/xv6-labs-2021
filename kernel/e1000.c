#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //
  uint32 index;
  struct  tx_desc *desc;

  /*
   * 发送环和发送缓冲区数组可能被多个进程同时访问
   * 因此整个描述符提交过程需要互斥保护
   */
  acquire(&e1000_lock);
  
  /*
   * TDT 指向驱动下一次应填写的发送描述符。
   */
  index = regs[E1000_TDT];
  desc = &tx_ring[index];

  /*
   * DD 未设置表示网卡尚未完成该描述符之前的发送任务。
   * 当前不能覆盖该描述符，因此返回失败。
   */
  if((desc->status & E1000_TXD_STAT_DD) == 0){
    release(&e1000_lock);
    return -1;
  }

  /*
   * 描述符已经由硬件处理完成。
   * 此时可以安全释放该位置上一次保存的 mbuf。
   */
  if(tx_mbufs[index] != 0){
    mbuffree(tx_mbufs[index]);
    tx_mbufs[index] = 0;
  }

  /*
   * 将当前网络包的数据地址和长度写入发送描述符
   */
  desc->addr = (uint64)m->head;
  desc->length = m->len;

  /*
   * EOP 表示当前描述符结束一个完整网络包
   * RS 要求硬件在完成发送后更新描述符状态
   */
  desc->cmd = E1000_TXD_CMD_EOP |
              E1000_TXD_CMD_RS;

  /*
   * 保存 mbuf 指针。
   * 现在不能立即释放，必须等网卡完成 DMA
   */
  tx_mbufs[index] = m;

  /*
   * 推进发送环尾指针。
   * 写入 TDT 后，E1000 才会看到新的发送任务。
   */
  regs[E1000_TDT] = (index + 1) % TX_RING_SIZE;

  release(&e1000_lock);
  
  return 0;
}

static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //

  for(;;){
    uint32 index;
    struct rx_desc *desc;
    struct mbuf *received_mbuf;
    struct mbuf *new_mbuf;

    /*
     * 接收描述符环、rx_mbufs 数组和 RDT 寄存器都是共享状态
     * 因此在检查和更新它们时需要获得网卡锁
     */
    acquire(&e1000_lock);

    /*
     * RDT 指向软件最后处理并归还给硬件的描述符
     * 所以下一个可能包含新数据包的位置是 RDT 加一
     */
    index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
    desc = &rx_ring[index];

    /*
     * DD 未设置表示网卡尚未在该描述符中完成数据包接收
     * 由于接收环按顺序处理，此时可以停止本轮扫描
     */
    if((desc->status & E1000_RXD_STAT_DD) == 0){
      release(&e1000_lock);
      return;
    }

    /*
     * 保存已经接收到数据包的旧 mbuf。
     * 描述符中的 length 是网卡实际写入的数据长度。
     */
    received_mbuf = rx_mbufs[index];
    received_mbuf->len = desc->length;

    /*
     * 为当前描述符分配新的空缓冲区。
     * 旧缓冲区即将交给上层协议栈，不能继续交给网卡使用。
     */
    new_mbuf = mbufalloc(0);
    if(new_mbuf == 0){
      release(&e1000_lock);
      panic("e1000_recv: mbufalloc");
    }

    /*
     * 将新的缓冲区安装到接收环中。
     */
    rx_mbufs[index] = new_mbuf;
    desc->addr = (uint64)new_mbuf->head;

    /*
     * 清除完成状态，将描述符重新交还给硬件。
     */
    desc->status = 0;

    /*
     * 告诉 E1000：该描述符已经处理完毕，可以再次用于接收。
     */
    regs[E1000_RDT] = index;

    /*
     * 在调用 net_rx() 前释放网卡锁。
     *
     * net_rx() 处理 ARP 请求时可能调用 e1000_transmit()
     * 发送 ARP Reply。如果收发共用同一把锁，而这里不先释放，
     * 将产生重复获取同一自旋锁的风险。
     */
    release(&e1000_lock);

    /*
     * 把收到的数据包交给 Ethernet、ARP、IP 和 UDP 协议处理层。
     * 从此处开始，旧 mbuf 的所有权转移给网络协议栈。
     */
    net_rx(received_mbuf);
  }
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}

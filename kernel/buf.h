struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;

  /*
   * 缓冲区最后一次变为空闲状态时的时间
   * 缓存回收时优先选择 timestamp 最小的缓冲区
   */
  uint timestamp;

  struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];
};


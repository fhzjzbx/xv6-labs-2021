#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

static int nthread = 1;
static int round = 0;

struct barrier {
  pthread_mutex_t barrier_mutex;
  pthread_cond_t barrier_cond;
  int nthread;      // Number of threads that have reached this round of the barrier
  int round;     // Barrier round
} bstate;

static void
barrier_init(void)
{
  assert(pthread_mutex_init(&bstate.barrier_mutex, NULL) == 0);
  assert(pthread_cond_init(&bstate.barrier_cond, NULL) == 0);
  bstate.nthread = 0;
}

static void 
barrier()
{
  // YOUR CODE HERE
  //
  // Block until all threads have called barrier() and
  // then increment bstate.round.
  //

  int current_round;
  /*
   * bstate.nthread 和 bstate.round 是多个线程共享的数据
   * 因此访问和修改它们之前必须获得互斥锁
   */
  pthread_mutex_lock(&bstate.barrier_mutex);

  /*
   * 保存当前线程进入屏障时所处的轮次
   * 等待线程将通过该值判断本轮是否已经结束
   */
  current_round = bstate.round;

  /*
   * 当前线程已经到达本轮屏障。
   */
  bstate.nthread++;

  /*
   * 如果当前线程是本轮最后一个到达屏障的线程，
   * 则由它负责结束本轮，推进到下一轮
   */
  if(bstate.nthread == nthread){
    /*
     * 为下一轮重新统计到达线程数，同时推进barrier
     */
    bstate.nthread = 0;
    bstate.round++;
    /*
     * 唤醒所有正在等待本轮完成的线程。
     */
    pthread_cond_broadcast(&bstate.barrier_cond);
  }else{
    /*
     * 当前线程不是最后到达的，需要等待
     * 使用 while 可以处理虚假唤醒，并在醒来后重新检查条件
     */
    while(current_round == bstate.round){
      pthread_cond_wait(&bstate.barrier_cond,
                        &bstate.barrier_mutex);
    }
  }

  /*
   * 当前线程已经可以通过屏障，释放互斥锁。
   */
  pthread_mutex_unlock(&bstate.barrier_mutex);
}

static void *
thread(void *xa)
{
  long n = (long) xa;
  long delay;
  int i;

  for (i = 0; i < 20000; i++) {
    int t = bstate.round;
    assert (i == t);
    barrier();
    usleep(random() % 100);
  }

  return 0;
}

int
main(int argc, char *argv[])
{
  pthread_t *tha;
  void *value;
  long i;
  double t1, t0;

  if (argc < 2) {
    fprintf(stderr, "%s: %s nthread\n", argv[0], argv[0]);
    exit(-1);
  }
  nthread = atoi(argv[1]);
  tha = malloc(sizeof(pthread_t) * nthread);
  srandom(0);

  barrier_init();

  for(i = 0; i < nthread; i++) {
    assert(pthread_create(&tha[i], NULL, thread, (void *) i) == 0);
  }
  for(i = 0; i < nthread; i++) {
    assert(pthread_join(tha[i], &value) == 0);
  }
  printf("OK; passed\n");
}

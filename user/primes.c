#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/*
    质数筛选函数
    参数：left_fd: 进程左侧管道的读段
*/
void sieve(int left_fd){
    int prime;
    int number;
    int right_pipe[2];
    int pid;

    // 从左侧管道读取的一个参数
    // 这个参数就是当前筛选器对应的质数
    if(read(left_fd, &prime, sizeof(prime)) != sizeof(prime)){
        close(left_fd);
        return;
    }

    printf("prime %d\n", prime);

    // 创建通向下一级筛选器的管道
    if(pipe(right_pipe) < 0){
        fprintf(2, "primes: fork failed\n");
        close(left_fd);
        exit(1);
    }

    pid = fork();

    if(pid < 0){
        fprintf(2, "primes: fork failed\n");
        close(left_fd);
        close(right_pipe[0]);
        close(right_pipe[1]);
        exit(1);
    }

    if(pid == 0){
        // 关闭右侧管道的写端和左侧管道，递归处理下一层
        close(right_pipe[1]);
        close(left_fd);
        sieve(right_pipe[0]);

        exit(0);
    }

    // 父进程只负责过滤当前质数的倍数，只写入，因此关闭读端
    close(right_pipe[0]);

    while(read(left_fd, &number, sizeof(number)) == sizeof(number)){
        // 将不是当前质数的倍数的数传给下一级
        if(number % prime != 0){
            if(write(right_pipe[1], &number, sizeof(number)) != sizeof(number)){
                fprintf(2, "prime: write failed\n");
                close(left_fd);
                close(right_pipe[1]);
                exit(1);
            }
        }
    }

    // 已完成所有筛选环节
    close(left_fd);
    close(right_pipe[1]);
    wait(0);
}

int
main(int argc, char *argv[])
{
    int first_pipe[2];
    int pid;
    int number;

    if(pipe(first_pipe) < 0){
        fprintf(2, "primes: pipe failed\n");
        exit(1);
    }

    pid = fork();

    if(pid < 0){
        fprintf(2, "primes: fork failed\n");
        close(first_pipe[0]);
        close(first_pipe[1]);
        exit(1);        
    }

    // 主进程读取第一条管道
    if(pid == 0){
        close(first_pipe[1]);
        sieve(first_pipe[0]);
        exit(0);
    }

    // 主进程向第一条管道写入数字
    close(first_pipe[0]);

    for(number = 2; number <= 35; number++){
        if(write(first_pipe[1], &number, sizeof(number)) != sizeof(number)){
            fprintf(2, "primes: write failed\n");
            close(first_pipe[1]);
            exit(1);
        }
    }

    // 写完后关闭写端
    close(first_pipe[1]);

    // 等待筛选完成
    wait(0);
    exit(0);
}
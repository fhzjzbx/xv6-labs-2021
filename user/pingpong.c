#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    int parent_to_child[2];
    int child_to_parent[2];
    int pid;
    char buffer;

    // 创建管道
    if(pipe(parent_to_child) < 0){
        fprintf(2, "pingpong: pipe failed\n");
        exit(1);
    }

    if(pipe(child_to_parent) < 0){
        fprintf(2, "pingpong: pipe failed\n");
        exit(1);
    }
    
    pid = fork();

    if(pid < 0){
        fprintf(2, "pingpong: fork failed\n");
        exit(1);
    }

    // 子进程
    if(pid == 0){
        // 只读取parent_to_child，写入child_to parent
        close(parent_to_child[1]);
        close(child_to_parent[0]);

        // 读取父进程的一个字节
        if(read(parent_to_child[0], &buffer, 1) != 1){
            fprintf(2, "pingpong: child read failed\n");
            exit(1);            
        }

        printf("%d: received ping\n", getpid());

        // 向父进程发送一个字节
        if(write(child_to_parent[1], &buffer, 1) != 1){
            fprintf(2, "pingpong: child write failed\n");
            exit(1);            
        }

        close(parent_to_child[0]);
        close(child_to_parent[1]);

        exit(0);
    }
    
    // 父进程
    else{
        // 只写入parent_to_child，读取child_to parent
        close(parent_to_child[0]);
        close(child_to_parent[1]);

        // 给子进程发送一个字节
        buffer = 'x';
        if(write(parent_to_child[1], &buffer, 1) != 1){
            fprintf(2, "pingpong: parent write failed\n");
            exit(1);  
        }

        // 读取子进程的一个字节
        if(read(child_to_parent[0], &buffer, 1) != 1){
            fprintf(2, "pingpong: parent read failed\n");
            exit(1);            
        }
        
        printf("%d: received pong\n", getpid());

        close(parent_to_child[1]);
        close(child_to_parent[0]);

        // 回收子进程
        wait(0);

        exit(0);
    }
}
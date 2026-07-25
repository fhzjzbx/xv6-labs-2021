#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    if(argc != 2){
        fprintf(2, "error: argument must be ticks\n");
        exit(1);
    }

    // 将参数字符串转化为整数
    int ticks = atoi(argv[1]);

    // 参数应该非负
    if(ticks < 0){
        fprintf(2, "error: ticks must be non-negative\n");
        exit(1);
    }

    // 没有问题就调用sleep并正常退出
    sleep(ticks);
    exit(0);
}
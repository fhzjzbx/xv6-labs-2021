#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"

#define LINE_BUFFER_SIZE 512

/*
 * 使用准备好的参数数组执行命令。
 * 参数组为args
 */
static void
run_command(char *args[]){
    int pid;

    pid = fork();

    if(pid < 0){
        fprintf(2, "xrags: fork failed\n");
        exit(1);
    }

    if(pid == 0){
        exec(args[0], args);

        // exec执行后不返回，执行失败才会执行下面的内容
        fprintf(2, "xargs: exec %s failed\n", args[0]);
        exit(1);
    }

    if(wait(0) < 0){
        fprintf(2, "xargs: wait failed\n");
        exit(1);
    }
}

/*
 * 处理已经读取完成的一行。
 *
 * buf: 保存这一行的字符，不包括换行符。
 * 
 * length: 当前行的字符数量。
 *
 * args: 即将传给 exec 的参数数组。
 *
 * fixed_argc: xargs 命令本身提供的固定参数数量。
 */
static void
process_line(char *buffer, int length, char *args[], int fixed_argc){
    int arg_count;
    int in_argument;
    int i;

    // 将当前行变成合法的C字符串
    buffer[length] = '\0';

    // 新参数从固定参数后面开始添加
    arg_count = fixed_argc;
    in_argument = 0;

    // 把行内的空格和制表符全部换成'\0'
    for(i = 0; i < length; i++){
        if(buffer[i] == ' ' || buffer[i] == '\t' || buffer[i] == '\r'){
            buffer[i] = '\0';
            in_argument = 0;
        }
        else if(in_argument == 0){
            // 当前不是空白字符，且之前不在参数内部
            // 说明是新参数的起始位置

            // args数组必须留一个位置保存空指针0
            if(arg_count >= MAXARG - 1){
                fprintf(2, "xargs: too many arguments\n");
                exit(1);
            }

            args[arg_count] = &buffer[i];
            arg_count++;
            in_argument = 1;
        }
    }

    args[arg_count] = 0;
    // 如果当前行只有空格、制表符或者全空，那么不执行命令
    if(arg_count == fixed_argc){
        return;
    }

    run_command(args);
}


int
main(int argc, char *argv[])
{
    // 用以保存最后传给exec的参数
    char *args[MAXARG];

    // 保存从标准输入读取的一行
    char buffer[LINE_BUFFER_SIZE];

    char c;
    int length;
    int fixed_args;
    int read_result;
    int i;

    // xargs后面必须有命令内容
    if(argc < 2){
        fprintf(2, "usage: xargs command [arguments...]\n");
        exit(1);
    }

    // 去掉argv[0]中的xargs
    fixed_args = argc - 1;

    // 为标准输入中的新增参数和最后的0保留数组空间
    if(fixed_args >= MAXARG - 1){
        fprintf(2, "xargs: too many fixed arguments\n");
        exit(1);
    }

    // 复制xargs后面的固定参数和命令
    for(i = 0; i < fixed_args; i++){
        args[i] = argv[i + 1];
    }

    // 先放置参数数组结束标志，之后可以通过process_line追加新参数
    args[fixed_args] = 0;
    length = 0;

    // 每次读取一个字符
    while((read_result = read(0, &c, 1)) > 0){
        if(c == '\n'){
            // 遇到换行符时说明完整的一行已经读取完毕
            process_line(buffer, length, args, fixed_args);

            // 清空长度，读取下一行
            length = 0;
        }
        else{
            // 保留一个字节用于最后的'\0'
            if(length >= LINE_BUFFER_SIZE - 1){
                fprintf(2, "xargs: input line too long\n");
                exit(1);
            }

            buffer[length] = c;
            length++;
        }
    }

    // 读取失败报错
    if(read_result < 0){
        fprintf(2, "xargs: read failed\n");
        exit(1);
    }

    // 到达EOF后，缓冲区仍有内容，需要继续读取
    if(length > 0){
        process_line(buffer, length, args, fixed_args);
    }

    exit(0);
}
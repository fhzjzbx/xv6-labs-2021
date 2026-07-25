#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

// 返回路径中的最后一部分
char *
base_name(char *path){
    char *p;

    // 先指向字符末尾的'\0'
    p = path + strlen(path);

    // 从后往前寻找第一个'/'
    while(p > path && *(p - 1) != '/'){
        p--;
    }

    return p;
}

void
find(char *path, char *target){
    char buffer[512];
    char *p;
    int fd;
    struct stat st;
    struct dirent de;

    // 打开当前路径
    fd = open(path, O_RDONLY);

    if(fd < 0){
        fprintf(2, "find cannot open %s\n", path);
        return;
    }

    // 获取当前文件类型
    if(fstat(fd, &st) < 0){
        fprintf(2, "find: cannnot stat %s\n", path);
        close(fd);
        return;
    }

    switch(st.type){

        // 普通文件时，比较最后一段名称与目标名称是否相同
        case T_FILE:
            if(strcmp(base_name(path), target) == 0){
                printf("%s\n", path);
            }
            break;

        // 目录时，需要在buffer中构造
        case T_DIR:
            if(strlen(path) + 1 + DIRSIZ + 1 > sizeof(buffer)){
                fprintf(2, "find: path too long\n");
                break;
            }


            strcpy(buffer, path);

            // p指向路径末尾，并添加'/'
            p = buffer + strlen(buffer);
            *p++ = '/';

            // 每次从目录中读取一个目录项
            while(read(fd, &de, sizeof(de)) == sizeof(de)){

                // 目录项未使用
                if(de.inum == 0){
                    continue;
                }

                // 将目录项名字复制到'/'后面
                memmove(p, de.name, DIRSIZ);

                // 加上'\0'保证读取
                p[DIRSIZ] = '\0';

                // 不允许进入"."和".."
                if(strcmp(p, ".") == 0 ||
                   strcmp(p, "..") == 0){
                    continue;
                }

                find(buffer, target);
            }
            break;
    }

    close(fd);
}

int
main(int argc, char *argv[])
{
    if(argc != 3){
        fprintf(2,"usage: find path name\n");
        exit(1);
    }

    find(argv[1], argv[2]);

    exit(0);
}
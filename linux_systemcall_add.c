리눅스커널새시스템콜함수추가가가능합니다.
과정1. Fs/read_write.c나 kernel/sys.c에 아래와 같이 SYSCALL_DEFINE*[인자수]로 선언한뒤 
과정2. arch/x86/entry/syscalls/syscall_64.tbl 파일을 열어서 
450     common  hicall                  sys_hicall
이처럼 유니키한 시스템콜 번호와 common[32/64비트 모두 지원] 그리고 시스템콜 함수명을 쓰고 시스템콜이름을 마지막으로 써서 선언합니다.
그 다음 커널을 빌드하고 부티한 다음 시스템콜을 사용할 수 있게 됩니다.

/**
* hicall - Greet the user
*/
SYSCALL_DEFINE1(hicall, char*, data) {
char name[256];
long err = strncpy_from_user(name, data, sizeof(name));

if(err < 0) {
        printk(KERN_WARNING "hicall: cannot read data from userspace\n");
        return -EFAULT;
}

printk(KERN_INFO "hicall: Hi %s, nice to meet you.\n", name);
return 0;
}


test_hicall.c

#include <unistd.h>
#include <stdio.h>
#include <sys/syscall.h>

#define SYS_hicall 450

int main(void) {
    char name[256];
    printf("Insert your name: ");
    scanf("%s", name);

    syscall(SYS_hicall, name);

    puts("Done! Please check dmesg");

    return 0;
}

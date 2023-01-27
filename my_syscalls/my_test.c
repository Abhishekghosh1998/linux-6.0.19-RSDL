#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/fs.h>
#include <../fs/internal.h>

/*
*   The function my_mount uses char pointers from the kernel space 
*   Allocates memory in the kernel space, to get struct filename
*   and ultimately calls path_mount
*/
int my_mount(char* source, char * dest, char* filesystem, unsigned long flags, void* data)
{
    struct path path;
    int ret;

    struct filename* filename = getname_kernel(dest);
    ret = filename_lookup(AT_FDCWD, filename, flags, &path, NULL);

    putname(filename);

    if(ret)
        return ret;

    ret = path_mount(source, &path, filesystem, flags, data);
    path_put(&path);
    return ret;
}

/*
*   Mimics the transfer of jobs as per the script :
*   while read P; do echo $P > /cpusets/housekeeping/tasks; done < /cpusets/tasks
*/

int transfer_tasks(char* dest_filename, char* source_filename){
    struct file *src  = filp_open(source_filename, O_RDWR, 0);
    struct file *dest;
    char buf[100];
    long long dest_offset = 0;
    long long src_offset = 0;
    if(IS_ERR(src)) printk("src file struct error\n");
    
    while (1){
        int count = 0;
        char ch; 
        int flag = 1;
        while(1){
            flag = kernel_read(src, &ch, 1, &src_offset);
            if (!flag)
                break;
            buf[count++]=ch;
            if (ch == '\n')
                break;
        }
        buf[count]=0;
        if (!flag)
            break;
        dest_offset = 0;
        dest = filp_open(dest_filename, O_WRONLY, 0);
        if(IS_ERR(dest)) printk("dest file struct error\n");
        kernel_write(dest, buf, count-1, &dest_offset);
        filp_close(dest, NULL);
    }
    filp_close(src, NULL);
    return 0;
}

/*  
*   The hack for isolating the cpu makes use of cpusets.
*   The functionality mimiced is that done by the following script 
*   mkdir /cpusets
*   mount -t cpuset none /cpusets
*   mkdir /cpusets/housekeeping
*   mkdir /cpusets/isolated
*   echo 1-3 > /cpusets/housekeeping/cpus
*   echo 0 > /cpusets/isolated/cpus
*   echo 0 > /cpusets/housekeeping/mems
*   echo 0 > /cpusets/isolated/mems
*   echo 0 > /cpusets/isolated/sched_load_balance
*   echo 0 > /cpusets/sched_load_balance
*   while read P ; do echo $P > /cpusets/housekeeping/tasks ; done < /cpusets/tasks
 */

SYSCALL_DEFINE0(isolate_cpu){

    struct file *f ;
    long long offset = 0;
    printk("Trying to make /cpusets directory ....\n");
    do_mkdirat(AT_FDCWD, getname_kernel("/cpusets"),0); 
    //no need to free the struct filename, do_mkdirat 
    //internally does that
    
    printk("Trying to mount the cpuset filesystem at /cpusets");
    my_mount(0, "/cpusets", "cpuset", 0, 0);

    printk("Trying to make the directory /cpusets/housekeeping\n");
    do_mkdirat(AT_FDCWD, getname_kernel("/cpusets/housekeeping"),0);
    
    printk("Trying to make the directory /cpusets/isolated\n");
    do_mkdirat(AT_FDCWD, getname_kernel("/cpusets/isolated"),0);

    printk("Writing 1-3 to /cpusets/housekeeping/cpus\n");
    f = filp_open("/cpusets/housekeeping/cpus", O_RDWR, 0);
    offset = 0;
    kernel_write(f, "1-3", 3, &offset);
    filp_close(f, NULL);

    printk("Writing 0 to /cpusets/isolated/cpus\n");
    f = filp_open("/cpusets/isolated/cpus", O_RDWR, 0);
    offset = 0;
    kernel_write(f, "0", 1, &offset);
    filp_close(f, NULL);

    printk("Writing 0 to /cpusets/housekeeping/mems\n");
    f = filp_open("/cpusets/housekeeping/mems", O_RDWR, 0);
    offset = 0;
    kernel_write(f, "0", 1, &offset);
    filp_close(f, NULL);

    printk("Writing 0 to /cpusets/isolated/mems\n");
    f = filp_open("/cpusets/isolated/mems", O_RDWR, 0);
    offset = 0;
    kernel_write(f, "0", 1, &offset);
    filp_close(f, NULL);

    printk("Writing 0 to /cpusets/isolated/sched_load_balance\n");
    f = filp_open("/cpusets/isolated/sched_load_balance", O_RDWR, 0);
    offset = 0;
    kernel_write(f, "0", 1, &offset);
    filp_close(f, NULL);

    printk("Writing 0 to /cpusets/sched_load_balance\n");
    f = filp_open("/cpusets/sched_load_balance", O_RDWR, 0);
    offset = 0;
    kernel_write(f, "0", 1, &offset);
    filp_close(f, NULL);

    printk("Trying to transfer tasks to housingkeeping...\n");
    transfer_tasks("/cpusets/housekeeping/tasks", "/cpusets/tasks");
    return 0;
}

/*
* The following system call, is used of assigning a process to the 
* isolated cpu 0. In short it does the following
* PID=123 #if 123 is the pid of the proccess to be assigned to cpu0
* echo $PID > /cpusets/isolated/tasks 
*/

SYSCALL_DEFINE1(my_sched_setaffinity, int , pid){

    struct file *f = filp_open("/cpusets/isolated/tasks", O_RDWR, 0);
    long long offset=0;
    char buf [10];
    int copy=pid;
    int digit_count=0;
    int count=0;

    if(pid == 0){
        buf[0]='0';
        buf[1]='\n';
        kernel_write(f, buf, 2, &offset);
        filp_close(f, NULL);
        return 0;
    }
    
    while(copy){
        copy/=10;
        digit_count++;
    }
    count = digit_count+1;
    buf[digit_count--]='\n';
    while (pid){
        buf[digit_count--]= pid%10+'0';
        pid/=10;
    }
    kernel_write(f, buf, count, &offset);
    filp_close(f, NULL);
    return 0;
}
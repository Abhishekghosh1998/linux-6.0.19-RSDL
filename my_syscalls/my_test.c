#include <linux/kernel.h>
#include <linux/syscalls.h>
//#include <linux/cpu.h>
//#include <linux/sched/isolation.h>
#include <linux/fs.h>
//#include <linux/fsnotify.h>
#include <../fs/internal.h>



struct filename* my_getname(const char* filename)
{
    struct filename *result;
    char *kname;

    result = __getname();

    kname = (char *)result -> iname;
    result -> name = kname;

    strcpy(kname, filename);

    result -> refcnt = 1;
    result -> uptr = NULL;
    result -> aname = NULL;

    return result;
}

int my_mount(char* source, char * dest, char* filesystem, unsigned long flags, void* data)
{
    struct path path;
    int ret;

    struct filename* filename = my_getname(dest);
    ret = filename_lookup(AT_FDCWD, filename, flags, &path, NULL);

    putname(filename);

    if(ret)
        return ret;

    ret = path_mount(source, &path, filesystem, flags, data);
    path_put(&path);
    return ret;
}
/*int my_open(char* filename, int flags){

    if (force_o_largefile())
		flags |= O_LARGEFILE;

    umode_t mode = 0;

    int dfd = AT_FDCWD;

    struct open_how how_struct = build_open_how(flags, mode);
	struct open_how * how = &how_struct;

    struct open_flags op;
	int fd = build_open_flags(how, &op);
	struct filename *tmp;

	if (fd)
		return fd;

    fd = get_unused_fd_flags(how->flags);
	if (fd >= 0) {
		struct file *f = filp_open(filename, flags, mode);//do_filp_open(dfd, tmp, &op);
		if (IS_ERR(f)) {
			put_unused_fd(fd);
			fd = PTR_ERR(f);
		} else {
			fsnotify_open(f);
			fd_install(fd, f);
		}
	}
	return fd;
}

*/
int copy_file(char* dest_filename, char* source_filename){
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
SYSCALL_DEFINE1(isolate_cpu, int , cpuid){
    
    /*umode_t mode=0;
    int fd;
    printk("trying to open file\n");
    char path []= "abc.txt";
    fd=my_open(path, O_RDWR);//do_sys_open(-100,"abc.txt", O_RDWR,mode);
    printk("Returned file descriptor = %d\n", fd);
    printk("Trying to write to file\n");
    //work_with_this write
    ksys_write(fd, "I AM ABHISHEK\n", 14);*/

    // struct file *f = filp_open("pqr.txt", O_RDWR, 0);
    // long long offset=0;
    // kernel_write(f, "I AM ABHISHEK", 13, &offset);
    // filp_close(f, NULL);
    // offset=10;
    // f = filp_open("pqr.txt", O_RDWR, 0);
    // kernel_write(f, "I AM HITECH", 11, &offset);
    // filp_close(f, NULL);

    /*printk("Offset after first write = %lld\n", offset);

    kernel_write(f, "I AM ABHISHEK", 13, 0);
    printk("Offset after second write = %lld\n", offset);
    */

    /*struct file *f = filp_open("file.txt", O_RDWR, 0);
    long long offset=0;
    char buf [10];
    int copy=cpuid;
    int digit_count=0;
    int count=0;
    while(copy){
        copy/=10;
        digit_count++;
    }
    count = digit_count+1;
    buf[digit_count+1]=0;
    buf[digit_count--]='\n';
    while (cpuid){
        buf[digit_count--]= cpuid%10+'0';
        cpuid/=10;
    }*/
    //printk("%s, %d", buf, count);
    //kernel_write(f, buf, count, &offset);
    //kernel_write(f, buf, count, &offset);


    // printk("Trying to copy files\n");
    
    // copy_file("abc_copy.txt", "abc.txt");

    // printk("Trying to make a directory\n");
    // do_mkdirat(AT_FDCWD, my_getname("/dev/cpuset"), 0);

    // printk("Trying to mount the cpuset file system");

    // my_mount(0, "/cpusets", "cpuset", 0, 0);    
    // remove_cpu(cpuid);
    // //printk("This message is printed from a system call by Abhishek Ghosh");
    // housekeeping_isolcpus_setup("domain,managed_irq,0");
    // housekeeping_nohz_full_setup("0");
    // add_cpu(cpuid);

    struct file *f ;
    long long offset = 0;
    printk("Trying to make /cpusets directory ....\n");
    do_mkdirat(AT_FDCWD, my_getname("/cpusets"),0);
    
    printk("Trying to mount the cpuset filesystem at /cpusets");
    my_mount(0, "/cpusets", "cpuset", 0, 0);

    printk("Trying to make the directory /cpusets/housekeeping\n");
    do_mkdirat(AT_FDCWD, my_getname("/cpusets/housekeeping"),0);
    
    printk("Trying to make the directory /cpusets/isolated\n");
    do_mkdirat(AT_FDCWD, my_getname("/cpusets/isolated"),0);

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
    copy_file("/cpusets/housekeeping/tasks", "/cpusets/tasks");


    return 0;
}

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
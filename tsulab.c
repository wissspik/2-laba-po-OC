#include <linux/module.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/timekeeping.h>
#include <linux/math64.h>
#include <linux/errno.h>

static struct proc_dir_entry *proc_file;
static u64 start_time;

/* Form the contents of /proc/tsulab. */
static int tsulab_show(struct seq_file *m, void *v)
{
    u64 elapsed = ktime_get_boottime_ns() - start_time;
    u64 seconds = div_u64(elapsed, NSEC_PER_SEC);

    seq_printf(m, "Seconds since module load: %llu\n",
               (unsigned long long)seconds);
    return 0;
}

static int tsulab_open(struct inode *inode, struct file *file)
{
    return single_open(file, tsulab_show, NULL);
}

static const struct proc_ops proc_file_ops = {
    .proc_open = tsulab_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static int __init tsulab_init(void)
{
    start_time = ktime_get_boottime_ns();
    proc_file = proc_create("tsulab", 0444, NULL, &proc_file_ops);
    if (!proc_file)
        return -ENOMEM;

    pr_info("Welcome to the Tomsk State University\n");
    return 0;
}

static void __exit tsulab_exit(void)
{
    proc_remove(proc_file);
    pr_info("Tomsk State University forever!\n");
}

module_init(tsulab_init);
module_exit(tsulab_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TSU lab 3: kernel module and /proc");

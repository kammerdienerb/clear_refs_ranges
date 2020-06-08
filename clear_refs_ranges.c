#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/pid_namespace.h>
#include <linux/swap.h>
#include <linux/swapops.h>

#define DRIVER_AUTHOR "Brandon Kammerdiener <bkammerd@vols.utk.edu>"
#define DRIVER_DESC   "Extend the clear_refs /proc functionality to page ranges"


#define CLEAR_REFS_MAGIC (0xC1EA77EF)


#define DBUG(fmt, ...) \
    printk(KERN_DEBUG "clear_refs_ranges: " fmt, ##__VA_ARGS__);
#define INFO(fmt, ...) \
    printk(KERN_INFO  "clear_refs_ranges: " fmt, ##__VA_ARGS__);



__attribute__((packed))
struct clear_refs_ranges_info {
    unsigned int  _magic;
    pid_t         pid;
    void         *range_start;
    void         *range_end;
};



#define ASSIGN_OR(lval, rval, err_code, fmt, ...) \
do {                                              \
    if (!((lval) = (rval))) {                     \
        DBUG(fmt, ##__VA_ARGS__);                 \
        status = (err_code);                      \
        goto out;                                 \
    }                                             \
} while (0)

static int get_mm(struct clear_refs_ranges_info *info, struct mm_struct **mm) {
    int                 status;
    struct task_struct *task;
    struct pid         *pid;

    *mm = NULL;

    status = 0;

    ASSIGN_OR(pid,  find_get_pid(info->pid),    -ESRCH, "couldn't find pid\n");
    ASSIGN_OR(task, pid_task(pid, PIDTYPE_PID), -ESRCH, "couldn't find task\n");
    ASSIGN_OR(*mm,  get_task_mm(task),          0,      "task has no mm\n");

out:
    return status;
}

/* Call pte_unmap() after using the returned pte_t. */
static pte_t * vpage_to_pte(struct mm_struct *mm, unsigned long vpage) {
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;

#define CHECK_OR_RETURN_NULL(which) \
    if (which##_none(*which) || which##_bad(*which)) { return NULL; }

    pgd = pgd_offset(mm, vpage);  CHECK_OR_RETURN_NULL(pgd);
    p4d = p4d_offset(pgd, vpage); CHECK_OR_RETURN_NULL(p4d);
    pud = pud_offset(p4d, vpage); CHECK_OR_RETURN_NULL(pud);
    pmd = pmd_offset(pud, vpage); CHECK_OR_RETURN_NULL(pmd);
    pte = pte_offset_map(pmd, vpage);

    return pte;
}

static int crr(struct clear_refs_ranges_info *info) {
    int                    status;
    struct mm_struct      *mm;
    struct vm_area_struct *vma;
    unsigned long long     vpage;
    pte_t                  old_pte,
                           pte,
                          *ptep;

    status = 0;
    mm     = NULL;

    if ((status = get_mm(info, &mm)) || !mm) { goto out; }

    down_write(&mm->mmap_sem);

    if (!mm->mmap) { DBUG("mm has no mmap\n"); }

    vma = NULL;

    for (vpage = (unsigned long long)info->range_start;
         vpage < (unsigned long long)info->range_end;
         vpage += PAGE_SIZE) {

        /* Get the correct vma for the page. */
        if (!vma || vpage < vma->vm_start || vpage >= vma->vm_end) {
            vma = find_vma(mm, vpage);
        }

        ptep = vpage_to_pte(mm, vpage);

        if (ptep) {
            pte = *ptep;

            if (pte_present(pte)) {
                old_pte = ptep_modify_prot_start(vma, vpage, ptep);
                pte = pte_wrprotect(old_pte);
                pte = pte_clear_soft_dirty(pte);
                ptep_modify_prot_commit(vma, vpage, ptep, old_pte, pte);
            } else if (is_swap_pte(pte)) {
                pte = pte_swp_clear_soft_dirty(pte);
                set_pte_at(vma->vm_mm, vpage, ptep, pte);
            }
        }
    }

    up_write(&mm->mmap_sem);

out:
    return status;
}










struct proc_dir_entry *pf_ent;

static ssize_t pf_w(struct file *file, const char *buf, size_t count, loff_t *ppos) {
    struct clear_refs_ranges_info *info_p, info;
    int                            status;

    if (count != sizeof(struct clear_refs_ranges_info)) {
        DBUG("write payload was not the correct size\n");
        return -EINVAL;
    }

    info_p = (struct clear_refs_ranges_info*)buf;

    if (copy_from_user(&info, info_p, sizeof(info)) != 0) {
        DBUG("error copying info from user space to kernel space\n");
        return -EFAULT;
    }

    if (info._magic != CLEAR_REFS_MAGIC) {
        DBUG("write payload has incorrect magic value\n");
        return -EINVAL;
    }

    if (info.range_end <= info.range_start) {
        DBUG("range_end >= range_start\n");
        return -EINVAL;
    }

    if (((unsigned long long)info.range_start) & (PAGE_SIZE - 1)
    ||  ((unsigned long long)info.range_end)   & (PAGE_SIZE - 1)) {
        DBUG("misaligned range value(s)\n");
        return -EINVAL;
    }

    status = crr(&info);
    if (status < 0) {
        DBUG("there was an error\n");
        return status;
    }

    return count;
}


/* https://patchwork.kernel.org/patch/11363867 */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(5,6,0)
static const struct file_operations pf_ops = {
    .owner = THIS_MODULE,
    .write = pf_w,
};
#else
static const struct proc_ops pf_ops = {
    .proc_write = pf_w,
};
#endif /* LINUX_VERSION_CODE <= KERNEL_VERSION(5,6,0) */







static int __init init_clear_refs_ranges(void) {
    pf_ent = proc_create("clear_refs_ranges", 0660, NULL, &pf_ops);

    if (pf_ent == NULL) { return -ENOMEM; }

    INFO("initialized\n");

    return 0;
}

static void __exit cleanup_clear_refs_ranges(void) {
    proc_remove(pf_ent);

    INFO("cleaned up\n");
}


module_init(init_clear_refs_ranges);
module_exit(cleanup_clear_refs_ranges);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);

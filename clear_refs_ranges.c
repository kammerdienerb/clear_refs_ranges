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
#include <linux/mempolicy.h>

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

static int crr_vma(struct clear_refs_ranges_info *info, struct vm_area_struct *vma, struct vm_area_struct **prev, unsigned long start, unsigned long end) {
	struct mm_struct *mm;
	int               error;
	unsigned long     new_flags;
	pgoff_t           pgoff;

    mm        = vma->vm_mm;
    error     = 0;
    new_flags = vma->vm_flags & ~VM_SOFTDIRTY;

    if (new_flags == vma->vm_flags) {
        *prev = vma;
        goto out;
    }

    pgoff = vma->vm_pgoff + ((start - vma->vm_start) >> PAGE_SHIFT);
    *prev = vma_merge(mm, *prev, start, end, new_flags, vma->anon_vma,
                      vma->vm_file, pgoff, vma_policy(vma),
                      vma->vm_userfaultfd_ctx);
    if (*prev) {
        vma = *prev;
        goto success;
    }

    *prev = vma;

	if (start != vma->vm_start) {
		if (unlikely(mm->map_count >= sysctl_max_map_count)) {
			error = -ENOMEM;
			goto out;
		}

		if ((error = __split_vma(mm, vma, start, 1))) { goto out; }
	}

	if (end != vma->vm_end) {
		if (unlikely(mm->map_count >= sysctl_max_map_count)) {
			error = -ENOMEM;
			goto out;
		}
		if ((error = __split_vma(mm, vma, end, 0))) { goto out; }
	}

success:
    vma->vm_flags = new_flags;

out:
    return error;
}

static int crr(struct clear_refs_ranges_info *info) {
    int                    status;
    struct mm_struct      *mm;
    struct vm_area_struct *vma;
    struct vm_area_struct *prev;
    unsigned long long     start;
    unsigned long long     end;
    unsigned long long     tmp;
    int                    did_down_write;
    unsigned long long     vpage;
    pte_t                  pte,
                          *ptep;

    status         = 0;
    mm             = NULL;
    did_down_write = 0;

    if ((status = get_mm(info, &mm)) || !mm) { goto out; }

    down_write(&mm->mmap_sem);
    did_down_write = 1;

    if (!mm->mmap) { DBUG("mm has no mmap\n"); }

    start = (unsigned long long)info->range_start;
    end   = (unsigned long long)info->range_end;

    /*
     * If the interval [start,end) covers some unmapped address
     * ranges, just ignore them, but return -ENOMEM at the end.
     * - different from the way of handling in mlock etc.
     */
    vma = find_vma_prev(mm, start, &prev);
    if (vma && start > vma->vm_start) {
        prev = vma;
    }

    for (;;) {
        /* Still start < end. */
        status = -ENOMEM;

        if (!vma) { goto out; }

        /* Here start < (end|vma->vm_end). */
        if (start < vma->vm_start) {
            start = vma->vm_start;
            if (start >= end) { goto do_ptes; }
        }

        /* Here vma->vm_start <= start < (end|vma->vm_end) */
        tmp = vma->vm_end;
        if (end < tmp) { tmp = end; }

        /* Here vma->vm_start <= start < tmp <= (end|vma->vm_end). */
        status = crr_vma(info, vma, &prev, start, tmp);

        if (status) { goto out; }

        start = tmp;

        if (prev && start < prev->vm_end) {
            start = prev->vm_end;
        }

        if (start >= end) { goto do_ptes; }

        if (prev) {
            vma = prev->vm_next;
        } else {
            vma = find_vma(mm, start);
        }
    }

do_ptes:

    for (vpage = (unsigned long long)info->range_start;
         vpage < (unsigned long long)info->range_end;
         vpage += PAGE_SIZE) {

        ptep = vpage_to_pte(mm, vpage);

        if (ptep) {
            pte = *ptep;

            if (pte_present(pte)) {
                pte = pte_wrprotect(pte);
                pte = pte_clear_soft_dirty(pte);
                set_pte_at(vma->vm_mm, vpage, ptep, pte);
            } else if (is_swap_pte(pte)) {
                pte = pte_swp_clear_soft_dirty(pte);
                set_pte_at(vma->vm_mm, vpage, ptep, pte);
            }
        }
    }

out:
    if (did_down_write) { up_write(&mm->mmap_sem); }

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
        DBUG("there was an error -- %d\n", status);
        if (status == -12) {
            DBUG("it is likely that the cause of the error was due "
                 "to an input range that covered non-mapped memory\n",
                 status);
        }
        return status;
    }

    return count;
}


/* https://patchwork.kernel.org/patch/11363867 */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(5,6,0)
static const struct file_operations pf_ops = {
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

fs_initcall(init_clear_refs_ranges);

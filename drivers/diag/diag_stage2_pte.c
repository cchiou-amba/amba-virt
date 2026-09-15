/*
 * diag_stage2_pte.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/pid.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/kvm_host.h>
#include <asm/kvm_pgtable.h>

static int target_pid = 0;
module_param(target_pid, int, 0444);
MODULE_PARM_DESC(target_pid, "QEMU target process PID");

static unsigned long target_gpa = 0x8000000000ULL;
module_param(target_gpa, ulong, 0444);
MODULE_PARM_DESC(target_gpa, "Target GPA to inspect (default 0x8000000000)");

typedef int (*fn_kvm_pgtable_get_leaf)(struct kvm_pgtable *pgt, u64 addr,
				       kvm_pte_t *ptep, u32 *level);

static int __init diag_stage2_init(void)
{
	struct task_struct *task;
	struct pid *pid_struct;
	struct files_struct *files;
	struct kvm *kvm = NULL;
	fn_kvm_pgtable_get_leaf get_leaf;
	struct kprobe kp = { .symbol_name = "kvm_pgtable_get_leaf" };
	kvm_pte_t pte = 0;
	u32 level = 0;
	int ret, fd;

	if (target_pid <= 0) {
		pr_err("diag_stage2_pte: target_pid parameter is required\n");
		return -EINVAL;
	}

	ret = register_kprobe(&kp);
	if (ret < 0) {
		pr_err("diag_stage2_pte: failed to resolve kvm_pgtable_get_leaf via kprobe: %d\n", ret);
		return ret;
	}
	get_leaf = (fn_kvm_pgtable_get_leaf)kp.addr;
	unregister_kprobe(&kp);

	rcu_read_lock();
	pid_struct = find_vpid(target_pid);
	if (!pid_struct) {
		rcu_read_unlock();
		pr_err("diag_stage2_pte: PID %d not found\n", target_pid);
		return -ESRCH;
	}
	task = pid_task(pid_struct, PIDTYPE_PID);
	if (!task) {
		rcu_read_unlock();
		pr_err("diag_stage2_pte: task for PID %d not found\n", target_pid);
		return -ESRCH;
	}
	get_task_struct(task);
	rcu_read_unlock();

	task_lock(task);
	files = task->files;
	if (!files) {
		task_unlock(task);
		put_task_struct(task);
		pr_err("diag_stage2_pte: task %d has no files\n", target_pid);
		return -EINVAL;
	}

	rcu_read_lock();
	for (fd = 0; fd < 256; fd++) {
		struct file *f = files_lookup_fd_rcu(files, fd);
		if (f && f->f_path.dentry) {
			const char *name = f->f_path.dentry->d_name.name;
			if (name && !strcmp(name, "kvm-vm")) {
				kvm = f->private_data;
				break;
			}
		}
	}
	rcu_read_unlock();
	task_unlock(task);
	put_task_struct(task);

	if (!kvm) {
		pr_err("diag_stage2_pte: could not find kvm-vm fd in PID %d\n", target_pid);
		return -ENODEV;
	}

	read_lock(&kvm->mmu_lock);
	ret = get_leaf(kvm->arch.mmu.pgt, target_gpa, &pte, &level);
	read_unlock(&kvm->mmu_lock);

	if (ret < 0) {
		pr_err("diag_stage2_pte: kvm_pgtable_get_leaf failed for GPA 0x%lx: %d\n", target_gpa, ret);
		return ret;
	}

	{
		bool valid = pte & 1;
		u8 memattr = (pte >> 2) & 0xf;
		u8 s2ap = (pte >> 6) & 0x3;
		u8 sh = (pte >> 8) & 0x3;
		u8 af = (pte >> 10) & 0x1;
		u64 hpa = pte & 0x0000fffffffff000ULL;
		const char *attr_name = "Unknown";

		if (memattr == 0x5)
			attr_name = "Normal-NC (MT_S2_FWB_NORMAL_NC / MT_S2_NORMAL_NC)";
		else if (memattr == 0x6)
			attr_name = "Normal-WB (MT_S2_FWB_NORMAL)";
		else if (memattr == 0x1)
			attr_name = "Device-nGnRE";

		pr_info("============================================================\n");
		pr_info("Stage-2 PTE Dump for PID %d GPA 0x%010lx:\n", target_pid, target_gpa);
		pr_info("  Raw PTE:      0x%016llx (Level %u, %s)\n", (unsigned long long)pte, level, valid ? "VALID" : "INVALID");
		pr_info("  Target HPA:   0x%010llx\n", (unsigned long long)hpa);
		pr_info("  MemAttr[5:2]: 0x%x -> %s\n", memattr, attr_name);
		pr_info("  S2AP[7:6]:    0x%x (%s)\n", s2ap, (s2ap == 3) ? "Read/Write" : (s2ap == 1) ? "Read-Only" : "Other");
		pr_info("  SH[9:8]:      0x%x (%s)\n", sh, (sh == 3) ? "Inner Shareable" : (sh == 2) ? "Outer Shareable" : "Non-shareable");
		pr_info("  AF[10]:       %u (Access Flag)\n", af);
		pr_info("============================================================\n");
	}

	return 0;
}

static void __exit diag_stage2_exit(void)
{
}

module_init(diag_stage2_init);
module_exit(diag_stage2_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ambarella KVM Stage-2 PTE diagnostic tool");
MODULE_AUTHOR("Ambarella International LLC");

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */

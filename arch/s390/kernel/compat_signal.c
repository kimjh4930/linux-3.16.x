/*
 *    Copyright IBM Corp. 2000, 2006
 *    Author(s): Denis Joseph Barrow (djbarrow@de.ibm.com,barrow_dj@yahoo.com)
 *               Gerhard Tonn (ton@de.ibm.com)                  
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  1997-11-28  Modified for POSIX.1b signals by Richard Henderson
 */

#include <linux/compat.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/wait.h>
#include <linux/ptrace.h>
#include <linux/unistd.h>
#include <linux/stddef.h>
#include <linux/tty.h>
#include <linux/personality.h>
#include <linux/binfmts.h>
#include <asm/ucontext.h>
#include <asm/uaccess.h>
#include <asm/lowcore.h>
#include <asm/switch_to.h>
#include "compat_linux.h"
#include "compat_ptrace.h"
#include "entry.h"

typedef struct 
{
	__u8 callee_used_stack[__SIGNAL_FRAMESIZE32];
	struct sigcontext32 sc;
	_sigregs32 sregs;
	int signo;
	__u32 gprs_high[NUM_GPRS];
	__u8 retcode[S390_SYSCALL_SIZE];
} sigframe32;

typedef struct 
{
	__u8 callee_used_stack[__SIGNAL_FRAMESIZE32];
	__u8 retcode[S390_SYSCALL_SIZE];
	compat_siginfo_t info;
	struct ucontext32 uc;
	__u32 gprs_high[NUM_GPRS];
} rt_sigframe32;

int copy_siginfo_to_user32(compat_siginfo_t __user *to, const siginfo_t *from)
{
	int err;

	/* If you change siginfo_t structure, please be sure
	   this code is fixed accordingly.
	   It should never copy any pad contained in the structure
	   to avoid security leaks, but must copy the generic
	   3 ints plus the relevant union member.  
	   This routine must convert siginfo from 64bit to 32bit as well
	   at the same time.  */
	err = __put_user(from->si_signo, &to->si_signo);
	err |= __put_user(from->si_errno, &to->si_errno);
	err |= __put_user((short)from->si_code, &to->si_code);
	if (from->si_code < 0)
		err |= __copy_to_user(&to->_sifields._pad, &from->_sifields._pad, SI_PAD_SIZE);
	else {
		switch (from->si_code >> 16) {
		case __SI_RT >> 16: /* This is not generated by the kernel as of now.  */
		case __SI_MESGQ >> 16:
			err |= __put_user(from->si_int, &to->si_int);
			/* fallthrough */
		case __SI_KILL >> 16:
			err |= __put_user(from->si_pid, &to->si_pid);
			err |= __put_user(from->si_uid, &to->si_uid);
			break;
		case __SI_CHLD >> 16:
			err |= __put_user(from->si_pid, &to->si_pid);
			err |= __put_user(from->si_uid, &to->si_uid);
			err |= __put_user(from->si_utime, &to->si_utime);
			err |= __put_user(from->si_stime, &to->si_stime);
			err |= __put_user(from->si_status, &to->si_status);
			break;
		case __SI_FAULT >> 16:
			err |= __put_user((unsigned long) from->si_addr,
					  &to->si_addr);
			break;
		case __SI_POLL >> 16:
			err |= __put_user(from->si_band, &to->si_band);
			err |= __put_user(from->si_fd, &to->si_fd);
			break;
		case __SI_TIMER >> 16:
			err |= __put_user(from->si_tid, &to->si_tid);
			err |= __put_user(from->si_overrun, &to->si_overrun);
			err |= __put_user(from->si_int, &to->si_int);
			break;
		default:
			break;
		}
	}
	return err ? -EFAULT : 0;
}

int copy_siginfo_from_user32(siginfo_t *to, compat_siginfo_t __user *from)
{
	int err;
	u32 tmp;

	err = __get_user(to->si_signo, &from->si_signo);
	err |= __get_user(to->si_errno, &from->si_errno);
	err |= __get_user(to->si_code, &from->si_code);

	if (to->si_code < 0)
		err |= __copy_from_user(&to->_sifields._pad, &from->_sifields._pad, SI_PAD_SIZE);
	else {
		switch (to->si_code >> 16) {
		case __SI_RT >> 16: /* This is not generated by the kernel as of now.  */
		case __SI_MESGQ >> 16:
			err |= __get_user(to->si_int, &from->si_int);
			/* fallthrough */
		case __SI_KILL >> 16:
			err |= __get_user(to->si_pid, &from->si_pid);
			err |= __get_user(to->si_uid, &from->si_uid);
			break;
		case __SI_CHLD >> 16:
			err |= __get_user(to->si_pid, &from->si_pid);
			err |= __get_user(to->si_uid, &from->si_uid);
			err |= __get_user(to->si_utime, &from->si_utime);
			err |= __get_user(to->si_stime, &from->si_stime);
			err |= __get_user(to->si_status, &from->si_status);
			break;
		case __SI_FAULT >> 16:
			err |= __get_user(tmp, &from->si_addr);
			to->si_addr = (void __force __user *)
				(u64) (tmp & PSW32_ADDR_INSN);
			break;
		case __SI_POLL >> 16:
			err |= __get_user(to->si_band, &from->si_band);
			err |= __get_user(to->si_fd, &from->si_fd);
			break;
		case __SI_TIMER >> 16:
			err |= __get_user(to->si_tid, &from->si_tid);
			err |= __get_user(to->si_overrun, &from->si_overrun);
			err |= __get_user(to->si_int, &from->si_int);
			break;
		default:
			break;
		}
	}
	return err ? -EFAULT : 0;
}

static int save_sigregs32(struct pt_regs *regs, _sigregs32 __user *sregs)
{
	_sigregs32 user_sregs;
	int i;

	user_sregs.regs.psw.mask = (__u32)(regs->psw.mask >> 32);
	user_sregs.regs.psw.mask &= PSW32_MASK_USER | PSW32_MASK_RI;
	user_sregs.regs.psw.mask |= PSW32_USER_BITS;
	user_sregs.regs.psw.addr = (__u32) regs->psw.addr |
		(__u32)(regs->psw.mask & PSW_MASK_BA);
	for (i = 0; i < NUM_GPRS; i++)
		user_sregs.regs.gprs[i] = (__u32) regs->gprs[i];
	save_access_regs(current->thread.acrs);
	memcpy(&user_sregs.regs.acrs, current->thread.acrs,
	       sizeof(user_sregs.regs.acrs));
	save_fp_ctl(&current->thread.fp_regs.fpc);
	save_fp_regs(current->thread.fp_regs.fprs);
	memcpy(&user_sregs.fpregs, &current->thread.fp_regs,
	       sizeof(user_sregs.fpregs));
	if (__copy_to_user(sregs, &user_sregs, sizeof(_sigregs32)))
		return -EFAULT;
	return 0;
}

static int restore_sigregs32(struct pt_regs *regs,_sigregs32 __user *sregs)
{
	_sigregs32 user_sregs;
	int i;

	/* Alwys make any pending restarted system call return -EINTR */
	current_thread_info()->restart_block.fn = do_no_restart_syscall;

	if (__copy_from_user(&user_sregs, &sregs->regs, sizeof(user_sregs)))
		return -EFAULT;

	if (!is_ri_task(current) && (user_sregs.regs.psw.mask & PSW32_MASK_RI))
		return -EINVAL;

	/* Loading the floating-point-control word can fail. Do that first. */
	if (restore_fp_ctl(&user_sregs.fpregs.fpc))
		return -EINVAL;

	/* Use regs->psw.mask instead of PSW_USER_BITS to preserve PER bit. */
	regs->psw.mask = (regs->psw.mask & ~(PSW_MASK_USER | PSW_MASK_RI)) |
		(__u64)(user_sregs.regs.psw.mask & PSW32_MASK_USER) << 32 |
		(__u64)(user_sregs.regs.psw.mask & PSW32_MASK_RI) << 32 |
		(__u64)(user_sregs.regs.psw.addr & PSW32_ADDR_AMODE);
	/* Check for invalid user address space control. */
	if ((regs->psw.mask & PSW_MASK_ASC) == PSW_ASC_HOME)
		regs->psw.mask = PSW_ASC_PRIMARY |
			(regs->psw.mask & ~PSW_MASK_ASC);
	regs->psw.addr = (__u64)(user_sregs.regs.psw.addr & PSW32_ADDR_INSN);
	for (i = 0; i < NUM_GPRS; i++)
		regs->gprs[i] = (__u64) user_sregs.regs.gprs[i];
	memcpy(&current->thread.acrs, &user_sregs.regs.acrs,
	       sizeof(current->thread.acrs));
	restore_access_regs(current->thread.acrs);

	memcpy(&current->thread.fp_regs, &user_sregs.fpregs,
	       sizeof(current->thread.fp_regs));

	restore_fp_regs(current->thread.fp_regs.fprs);
	clear_pt_regs_flag(regs, PIF_SYSCALL); /* No longer in a system call */
	return 0;
}

static int save_sigregs_gprs_high(struct pt_regs *regs, __u32 __user *uregs)
{
	__u32 gprs_high[NUM_GPRS];
	int i;

	for (i = 0; i < NUM_GPRS; i++)
		gprs_high[i] = regs->gprs[i] >> 32;
	if (__copy_to_user(uregs, &gprs_high, sizeof(gprs_high)))
		return -EFAULT;
	return 0;
}

static int restore_sigregs_gprs_high(struct pt_regs *regs, __u32 __user *uregs)
{
	__u32 gprs_high[NUM_GPRS];
	int i;

	if (__copy_from_user(&gprs_high, uregs, sizeof(gprs_high)))
		return -EFAULT;
	for (i = 0; i < NUM_GPRS; i++)
		*(__u32 *)&regs->gprs[i] = gprs_high[i];
	return 0;
}

COMPAT_SYSCALL_DEFINE0(sigreturn)
{
	struct pt_regs *regs = task_pt_regs(current);
	sigframe32 __user *frame = (sigframe32 __user *)regs->gprs[15];
	sigset_t set;

	if (__copy_from_user(&set.sig, &frame->sc.oldmask, _SIGMASK_COPY_SIZE32))
		goto badframe;
	set_current_blocked(&set);
	if (restore_sigregs32(regs, &frame->sregs))
		goto badframe;
	if (restore_sigregs_gprs_high(regs, frame->gprs_high))
		goto badframe;
	return regs->gprs[2];
badframe:
	force_sig(SIGSEGV, current);
	return 0;
}

COMPAT_SYSCALL_DEFINE0(rt_sigreturn)
{
	struct pt_regs *regs = task_pt_regs(current);
	rt_sigframe32 __user *frame = (rt_sigframe32 __user *)regs->gprs[15];
	sigset_t set;

	if (__copy_from_user(&set, &frame->uc.uc_sigmask, sizeof(set)))
		goto badframe;
	set_current_blocked(&set);
	if (restore_sigregs32(regs, &frame->uc.uc_mcontext))
		goto badframe;
	if (restore_sigregs_gprs_high(regs, frame->gprs_high))
		goto badframe;
	if (compat_restore_altstack(&frame->uc.uc_stack))
		goto badframe; 
	return regs->gprs[2];
badframe:
	force_sig(SIGSEGV, current);
	return 0;
}	

/*
 * Set up a signal frame.
 */


/*
 * Determine which stack to use..
 */
static inline void __user *
get_sigframe(struct k_sigaction *ka, struct pt_regs * regs, size_t frame_size)
{
	unsigned long sp;

	/* Default to using normal stack */
	sp = (unsigned long) A(regs->gprs[15]);

	/* Overflow on alternate signal stack gives SIGSEGV. */
	if (on_sig_stack(sp) && !on_sig_stack((sp - frame_size) & -8UL))
		return (void __user *) -1UL;

	/* This is the X/Open sanctioned signal stack switching.  */
	if (ka->sa.sa_flags & SA_ONSTACK) {
		if (! sas_ss_flags(sp))
			sp = current->sas_ss_sp + current->sas_ss_size;
	}

	return (void __user *)((sp - frame_size) & -8ul);
}

static inline int map_signal(int sig)
{
	if (current_thread_info()->exec_domain
	    && current_thread_info()->exec_domain->signal_invmap
	    && sig < 32)
		return current_thread_info()->exec_domain->signal_invmap[sig];
        else
		return sig;
}

static int setup_frame32(int sig, struct k_sigaction *ka,
			sigset_t *set, struct pt_regs * regs)
{
	sigframe32 __user *frame = get_sigframe(ka, regs, sizeof(sigframe32));

	if (frame == (void __user *) -1UL)
		goto give_sigsegv;

	if (__copy_to_user(&frame->sc.oldmask, &set->sig, _SIGMASK_COPY_SIZE32))
		goto give_sigsegv;

	if (save_sigregs32(regs, &frame->sregs))
		goto give_sigsegv;
	if (save_sigregs_gprs_high(regs, frame->gprs_high))
		goto give_sigsegv;
	if (__put_user((unsigned long) &frame->sregs, &frame->sc.sregs))
		goto give_sigsegv;

	/* Set up to return from userspace.  If provided, use a stub
	   already in userspace.  */
	if (ka->sa.sa_flags & SA_RESTORER) {
		regs->gprs[14] = (__u64 __force) ka->sa.sa_restorer | PSW32_ADDR_AMODE;
	} else {
		regs->gprs[14] = (__u64 __force) frame->retcode | PSW32_ADDR_AMODE;
		if (__put_user(S390_SYSCALL_OPCODE | __NR_sigreturn,
			       (u16 __force __user *)(frame->retcode)))
			goto give_sigsegv;
        }

	/* Set up backchain. */
	if (__put_user(regs->gprs[15], (unsigned int __user *) frame))
		goto give_sigsegv;

	/* Set up registers for signal handler */
	regs->gprs[15] = (__force __u64) frame;
	/* Force 31 bit amode and default user address space control. */
	regs->psw.mask = PSW_MASK_BA |
		(PSW_USER_BITS & PSW_MASK_ASC) |
		(regs->psw.mask & ~PSW_MASK_ASC);
	regs->psw.addr = (__force __u64) ka->sa.sa_handler;

	regs->gprs[2] = map_signal(sig);
	regs->gprs[3] = (__force __u64) &frame->sc;

	/* We forgot to include these in the sigcontext.
	   To avoid breaking binary compatibility, they are passed as args. */
	if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL ||
	    sig == SIGTRAP || sig == SIGFPE) {
		/* set extra registers only for synchronous signals */
		regs->gprs[4] = regs->int_code & 127;
		regs->gprs[5] = regs->int_parm_long;
		regs->gprs[6] = task_thread_info(current)->last_break;
	}

	/* Place signal number on stack to allow backtrace from handler.  */
	if (__put_user(regs->gprs[2], (int __force __user *) &frame->signo))
		goto give_sigsegv;
	return 0;

give_sigsegv:
	force_sigsegv(sig, current);
	return -EFAULT;
}

static int setup_rt_frame32(int sig, struct k_sigaction *ka, siginfo_t *info,
			   sigset_t *set, struct pt_regs * regs)
{
	int err = 0;
	rt_sigframe32 __user *frame = get_sigframe(ka, regs, sizeof(rt_sigframe32));

	if (frame == (void __user *) -1UL)
		goto give_sigsegv;

	if (copy_siginfo_to_user32(&frame->info, info))
		goto give_sigsegv;

	/* Create the ucontext.  */
	err |= __put_user(UC_EXTENDED, &frame->uc.uc_flags);
	err |= __put_user(0, &frame->uc.uc_link);
	err |= __compat_save_altstack(&frame->uc.uc_stack, regs->gprs[15]);
	err |= save_sigregs32(regs, &frame->uc.uc_mcontext);
	err |= save_sigregs_gprs_high(regs, frame->gprs_high);
	err |= __copy_to_user(&frame->uc.uc_sigmask, set, sizeof(*set));
	if (err)
		goto give_sigsegv;

	/* Set up to return from userspace.  If provided, use a stub
	   already in userspace.  */
	if (ka->sa.sa_flags & SA_RESTORER) {
		regs->gprs[14] = (__u64 __force) ka->sa.sa_restorer | PSW32_ADDR_AMODE;
	} else {
		regs->gprs[14] = (__u64 __force) frame->retcode | PSW32_ADDR_AMODE;
		if (__put_user(S390_SYSCALL_OPCODE | __NR_rt_sigreturn,
			       (u16 __force __user *)(frame->retcode)))
			goto give_sigsegv;
	}

	/* Set up backchain. */
	if (__put_user(regs->gprs[15], (unsigned int __force __user *) frame))
		goto give_sigsegv;

	/* Set up registers for signal handler */
	regs->gprs[15] = (__force __u64) frame;
	/* Force 31 bit amode and default user address space control. */
	regs->psw.mask = PSW_MASK_BA |
		(PSW_USER_BITS & PSW_MASK_ASC) |
		(regs->psw.mask & ~PSW_MASK_ASC);
	regs->psw.addr = (__u64 __force) ka->sa.sa_handler;

	regs->gprs[2] = map_signal(sig);
	regs->gprs[3] = (__force __u64) &frame->info;
	regs->gprs[4] = (__force __u64) &frame->uc;
	regs->gprs[5] = task_thread_info(current)->last_break;
	return 0;

give_sigsegv:
	force_sigsegv(sig, current);
	return -EFAULT;
}

/*
 * OK, we're invoking a handler
 */	

void handle_signal32(unsigned long sig, struct k_sigaction *ka,
		    siginfo_t *info, sigset_t *oldset, struct pt_regs *regs)
{
	int ret;

	/* Set up the stack frame */
	if (ka->sa.sa_flags & SA_SIGINFO)
		ret = setup_rt_frame32(sig, ka, info, oldset, regs);
	else
		ret = setup_frame32(sig, ka, oldset, regs);
	if (ret)
		return;
	signal_delivered(sig, info, ka, regs,
				 test_thread_flag(TIF_SINGLE_STEP));
}


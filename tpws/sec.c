#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include "sec.h"
#include <unistd.h>
#include <fcntl.h>
#include <grp.h>

#ifdef __linux__

#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <syscall.h>

/************ SECCOMP ************/
#ifdef __X32_SYSCALL_BIT
#define X32_SYSCALL_BIT __X32_SYSCALL_BIT
#else
#define X32_SYSCALL_BIT 0x40000000
#endif
// block most of the undesired syscalls to harden against code execution
static long blocked_syscalls[] = {
#ifdef SYS_execv
SYS_execv,
#endif
SYS_execve,SYS_execveat,
#ifdef SYS_exec_with_loader
SYS_exec_with_loader,
#endif
SYS_clone,
#ifdef SYS_clone3
SYS_clone3,
#endif
#ifdef SYS_osf_execve
SYS_osf_execve,
#endif
#ifdef SYS_fork
SYS_fork,
#endif
#ifdef SYS_vfork
SYS_vfork,
#endif
#ifdef SYS_unlink
SYS_unlink,
#endif
SYS_unlinkat,
#ifdef SYS_chmod
SYS_chmod,
#endif
SYS_fchmod,SYS_fchmodat,
#ifdef SYS_chown
SYS_chown,
#endif
#ifdef SYS_chown32
SYS_chown32,
#endif
SYS_fchown,
#ifdef SYS_fchown32
SYS_fchown32,
#endif
#ifdef SYS_lchown
SYS_lchown,
#endif
#ifdef SYS_lchown32
SYS_lchown32,
#endif
SYS_fchownat,
#ifdef SYS_symlink
SYS_symlink,
#endif
SYS_symlinkat,
#ifdef SYS_link
SYS_link,
#endif
SYS_linkat,
SYS_pkey_mprotect,SYS_mprotect,
SYS_truncate,
#ifdef SYS_truncate64
SYS_truncate64,
#endif
SYS_ftruncate,
#ifdef SYS_ftruncate64
SYS_ftruncate64,
#endif
#ifdef SYS_mknod
SYS_mknod,
#endif
SYS_mknodat,
#ifdef SYS_mkdir
SYS_mkdir,
#endif
SYS_mkdirat,
#ifdef SYS_rmdir
SYS_rmdir,
#endif
#ifdef SYS_rename
SYS_rename,
#endif
SYS_renameat,SYS_renameat2
};
#define BLOCKED_SYSCALL_COUNT (sizeof(blocked_syscalls)/sizeof(*blocked_syscalls))

static void set_filter(struct sock_filter *filter, __u16 code, __u8 jt, __u8 jf, __u32 k)
{
	filter->code = code;
	filter->jt = jt;
	filter->jf = jf;
	filter->k = k;
}
// deny all blocked syscalls
bool set_seccomp()
{
#define SECCOMP_PROG_SIZE (6 + BLOCKED_SYSCALL_COUNT)
	struct sock_fprog prog = { .len = SECCOMP_PROG_SIZE };
	int res,i,idx=0;

	prog.filter = calloc(SECCOMP_PROG_SIZE, sizeof(*prog.filter));
	if (!prog.filter) return false;
	set_filter(&prog.filter[idx++], BPF_LD + BPF_W + BPF_ABS, 0, 0, arch_nr);
	set_filter(&prog.filter[idx++], BPF_JMP + BPF_JEQ + BPF_K, 0, 3 + BLOCKED_SYSCALL_COUNT, ARCH_NR); // fail
	set_filter(&prog.filter[idx++], BPF_LD + BPF_W + BPF_ABS, 0, 0, syscall_nr);
	set_filter(&prog.filter[idx++], BPF_JMP + BPF_JGT + BPF_K, 1 + BLOCKED_SYSCALL_COUNT, 0, X32_SYSCALL_BIT - 1); // fail
/*
	// ! THIS IS NOT WORKING BECAUSE perror() in glibc dups() stderr
	set_filter(&prog.filter[idx++], BPF_JMP + BPF_JEQ + BPF_K, 0, 3, SYS_write); // special check for write call
	set_filter(&prog.filter[idx++], BPF_LD + BPF_W + BPF_ABS, 0, 0, syscall_arg(0)); // fd
	set_filter(&prog.filter[idx++], BPF_JMP + BPF_JGT + BPF_K, 2 + BLOCKED_SYSCALL_COUNT, 0, 2); // 1 - stdout, 2 - stderr. greater are bad
	set_filter(&prog.filter[idx++], BPF_LD + BPF_W + BPF_ABS, 0, 0, syscall_nr); // reload syscall_nr
*/
	for(i=0 ; i<BLOCKED_SYSCALL_COUNT ; i++)
	{
		set_filter(&prog.filter[idx++], BPF_JMP + BPF_JEQ + BPF_K, BLOCKED_SYSCALL_COUNT-i, 0, blocked_syscalls[i]);
	}
	set_filter(&prog.filter[idx++], BPF_RET + BPF_K, 0, 0, SECCOMP_RET_ALLOW); // success case
	set_filter(&prog.filter[idx++], BPF_RET + BPF_K, 0, 0, SECCOMP_RET_KILL); // fail case
	res=prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
	free(prog.filter);
	return res>=0;
}



bool sec_harden()
{
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
	{
		perror("PR_SET_NO_NEW_PRIVS(prctl)");
		return false;
	}
#if ARCH_NR!=0
	if (!set_seccomp())
	{
		perror("seccomp");
		return false;
	}
#endif
	return true;
}



bool checkpcap(uint64_t caps)
{
	if (!caps) return true; // no special caps reqd

	struct __user_cap_header_struct ch = {_LINUX_CAPABILITY_VERSION_3, getpid()};
	struct __user_cap_data_struct cd[2];
	uint32_t c0 = (uint32_t)caps;
	uint32_t c1 = (uint32_t)(caps>>32);

	return !capget(&ch,cd) && (cd[0].effective & c0)==c0 && (cd[1].effective & c1)==c1;
}
bool setpcap(uint64_t caps)
{
	struct __user_cap_header_struct ch = {_LINUX_CAPABILITY_VERSION_3, getpid()};
	struct __user_cap_data_struct cd[2];
	
	cd[0].effective = cd[0].permitted = (uint32_t)caps;
	cd[0].inheritable = 0;
	cd[1].effective = cd[1].permitted = (uint32_t)(caps>>32);
	cd[1].inheritable = 0;

	return !capset(&ch,cd);
}
int getmaxcap()
{
	int maxcap = CAP_LAST_CAP;
	FILE *F = fopen("/proc/sys/kernel/cap_last_cap", "r");
	if (F)
	{
		int n = fscanf(F, "%d", &maxcap);
		fclose(F);
	}
	return maxcap;

}
bool dropcaps()
{
	uint64_t caps = 0;
	int maxcap = getmaxcap();

	if (setpcap(caps|(1<<CAP_SETPCAP)))
	{
		for (int cap = 0; cap <= maxcap; cap++)
		{
			if (prctl(PR_CAPBSET_DROP, cap)<0)
			{
				fprintf(stderr, "could not drop bound cap %d\n", cap);
				perror("cap_drop_bound");
			}
		}
	}
	// now without CAP_SETPCAP
	if (!setpcap(caps))
	{
		perror("setpcap");
		return checkpcap(caps);
	}
	return true;
}
#else // __linux__

bool sec_harden()
{
	// noop
	return true;
}

#endif // __linux__



bool can_drop_root()
{
#ifdef __linux__
	// has some caps
	return checkpcap((1<<CAP_SETUID)|(1<<CAP_SETGID)|(1<<CAP_SETPCAP));
#else
	// effective root
	return !geteuid();
#endif
}

bool droproot(uid_t uid, gid_t gid)
{
#ifdef __linux__
	if (prctl(PR_SET_KEEPCAPS, 1L))
	{
		perror("prctl(PR_SET_KEEPCAPS)");
		return false;
	}
#endif
	// drop all SGIDs
	if (setgroups(0,NULL))
	{
		perror("setgroups");
		return false;
	}
	if (setgid(gid))
	{
		perror("setgid");
		return false;
	}
	if (setuid(uid))
	{
		perror("setuid");
		return false;
	}
#ifdef __linux__
	return dropcaps();
#else
	return true;
#endif
}

void print_id()
{
 int i,N;
 gid_t g[128];
 printf("Running as UID=%u GID=",getuid());
 N=getgroups(sizeof(g)/sizeof(*g),g);
 if (N>0)
 {
	for(i=0;i<N;i++)
		printf(i==(N-1) ? "%u" : "%u,", g[i]);
	printf("\n");
 }
 else
	printf("%u\n",getgid());
}

void daemonize()
{
	int pid;

	pid = fork();
	if (pid == -1)
	{
		perror("fork");
		exit(2);
	}
	else if (pid != 0)
		exit(0);

	if (setsid() == -1)
		exit(2);
	if (chdir("/") == -1)
		exit(2);
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
	/* redirect fd's 0,1,2 to /dev/null */
	open("/dev/null", O_RDWR);
	int fd;
	/* stdin */
	fd = dup(0);
	/* stdout */
	fd = dup(0);
	/* stderror */
}

bool writepid(const char *filename)
{
	FILE *F;
	if (!(F = fopen(filename, "w")))
		return false;
	fprintf(F, "%d", getpid());
	fclose(F);
	return true;
}

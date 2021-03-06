#include <bits/ioctl/pty.h>
#include <bits/ioctl/tty.h>

#include <sys/file.h>
#include <sys/proc.h>
#include <sys/creds.h>
#include <sys/fprop.h>
#include <sys/signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <string.h>
#include <format.h>
#include <sigset.h>
#include <util.h>

#include "common.h"
#include "ptyhub.h"

#define MAX_RUNNING_PROCS 4096

void close_pipe(CTX, struct proc* pc)
{
	int fd = pc->mfd;

	if(fd <= 0) return;

	(void)sys_close(fd);

	pc->mfd = -1;

	ctx->pollset = 0;

	int pid = pc->pid;

	if(pid <= 0) return;

	(void)sys_kill(pid, SIGHUP);
}

void handle_pipe(CTX, struct proc* pc)
{
	if(sys_ioctli(pc->mfd, TCFLSH, 0) >= 0)
		return;

	close_pipe(ctx, pc);
}

static int xid_is_in_use(CTX, int val)
{
	struct proc* pc = ctx->procs;
	struct proc* pe = pc + ctx->nprocs;

	for(; pc < pe; pc++) {
		int xid = pc->xid;

		if(!xid)
			continue;
		if(xid < 0)
			xid = -xid;
		if(xid == val)
			return xid;
	}

	return 0;
}

static int pick_unused_xid(CTX)
{
	int xid = ctx->lastxid + 1;
	int aprocs = ctx->nprocs_nonempty;
	int i, tries = aprocs + 1;

	if(xid > 100 && xid > 2*aprocs)
		xid = 1;

	for(i = 0; i < tries; i++)
		if(!xid_is_in_use(ctx, xid))
			break;
	if(i >= tries)
		return -EAGAIN;

	ctx->lastxid = xid;

	return xid;
}

static void update_proc_counts(CTX)
{
	int i, nprocs = ctx->nprocs;
	struct proc* procs = ctx->procs;
	int limit = 0, nonempty = 0, running = 0;

	for(i = 0; i < nprocs; i++) {
		struct proc* pc = &procs[i];

		if(!pc->xid) continue;

		nonempty++;
		limit = i + 1;

		if(pc->pid <= 0) continue;

		running++;
	}

	ctx->nprocs = limit;
	ctx->nprocs_nonempty = nonempty;
	ctx->nprocs_running = running;
	ctx->pollset = 0;

	void* end = procs + limit;

	ctx->sep = end;
	ctx->ptr = end;
}

static void wipe_proc(struct proc* pc)
{
	memzero(pc, sizeof(*pc));
}

static void drop_proc(CTX, struct proc* pc)
{
	wipe_proc(pc);
	update_proc_counts(ctx);
}

void maybe_trim_heap(CTX)
{
	void* brk = ctx->brk;
	void* ptr = ctx->ptr;
	void* end = ctx->end;

	long size = ptr - brk;
	long need = pagealign(size);

	void* cut = brk + need;

	if(cut >= end) return;

	ctx->end = sys_brk(cut);
}

static struct proc* grab_proc(CTX)
{
	struct proc* pc = ctx->procs;
	struct proc* pe = pc + ctx->nprocs;

	for(; pc < pe; pc++)
		if(!pc->xid)
			goto out;

	ctx->pollset = 0;
	ctx->ptr = ctx->sep;

	if(!(pc = heap_alloc(ctx, sizeof(*pc))))
		return pc;

	ctx->sep = ctx->ptr;
	ctx->nprocs++;
out:
	ctx->pollset = 0;

	memzero(pc, sizeof(*pc));

	return pc;
}

void check_children(CTX)
{
	int pid, status;

	while((pid = sys_waitpid(-1, &status, WNOHANG)) > 0) {
		struct proc* pc = ctx->procs;
		struct proc* pe = pc + ctx->nprocs;

		for(; pc < pe; pc++)
			if(pc->pid == pid)
				break;
		if(pc >= pe)
			return;

		notify_exit(ctx, pc, status);
		drop_proc(ctx, pc);

	} if(pid < 0 && pid != -ECHILD) {
		fail("waitpid", NULL, pid);
	}
}

void* heap_alloc(CTX, int size)
{
	void* brk = ctx->brk;
	void* ptr = ctx->ptr;
	void* end = ctx->end;
	long left;

	if(!brk) {
		brk = sys_brk(NULL);
		ptr = end = brk;
		ctx->brk = brk;
		ctx->end = end;
		ctx->procs = brk;
	}

	if((left = end - ptr) >= size)
		goto got;

	long need = pagealign(size - left);
	end = sys_brk(end + need);

	ctx->end = end;

	if((left = end - ptr) < size)
		return NULL;
got:
	ctx->ptr = ptr + size;

	return ptr;
}

static int prep_path(char* path, int len, char* name)
{
	char* p = path;
	char* e = path + len;

	p = fmtstr(p, e, CONFDIR);
	p = fmtstr(p, e, "/");
	p = fmtstr(p, e, name);

	if(p >= e) return -ENAMETOOLONG;

	*p = '\0';

	return 0;
}

static int child(int sfd, char* path, char** argv, char** envp)
{
	int ret;
	struct sigset empty;

	sigemptyset(&empty);

	if((ret = sys_sigprocmask(SIG_SETMASK, &empty, NULL)) < 0)
		fail("sigprocmask", NULL, ret);

	if((ret = sys_dup2(sfd, 0)) < 0)
		fail("dup2", "stdin", ret);
	if((ret = sys_dup2(sfd, 1)) < 0)
		fail("dup2", "stdout", ret);
	if((ret = sys_dup2(sfd, 2)) < 0)
		fail("dup2", "stderr", ret);

	if((ret = sys_setsid()) < 0)
		fail("setsid", NULL, ret);
	if(sys_ioctli(STDIN, TIOCSCTTY, 0) < 0)
		fail("ioctl", "TIOCSCTTY", ret);

	ret = sys_execve(path, argv, envp);

	fail("execve", path, ret);
}

static int spawn_proc(struct proc* pc, char* path, char** argv, char** envp)
{
	int pid, ret;
	int mfd, sfd;
	int flags = O_RDWR | O_NOCTTY | O_CLOEXEC;
	int lock = 0;

	if((mfd = sys_open("/dev/ptmx", flags)) < 0)
		return mfd;
	if((ret = sys_ioctl(mfd, TIOCSPTLCK, &lock)) < 0)
		return ret;
	if((sfd = sys_ioctli(mfd, TIOCGPTPEER, flags)) < 0)
		return sfd;

	if((pid = sys_fork()) < 0)
		return ret;

	if(pid == 0)
		_exit(child(sfd, path, argv, envp));

	sys_close(sfd);

	pc->mfd = mfd;
	pc->pid = pid;

	return 0;
}

static int validate_name(char* name)
{
	byte* p = (byte*)name;
	byte c;

	while((c = *p++))
		if(c < 0x20 || c == '/')
			return -EINVAL;

	return 0;
}

int spawn_child(CTX, char** argv, char** envp)
{
	struct proc* pc;
	char path[100];
	char* name = argv[0];
	int nlen = strlen(name);
	int xid, ret;

	if(nlen > ssizeof(pc->name) - 1)
		return -ENAMETOOLONG;
	if(validate_name(name))
		return -ENOENT;
	if(ctx->nprocs_running > MAX_RUNNING_PROCS)
		return -EMFILE;
	if((xid = pick_unused_xid(ctx)) < 0)
		return -EMFILE;

	if((ret = prep_path(path, sizeof(path), name)) < 0)
		return ret;

	if((ret = sys_access(path, X_OK)) < 0)
		return ret;
	if(!(pc = grab_proc(ctx)))
		return -ENOMEM;

	memzero(pc, sizeof(*pc));
	memcpy(pc->name, name, nlen);

	pc->xid = xid;

	if((ret = spawn_proc(pc, path, argv, envp)) < 0) {
		wipe_proc(pc);
		return ret;
	} else {
		update_proc_counts(ctx);
		return xid;
	}
}

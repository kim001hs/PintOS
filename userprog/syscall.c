#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/synch.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "userprog/process.h"
#include <string.h>
#include "threads/palloc.h"
#include "threads/malloc.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

/* 전역 파일 락.
   파일 관련 시스템 콜을 수행할 때마다 이 락을 획득하여
   파일 시스템의 손상을 방지한다.
   Pintos에는 파일별 락이 없기 때문에,
   서로 다른 파일을 접근하는 경우라도
   현재 파일 시스템 콜이 끝날 때까지 대기해야 한다. */
static struct lock filesys_lock;

static void s_halt(void) NO_RETURN;
void s_exit(int status) NO_RETURN;
static int s_fork(const char *thread_name, struct intr_frame *f);
static int s_exec(const char *file);
static int s_wait(pid_t);
static bool s_create(const char *file, unsigned initial_size);
static bool s_remove(const char *file);
static int s_open(const char *file);
static int s_filesize(int fd);
static int s_read(int fd, void *buffer, unsigned length);
static int s_write(int fd, const void *buffer, unsigned length);
static void s_seek(int fd, unsigned position);
static unsigned s_tell(int fd);
static void s_close(int fd);

static void s_check_access(const char *file);
static void s_check_buffer(const void *buffer, unsigned length);
static int realloc_fd_table(struct thread *t);
static void s_check_fd(int fd);
// extra
static int s_dup2(int oldfd, int newfd);
/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */
#define MSR_STAR 0xc0000081			/* Segment selector msr */
#define MSR_LSTAR 0xc0000082		/* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void)
{
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
							((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
	// 파일 시스템 콜용 락 init
	lock_init(&filesys_lock);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED)
{
	// TODO: Your implementation goes here.
	// %rdi, %rsi, %rdx, %r10, %r8, %r9: 시스템 콜 인자
	switch (f->R.rax)
	{
	/* Projects 2 and later. */
	case SYS_HALT:
		s_halt();
		break;
	case SYS_EXIT:
		s_exit(f->R.rdi);
		break;
	case SYS_FORK:
		f->R.rax = s_fork(f->R.rdi, f);
		break;
	case SYS_EXEC:
		f->R.rax = s_exec(f->R.rdi);
		break;
	case SYS_WAIT:
		f->R.rax = s_wait(f->R.rdi);
		break;
	case SYS_CREATE:
		f->R.rax = s_create(f->R.rdi, f->R.rsi);
		break;
	case SYS_REMOVE:
		f->R.rax = s_remove(f->R.rdi);
		break;
	case SYS_OPEN:
		f->R.rax = s_open(f->R.rdi);
		break;
	case SYS_FILESIZE:
		f->R.rax = s_filesize(f->R.rdi);
		break;
	case SYS_READ:
		f->R.rax = s_read(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_WRITE:
		f->R.rax = s_write(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_SEEK:
		s_seek(f->R.rdi, f->R.rsi);
		break;
	case SYS_TELL:
		f->R.rax = s_tell(f->R.rdi);
		break;
	case SYS_CLOSE:
		s_close(f->R.rdi);
		break;
		/* Project 3 and optionally project 4. */
		// case SYS_MMAP:
		// 	break;
		// case SYS_MUNMAP:
		// 	break;
		// /* Project 4 only. */
		// case SYS_CHDIR:
		// 	break;
		// case SYS_MKDIR:
		// 	break;
		// case SYS_READDIR:
		// 	break;
		// case SYS_ISDIR:
		// 	break;
		// case SYS_INUMBER:
		// 	break;
		// case SYS_SYMLINK:
		// 	break;
		// /* Extra for Project 2 */
	case SYS_DUP2:
		f->R.rax = s_dup2(f->R.rdi, f->R.rsi);
		break;
		// case SYS_MOUNT:
		// 	break;
		// case SYS_UMOUNT:
		// 	break;

	default:
		thread_exit();
		break;
	}
}

static void s_halt(void)
{
	power_off();
}

void s_exit(int status)
{
	struct thread *cur = thread_current();
	cur->exit_status = status;

	printf("%s: exit(%d)\n", thread_name(), status);
	thread_exit();
}

static int s_fork(const char *thread_name, struct intr_frame *f)
{
	s_check_access(thread_name);

	tid_t child_tid = process_fork(thread_name, f);

	if (child_tid == TID_ERROR)
		return TID_ERROR;

	struct thread *child = get_thread_by_tid(child_tid);

	struct thread *cur = thread_current();
	if (child == NULL)
		return TID_ERROR;

	sema_down(&child->fork_sema);

	// Check if child failed during __do_fork
	if (child->exit_status == -1)
		return TID_ERROR;

	return child_tid;
}

static int s_exec(const char *file)
{
	s_check_access(file);

	char *fn_copy = palloc_get_page(0);
	if (fn_copy == NULL)
		return -1;
	strlcpy(fn_copy, file, PGSIZE);

	int res = process_exec(fn_copy);
	if (res < 0)
		s_exit(-1);
	return res;
}

static int s_wait(int tid)
{
	return process_wait(tid);
}

static bool s_create(const char *file, unsigned initial_size)
{
	s_check_access(file);

	return filesys_create(file, initial_size);
}

static bool s_remove(const char *file)
{
	s_check_access(file);
	return filesys_remove(file);
}

static int s_open(const char *file)
{
	s_check_access(file);
	int fd = -1;

	lock_acquire(&filesys_lock);
	struct file *target_file = filesys_open(file);
	lock_release(&filesys_lock);

	if (target_file == NULL)
	{
		return -1;
	}

	struct thread *t = thread_current();

	for (int i = 0; i < t->fd_table_size; i++)
	{
		if (!t->fd_table[i])
		{
			fd = i;
			break;
		}
	}

	if (fd == -1)
	{
		if (realloc_fd_table(t) == -1)
		{
			lock_acquire(&filesys_lock);
			file_close(target_file);
			lock_release(&filesys_lock);
			return -1;
		}
		fd = t->fd_table_size / 2;
	}
	t->fd_table[fd] = target_file;
	return fd;
}

static int s_filesize(int fd)
{
	s_check_fd(fd);
	struct file *f = thread_current()->fd_table[fd];
	if (f == NULL || f == STDOUT || f == STDIN)
		return -1;
	int size;
	lock_acquire(&filesys_lock);
	size = file_length(f);
	lock_release(&filesys_lock);
	return size;
}

static int s_read(int fd, void *buffer, unsigned length)
{
	s_check_buffer(buffer, length);
	s_check_fd(fd);
	int bytes_read = 0;

	// 3. 파일 디스크립터에서 파일 찾기
	struct file *f = thread_current()->fd_table[fd];
	if (f == STDIN)
	{
		lock_acquire(&filesys_lock);
		for (unsigned i = 0; i < length; i++)
			((uint8_t *)buffer)[i] = input_getc();
		lock_release(&filesys_lock);
		return length;
	}
	if (f == NULL || f == STDOUT)
		return -1;

	// 4. 파일 읽기
	lock_acquire(&filesys_lock);
	bytes_read = file_read(f, buffer, length);
	lock_release(&filesys_lock);

	return bytes_read;
}

// write
//
/* Writes size bytes from buffer to the open file fd.
Returns the number of bytes actually written,
which may be less than size if some bytes could not be written. */
static int s_write(int fd, const void *buffer, unsigned length)
{
	s_check_buffer(buffer, length);
	s_check_fd(fd);

	// 파일에 write 하기
	struct file *curr_file = thread_current()->fd_table[fd];
	// 파일을 못 가져오면
	if (curr_file == NULL || curr_file == STDIN)
	{
		return -1;
	}
	else if (curr_file == STDOUT)
	{
		lock_acquire(&filesys_lock);
		putbuf(buffer, length);
		lock_release(&filesys_lock);
		return length;
	}
	// write 하기전에 lock
	lock_acquire(&filesys_lock);
	int written = file_write(curr_file, buffer, length); // file.h
	lock_release(&filesys_lock);

	return written;
}

static void s_seek(int fd, unsigned position)
{
	s_check_fd(fd);
	// stdin(0)과 stdout(1)은 seek 의미가 없으므로, 오류를 내지 않고 그대로 무시
	struct file *curr_file = thread_current()->fd_table[fd];
	if (curr_file == NULL)
	{
		return;
	}
	else if (curr_file == STDIN || curr_file == STDOUT)
		return;
	lock_acquire(&filesys_lock);
	file_seek(curr_file, position);
	lock_release(&filesys_lock);
}

static unsigned s_tell(int fd)
{
	s_check_fd(fd);
	struct file *curr_file = thread_current()->fd_table[fd];
	if (curr_file == NULL)
	{
		s_exit(-1);
	}
	else if (curr_file == STDIN || curr_file == STDOUT)
		return 0;
	lock_acquire(&filesys_lock);
	off_t next_byte = file_tell(curr_file);
	lock_release(&filesys_lock);
	return (unsigned)next_byte;
}

static void s_close(int fd)
{
	s_check_fd(fd);
	struct file *curr_file = thread_current()->fd_table[fd];
	if (curr_file == NULL)
	{
		s_exit(-1);
	}
	else if (curr_file != STDIN && curr_file != STDOUT)
	{
		decrease_ref_count(curr_file);
		if (check_ref_count(curr_file) == 0)
		{
			lock_acquire(&filesys_lock);
			file_close(curr_file);
			lock_release(&filesys_lock);
		}
	}
	thread_current()->fd_table[fd] = NULL;
	return;
}

static int s_dup2(int oldfd, int newfd)
{
	struct thread *t = thread_current();

	if (oldfd < 0 || oldfd >= t->fd_table_size || t->fd_table[oldfd] == NULL)
		return -1;
	if (newfd < 0)
		return -1;
	if (oldfd == newfd)
		return newfd;

	// newfd가 fd_table최대값보다 크면 계속 키움
	while (newfd >= t->fd_table_size)
	{
		if (realloc_fd_table(t) == -1)
			return -1;
	}

	// 이미 열려있으면 닫기
	if (t->fd_table[newfd] != NULL)
		s_close(newfd);

	// 포인터 복사
	struct file *f = t->fd_table[oldfd];
	t->fd_table[newfd] = f;

	// ref_count 올리기
	if (f != STDIN && f != STDOUT)
		increase_ref_count(f);

	return newfd;
}

static void s_check_access(const char *file)
{
	if (file == NULL || !is_user_vaddr(file) || pml4_get_page(thread_current()->pml4, file) == NULL)
	{
		s_exit(-1);
	}
}

static void s_check_buffer(const void *buffer, unsigned length)
{
	if (buffer == NULL)
		s_exit(-1);
	const uint8_t *start = (const uint8_t *)buffer;
	const uint8_t *end = start + length - 1;
	s_check_access(start);
	if (length > 0)
		s_check_access(end);

	for (const uint8_t *p = pg_round_down(start) + PGSIZE; p <= pg_round_down(end); p += PGSIZE)
	{
		s_check_access(p);
	}
}

static void s_check_fd(int fd)
{
	struct thread *t = thread_current();
	if (fd < 0 || fd >= t->fd_table_size)
	{
		s_exit(-1);
	}
}

static int realloc_fd_table(struct thread *t)
{
	int new_size = t->fd_table_size * 2;
	struct file **new_table = malloc(sizeof(struct file *) * new_size);
	if (new_table == NULL)
	{
		return -1;
	}
	memset(new_table, 0, sizeof(struct file *) * new_size);
	memcpy(new_table, t->fd_table, sizeof(struct file *) * t->fd_table_size);
	free(t->fd_table);
	t->fd_table = new_table;
	t->fd_table_size = new_size;
	return 1;
}
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
static void s_exit(int status) NO_RETURN;
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
static int is_my_child(struct thread *parent, struct thread *child);
void free_child_resources(struct thread *child_thread);
enum fd_type
{
	READ = 0,
	WRITE = 1
};
static void s_check_fd(int fd, enum fd_type type);
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
		// case SYS_DUP2:
		// 	break;
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

static void s_exit(int status)
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
	struct thread *cur = thread_current();
	struct thread *child = get_thread_by_tid(tid);
	int exit_code = -1;

	if (child == NULL || cur->waited)
	{
		return -1;
	}
	sema_down(&child->wait_sema);
	cur->waited = true;
	exit_code = child->exit_status;

	return exit_code;
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

	for (int i = 2; i < 128; i++)
	{
		if (t->fd_table[i] == NULL)
		{
			t->fd_table[i] = target_file;
			fd = i;
			break;
		}
	}
	if (fd == -1)
	{
		/* FD 공간이 없음 → file 닫고 실패 반환 */
		lock_acquire(&filesys_lock);
		file_close(target_file);
		lock_release(&filesys_lock);
		return -1;
	}
	return fd;
}

static int s_filesize(int fd)
{
	s_check_fd(fd, READ);
	struct file *f = thread_current()->fd_table[fd];
	if (f == NULL)
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
	s_check_fd(fd, READ);
	int bytes_read = 0;

	// 2. stdin (fd == 0)
	if (fd == 0)
	{
		for (unsigned i = 0; i < length; i++)
			((uint8_t *)buffer)[i] = input_getc();
		return length;
	}

	// 3. 파일 디스크립터에서 파일 찾기
	struct file *f = thread_current()->fd_table[fd];
	if (f == NULL)
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
	s_check_fd(fd, WRITE);

	// 콘솔 출력
	if (fd == 1)
	{
		putbuf(buffer, length);
		return length;
	}

	// 파일에 write 하기
	struct file *curr_file = thread_current()->fd_table[fd];
	// 파일을 못 가져오면
	if (curr_file == NULL)
	{
		return -1;
	}

	// write 하기전에 lock
	lock_acquire(&filesys_lock);
	int written = file_write(curr_file, buffer, length); // file.h
	lock_release(&filesys_lock);

	return written;
}

static void s_seek(int fd, unsigned position)
{
	// 	다음 읽기/쓰기 위치를 `position`으로 변경. 파일 끝을 넘어가도 오류 아님.

	// > 다만 Project 4 이전에는 파일 길이가 고정이므로, 실제로는 오류가 발생할 수 있음.
}

static unsigned s_tell(int fd)
{
	// 현재 fd에서 다음 읽기/쓰기가 이루어질 위치(바이트 단위)를 반환.
}

static void s_close(int fd)
{
	// 파일 디스크립터 fd를 닫습니다. 프로세스가 종료되면 모든 fd는 자동으로 닫힙니다.
}

static int s_dup2(int oldfd, int newfd)
{
	// - `dup2()`는 `oldfd`를 복제하여 **지정된 번호인 `newfd`로 새로운 파일 디스크립터**를 생성합니다.
	// - 성공 시 `newfd`를 반환합니다.
	// ### 동작 규칙:
	// - `oldfd`가 **유효하지 않으면**, 실패하며 `1`을 반환하고, `newfd`는 **닫히지 않습니다**.
	// - `oldfd`와 `newfd`가 **같으면**, 아무 동작도 하지 않고 `newfd`를 반환합니다.
	// - `newfd`가 **이미 열려 있는 경우**, **조용히 닫은 후에** `oldfd`를 복제합니다.
	// - 복제된 디스크립터는 **파일 오프셋과 상태 플래그를 공유**합니다.
	// 	- 예: `seek()`으로 하나의 파일 위치를 바꾸면, 다른 디스크립터도 같은 위치를 가리킵니다.
	// - **`fork()` 이후에도 dup된 fd의 의미는 유지되어야 합니다.**
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

static void s_check_fd(int fd, enum fd_type type)
{
	if (fd < 0 || fd >= 128)
	{
		s_exit(-1);
	}
	else if ((type == READ && fd == 1) || (type == WRITE && fd == 0))
	{
		s_exit(-1);
	}
}

void free_child_resources(struct thread *child_thread)
{
	list_remove(&child_thread->child_elem);
	palloc_free_page(child_thread);
}

extern struct list all_list;

/* Lock for all_list synchronization, if necessary. */
// extern struct lock all_list_lock;

/**
 * @brief 스레드 ID(tid)를 사용하여 해당 스레드 구조체를 찾습니다.
 * * @param tid 찾고자 하는 스레드의 ID.
 * @return struct thread* 스레드를 찾으면 해당 스레드 구조체의 포인터를,
 * 찾지 못하면 NULL을 반환합니다.
 */

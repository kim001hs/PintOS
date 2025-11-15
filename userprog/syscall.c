#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

static void s_halt(void) NO_RETURN;
static void s_exit(int status) NO_RETURN;
static int s_fork(const char *thread_name);
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
		f->R.rax = s_fork(f->R.rdi);
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

static int s_fork(const char *thread_name)
{
	// 	현재 프로세스를 복제하여 새 자식 프로세스를 생성합니다. 이름은 `thread_name`을 따릅니다. 다음 사항을 만족해야 합니다:

	// - 복제 시 **callee-saved 레지스터**들만 복사하면 됩니다: `%rbx`, `%rsp`, `%rbp`, `%r12`~`%r15`
	// - 반환값은 자식 프로세스에서 0, 부모 프로세스에서는 자식의 pid
	// - **파일 디스크립터 및 가상 메모리 공간 등 리소스를 복제**해야 합니다
	// - 부모는 자식이 자원을 성공적으로 복제했는지 확인 전까지 `fork()`에서 반환되면 안 됩니다
	// - 복제에 실패한 경우, 부모는 `TID_ERROR`를 반환

	// `threads/mmu.c`에 있는 `pml4_for_each()`를 사용해 가상 주소 공간 전체를 복사할 수 있습니다. `pte_for_each_func`에 해당하는 함수를 채워야 합니다.
	// return process_fork(thread_name, thread_current()->tf); // 맞는지 몰?루
}

static int s_exec(const char *file)
{
	char *fn_copy = palloc_get_page(0);
	if (fn_copy == NULL)
		return -1;
	strlcpy(fn_copy, file, PGSIZE);

	int res = process_exec(fn_copy);
	// palloc_free_page(fn_copy); free는 process_exec안에서
	return res;
}

static int s_wait(int tid)
{
	// 	자식 프로세스 `pid`의 종료를 기다리고 종료 코드를 반환합니다:

	// - 종료되지 않았다면 종료될 때까지 대기
	// - `exit()`로 종료했다면 해당 status 반환
	// - 커널에 의해 종료되었으면 `1` 반환

	// 다음 조건 중 하나라도 만족하면 즉시 `-1`을 반환해야 합니다:

	// - `pid`는 현재 프로세스의 **직계 자식**이 아님
	// - 이미 `wait(pid)`가 호출된 적이 있음

	// 부모 프로세스는 자식을 어떤 순서로든 기다릴 수 있고, 기다리지 않고 먼저 종료될 수도 있습니다. **자식 프로세스는 부모가 기다리든 말든 자원을 반드시 정리해야 합니다**.

	// **Pintos 전체 종료는 최초 프로세스가 종료되어야만 발생해야 합니다.** 이를 위해 기본적으로 `main()` 함수에서 `process_wait()`가 호출됩니다. `wait()` 시스템 콜은 이를 활용하여 구현해야 합니다.

	// > wait()는 이 프로젝트에서 가장 구현이 복잡한 시스템 콜입니다.
	// >
	// process_wait(tid);
	return process_wait(tid);
}

static bool s_create(const char *file, unsigned initial_size)
{
	// 	크기 `initial_size`의 새 파일을 생성합니다. 성공 시 `true`, 실패 시 `false` 반환.

	// > 생성은 열기와 다릅니다. 열려면 open() 호출 필요.
	// process_create_initd(file);
}

static bool s_remove(const char *file)
{
	// 파일을 삭제합니다. 열려 있든 닫혀 있든 상관없이 삭제 가능. 성공 시 true.
}

static int s_open(const char *file)
{
	// 	파일을 열고 **파일 디스크립터(fd)**를 반환합니다. 실패 시 `-1`.

	// - **fd 0**: 표준 입력 (STDIN)
	// - **fd 1**: 표준 출력 (STDOUT)
	// - 사용자 프로세스는 **0, 1 외의 디스크립터**만 받을 수 있음
	// - 파일을 여러 번 열면 각각 다른 fd 반환
	// - **fd는 자식에게 상속되며**, fd는 독립적으로 닫힘
}

static int s_filesize(int fd)
{
	// 열려 있는 파일의 크기를 바이트 단위로 반환합니다.
}

static int s_read(int fd, void *buffer, unsigned length)
{
	// fd에서 buffer로 최대 size 바이트 읽음.
	// 반환값은 실제로 읽은 바이트 수 (EOF이면 0, 실패 시 -1). fd 0이면 키보드에서 입력.
}

static int s_write(int fd, const void *buffer, unsigned length)
{
	// test
	if (buffer == NULL)
		return;
	if (fd == 1)
	{
		putbuf(buffer, length);
		return length;
	}
	// 1 아닐 때 구현 필요
	return -1;
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
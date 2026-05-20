#include "filesys/file.h"
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/init.h" /* power_off() */
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "threads/vaddr.h" /* is_user_vaddr / KERN_BASE 사용 */
#include "intrinsic.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/input.h"	/* input_getc() — SYS_READ stdin 분기에서 사용 */
#include "threads/palloc.h" /* SYS_EXEC 에서 palloc_get_page() 인자로 넣을 떄 PAL_ZERO 필요*/
#include "vm/vm.h"

// 주소가 writable한지 확인하는 헬퍼 함수
static void is_writable_user_addr(const void *uaddr);

/* 파일 시스템 락 선언 */
struct lock filesys_lock;

void syscall_entry(void);
void syscall_handler(struct intr_frame *);
struct fd_entry
{
	int fd;
	struct file *file;
	struct list_elem elem;
};

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
#define FD_MAX 128

void syscall_init(void)
{

	lock_init(&filesys_lock);
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
							((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			  FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	lock_init(&filesys_lock);
}

/* stage 0 최소 메모리 검증.
 *   - NULL 즉시 거절
 *   - is_user_vaddr(p)는 ((uint64_t)p < KERN_BASE)와 동치 →
 *     커널 영역 포인터를 유저가 넘긴 경우 차단
 * 언맵된 페이지/페이지 경계 걸침 등 정식 검증은 이후 단계에서 추가. */
static void validate_user_addr(const void *uaddr)
{
	if (uaddr == NULL || !is_user_vaddr(uaddr))
	{
		thread_current()->exit_status = -1;
		thread_exit();
	}
#ifndef VM
	if (pml4_get_page(thread_current()->pml4, uaddr) == NULL)
	{
		thread_current()->exit_status = -1;
		thread_exit();
	}
#endif
}

static void is_writable_user_addr(const void *uaddr)
{
	struct page *page = spt_find_page(&thread_current()->spt, uaddr);
	// CASE 1 : 페이지가 존재함
	if (page != NULL)
	{
		// 쓰기 불가능이면 종료
		if (page->writable == false)
		{
			thread_current()->exit_status = -1;
			thread_exit();
		}
	}
	// CASE 2 : 페이지가 존재하지 않음
	else
	{
		uintptr_t user_rsp = thread_current()->user_rsp;
		void *stack_limit = (void *)(USER_STACK - (1 << 20));

		// 만약 스택 growth가 불가능한 영역이면 종료
		if ((uaddr >= (void *)(user_rsp - 8) && uaddr >= stack_limit) == false)
		{
			thread_current()->exit_status = -1;
			thread_exit();
		}
	}
}

/* x86-64 syscall ABI (KAIST 기준)
 *   rax       = syscall 번호 / 반환값
 *   rdi, rsi, rdx, r10, r8, r9 = 인자 1~6
 * intr_frame->R 의 동명 필드에 그대로 들어 있다. */
void syscall_handler(struct intr_frame *f UNUSED)
{
	// 현재 스레드의 유저 스택 포인터를, thread 구조체의 user_rsp에 저장
	thread_current()->user_rsp = f->rsp;
	uint64_t sysno = f->R.rax;

	switch (sysno)
	{
	/* 파일 관련 */
	case SYS_CREATE:
	{

		const char *filename = (const void *)f->R.rdi;
		unsigned size = (unsigned)f->R.rsi;

		/* 유저 메모리 검증 */
		validate_user_addr(filename);

		/* 파일 시스템 전용 lock */
		lock_acquire(&filesys_lock);

		/* 2. filesys_create 호출 */
		bool success = filesys_create(filename, size);

		lock_release(&filesys_lock);

		/* 3. 반환값 세팅 */
		f->R.rax = success;

		break;
	}

	case SYS_OPEN:
	{
		/*
			1. filename 검증 (validate_user_addr)
			2. filesys_open() 호출
			3. 반환값이 NULL이면 → f->R.rax = -1
			4. 성공이면 → fd_table[fd_next] = file
			5. f->R.rax = fd_next
			6. fd_next 증가
		*/

		const char *filename = (const void *)f->R.rdi;

		/* 유저 메모리 검증 */
		validate_user_addr(filename);
		lock_acquire(&filesys_lock);

		struct file *file = filesys_open(filename);

		if (file == NULL)
		{
			f->R.rax = -1;
			lock_release(&filesys_lock);
			break;
		}
		else
		{
			thread_current()->fd_table[thread_current()->fd_next] = file;
			f->R.rax = thread_current()->fd_next;
			thread_current()->fd_next++;
		}

		lock_release(&filesys_lock);

		break;
	}

	case SYS_CLOSE:
	{
		/*
			1. file_close(file) 호출
			2. fd_table[fd] = NULL
		*/

		uint64_t fd = (uint64_t)f->R.rdi;

		lock_acquire(&filesys_lock);

		/* int 범위 체크 */
		if (fd < 2 || fd >= 128)
		{
			lock_release(&filesys_lock);
			break;
		}
		else
		{
			struct file *file = thread_current()->fd_table[fd];
			if (file == NULL)
			{
				lock_release(&filesys_lock);
				break;
			}
			file_close(file);
			thread_current()->fd_table[fd] = NULL;
		}
		lock_release(&filesys_lock);

		break;
	}

	case SYS_READ:
	{
		int fd = (int)f->R.rdi;
		const void *buffer = (const void *)f->R.rsi;
		unsigned size = (unsigned)f->R.rdx;

		// 1. 유효한 주소인지 검사
		// 2. write 가능한지 검사
		validate_user_addr(buffer);
		is_writable_user_addr(buffer);

		lock_acquire(&filesys_lock);

		if (fd < 0 || fd >= 128)
		{
			/* 범위 밖 fd는 stdin/stdout 분기보다 먼저 차단해야
			 * fd_table[음수] 같은 잘못된 인덱싱이 막힌다. */
			f->R.rax = -1;
			lock_release(&filesys_lock);
			break;
		}
		else if (fd == 0)
		{
			/* fd=0 (stdin) */
			for (int i = 0; i < size; i++)
			{
				((char *)buffer)[i] = input_getc(); // buffer에 저장
			}
			f->R.rax = size;
			lock_release(&filesys_lock);
			break;
		}
		else if (fd == 1)
		{
			f->R.rax = -1;
			lock_release(&filesys_lock);
			break;
		}
		else if (fd >= 2)
		{
			/* fd_table에서 파일 찾아서 file_read() 호출 */
			struct file *file = thread_current()->fd_table[fd];
			if (file == NULL)
			{
				lock_release(&filesys_lock);
				break;
			}
			f->R.rax = file_read(file, buffer, size);
			lock_release(&filesys_lock);
		}
		break;
	}

	case SYS_WRITE:
	{
		int fd = (int)f->R.rdi;
		const void *buffer = (const void *)f->R.rsi;
		unsigned size = (unsigned)f->R.rdx;

		/* stage 0: KERN_BASE 체크만 (요구사항대로 최소화) */
		validate_user_addr(buffer);

		lock_acquire(&filesys_lock);

		if (fd < 0 || fd >= 128)
		{
			f->R.rax = -1;
			lock_release(&filesys_lock);
			break;
		}
		else if (fd == 0)
		{
			f->R.rax = -1;
			lock_release(&filesys_lock);
			break;
		}
		else if (fd == 1)
		{
			/* fd=1 (stdout): putbuf로 콘솔 출력.
			 *   - putbuf는 내부 console lock으로 한 호출분을 보호하여
			 *     다른 출력과 섞이지 않는다.
			 *   - 유저 모드 printf()의 최종 종착지가 바로 이 분기.
			 *     이게 없으면 유저 프로그램의 모든 출력이 사라진다. */
			putbuf(buffer, size);
			f->R.rax = size; /* 쓴 바이트 수 반환 (stdout은 size 그대로) */
			lock_release(&filesys_lock);
		}
		else if (fd >= 2)
		{
			struct file *file = thread_current()->fd_table[fd];
			if (file == NULL)
			{
				lock_release(&filesys_lock);
				break;
			}
			f->R.rax = file_write(file, buffer, size);
			lock_release(&filesys_lock);
		}
		break;
	}

	case SYS_FILESIZE:
	{
		int fd = (int)f->R.rdi;

		lock_acquire(&filesys_lock);

		/* fd_table[fd] 직전 범위 가드 — 음수/오버플로 fd로 인한 OOB 차단 */
		if (fd < 2 || fd >= 128)
		{
			f->R.rax = -1;
			lock_release(&filesys_lock);
			break;
		}

		struct file *file = thread_current()->fd_table[fd];
		if (file == NULL)
		{
			f->R.rax = -1;
			lock_release(&filesys_lock);
			break;
		}

		f->R.rax = file_length(file);
		lock_release(&filesys_lock);
		break;
	}

	/* 프로세스 관련 */
	case SYS_EXEC:
	{
		/* 유저가 넘긴 파일명 포인터 검증 */
		const char *filename = (const void *)f->R.rdi;
		validate_user_addr(filename);

		/* process_exec는 palloc으로 할당된 메모리를 기대하므로
		 * 유저 스택의 filename을 그대로 넘기면 안 됨
		 * 새 페이지에 복사해서 넘김 */
		char *fn_copy = palloc_get_page(PAL_ZERO);
		strlcpy(fn_copy, filename, PGSIZE);

		/* 성공하면 돌아오지 않음 (do_iret으로 유저 모드 전환)
		 * 실패하면 -1 반환 */
		int result = process_exec(fn_copy);
		thread_current()->exit_status = -1;
		thread_exit();
		NOT_REACHED();
	}

	case SYS_FORK:
	{
		const char *name = (const void *)f->R.rdi; /* 자식 이름 */
		tid_t tid = process_fork(name, f);		   /* f가 곧 if_ */
		sema_down(&thread_current()->fork_sema);
		f->R.rax = tid;

		break;
	}

	case SYS_WAIT:
	{
		tid_t pid = (tid_t)f->R.rdi;
		f->R.rax = process_wait(pid);
		break;
	}

	case SYS_HALT:
		/* 머신을 즉시 종료한다.
		 * power_off()는 QEMU/Bochs에 shutdown 신호를 보내며 NO_RETURN이다. */
		power_off();
		NOT_REACHED();

	case SYS_EXIT:
	{
		/* rdi에 담긴 종료 코드를 thread 구조체에 저장한 뒤 종료한다.
		 * exit_status는 process_wait()가 회수할 때 쓰이며,
		 * process_exit()의 종료 메시지 출력에도 사용된다.
		 * thread_exit()가 process_exit()를 호출하므로 별도 호출 불필요. */
		int status = (int)f->R.rdi;
		thread_current()->exit_status = status;
		thread_exit();
		NOT_REACHED();
	}

	case SYS_MMAP:
	{
		void *addr = (void *)f->R.rdi;
		size_t length = f->R.rsi;
		int writable = (int)f->R.rdx;
		int fd = (int)f->R.rcx;
		off_t offset = (off_t)f->R.r8;

		// fd 검사
		if (fd < 2 || fd >= 128)
		{
			f->R.rax = NULL;
			break;
		}

		// 현재 스레드의 fd_table에서 fd가 가리키고 있는 file이 있는지 검사
		struct file *file = thread_current()->fd_table[fd];
		if (file == NULL)
		{
			f->R.rax = NULL;
			break;
		}

		f->R.rax = (uint64_t)do_mmap(addr, length, writable, file, offset);
		break;
	}

	default:
		/* 아직 라우팅 안 된 시스템 콜.
		 * 디버깅 가시성을 위해 한 줄 찍고 종료. */
		printf("[stage0] unhandled syscall: %llu\n",
			   (unsigned long long)sysno);
		thread_exit();
	}
}

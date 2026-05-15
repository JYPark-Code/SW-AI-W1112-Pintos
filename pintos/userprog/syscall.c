#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/init.h"       /* power_off() */
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "threads/vaddr.h"      /* is_user_vaddr / KERN_BASE 사용 */
#include "intrinsic.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/input.h"      /* input_getc() — SYS_READ stdin 분기에서 사용 */
#include "threads/palloc.h" 	/* SYS_EXEC 에서 palloc_get_page() 인자로 넣을 떄 PAL_ZERO 필요*/
#include "threads/mmu.h"        /* pml4_get_page — 유저 포인터 매핑 검증에 사용 */
#include "vm/vm.h"

/* 파일 시스템 락 선언 */
struct lock filesys_lock;

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {

	lock_init(&filesys_lock);
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* 유저가 넘긴 포인터의 안전성을 검증한다.
 * 인자: uaddr — 유저 주소 공간을 가리켜야 하는 포인터
 * 반환: 정상이면 그냥 리턴, 비정상이면 exit(-1)로 즉시 종료 (반환 X)
 *
 * 공통 단계:
 *   1) NULL: 가장 흔한 잘못된 포인터. dereference 전에 차단.
 *   2) !is_user_vaddr: 유저가 KERN_BASE 이상의 커널 주소를 넘긴 경우 차단.
 *      빼먹으면 syscall로 커널 메모리를 읽거나 쓸 수 있다.
 *
 * 매핑 검사 단계는 빌드 모드에 따라 다르다:
 *   - userprog (no VM): pml4_get_page == NULL이면 차단. 매핑이 없으면
 *     dereference 시 커널 page fault → kill() 패닉이 나기 때문.
 *   - VM build: pml4에 없어도 SPT에 lazy 엔트리가 있거나, 스택 성장
 *     후보일 수 있다. 그래서 pml4 단독 검사로는 정상 포인터를 잘못 차단한다
 *     (예: pt-grow-stk-sc가 64KB 스택 버퍼를 syscall로 넘기는 케이스).
 *     SPT 보유 또는 스택 성장 영역(rsp-8 이상, 1MB 한도) 안이면 통과시킨다.
 *
 * 함정: thread_exit()는 NO_RETURN이므로 호출자에서 추가 처리 불필요. */
static void
validate_user_addr (const void *uaddr) {
    if (uaddr == NULL || !is_user_vaddr(uaddr)) {
        thread_current()->exit_status = -1;
        thread_exit();
    }
#ifndef VM
    if (pml4_get_page(thread_current()->pml4, uaddr) == NULL) {
        thread_current()->exit_status = -1;
        thread_exit();
    }
#else
    if (pml4_get_page(thread_current()->pml4, uaddr) != NULL)
        return;
    if (spt_find_page(&thread_current()->spt, (void *) uaddr) != NULL)
        return;
    /* 스택 성장 후보: USER_STACK - 1MB ≤ addr < USER_STACK, 그리고
     * 유저 rsp - 8 이상 (PUSH 명령어 8바이트 아래 영역까지 정상). */
    uintptr_t rsp = thread_current()->user_rsp;
    if ((uintptr_t) uaddr >= rsp - 8
        && (uintptr_t) uaddr < (uintptr_t) USER_STACK
        && (uintptr_t) uaddr >= (uintptr_t) USER_STACK - (1 << 20))
        return;
    thread_current()->exit_status = -1;
    thread_exit();
#endif
}

/* x86-64 syscall ABI (KAIST 기준)
 *   rax       = syscall 번호 / 반환값
 *   rdi, rsi, rdx, r10, r8, r9 = 인자 1~6
 * intr_frame->R 의 동명 필드에 그대로 들어 있다. */
void
syscall_handler (struct intr_frame *f UNUSED) {
	uint64_t sysno = f->R.rax;
	thread_current()->tf.rsp = f->rsp;
	thread_current()->user_rsp = f->rsp;

	switch (sysno) {
		/* 파일 관련 */
		/* SYS_CREATE: 새 파일을 생성한다.
		 * 인자: rdi=파일 이름(유저 포인터), rsi=초기 크기
		 * 반환: 성공 1, 실패 0
		 * 주의: filesys 모듈은 내부 동기화가 없어 커널 lock이 필수.
		 *       NULL/커널 포인터/언맵 포인터 모두 validate_user_addr가
		 *       단일 진입점에서 처리하므로 별도 분기 불필요. */
		case SYS_CREATE: {

			const char  *filename = (const void *) f->R.rdi;
			unsigned       size   = (unsigned) f->R.rsi;

			/* 유저가 넘긴 파일명 포인터 검증 — bad-ptr이면 여기서 exit(-1) */
			validate_user_addr(filename);

			/* filesys는 race-free하지 않으므로 커널 전역 lock으로 직렬화 */
			lock_acquire(&filesys_lock);

			bool success = filesys_create(filename, size);

			lock_release(&filesys_lock);

			f->R.rax = success;

			break;
		}

		/* SYS_OPEN: 파일을 열어 fd를 발급한다.
		 * 인자: rdi=파일 이름(유저 포인터)
		 * 반환: 성공 시 fd(>=2), 실패 시 -1
		 * 주의:
		 *   - filesys_open() 반환값이 NULL이면 "파일 없음/열기 실패"이므로
		 *     fd 발급 없이 -1 반환. open-missing 테스트가 이 경로를 검증.
		 *   - fd 는 [2, 128) 범위의 가장 작은 비어 있는 슬롯을 사용한다.
		 *     단조 증가(fd_next++) 방식은 close 후 슬롯 재사용을 못 해서
		 *     126번째 open 부근에 fd_table[128] OOB write 가 발생 → 커널 fault.
		 *     multi-oom 이 fd 126개 open 사이클을 반복해서 그 경로를 검증.
		 *   - file == NULL 체크는 반드시 lock 안에서 해야 한다.
		 *     lock 밖에서 NULL 체크 후 진입 직전 다른 스레드의 close가 끼면
		 *     filesys 상태가 한 번 더 흔들릴 수 있어 race 위험. */
		case SYS_OPEN: {

			const char  *filename = (const void *) f->R.rdi;

			validate_user_addr(filename);
			lock_acquire(&filesys_lock);

			struct file *file = filesys_open(filename);

			if (file == NULL) {
				/* 파일 없음/디스크 에러 — fd 미할당 후 -1 */
				f->R.rax = -1;
				lock_release(&filesys_lock);
				break;
			}

			/* 가장 작은 비어 있는 fd 슬롯 검색 ([2, 128)) */
			struct thread *t = thread_current();
			int fd = -1;
			for (int i = 2; i < 128; i++) {
				if (t->fd_table[i] == NULL) {
					fd = i;
					break;
				}
			}
			if (fd == -1) {
				/* 전부 사용 중 — 열린 file 을 다시 닫고 -1 반환.
				 * 이 처리를 빼면 file struct leak. */
				file_close(file);
				f->R.rax = -1;
				lock_release(&filesys_lock);
				break;
			}
			t->fd_table[fd] = file;
			f->R.rax = fd;

			lock_release(&filesys_lock);

			break;
		}

		/* SYS_CLOSE: fd가 가리키는 파일을 닫는다.
		 * 인자: rdi=fd
		 * 반환: 없음 (잘못된 fd는 조용히 무시 — 표준 동작)
		 * 주의:
		 *   - fd 유효 범위는 [2, 128). 0/1은 콘솔이라 닫을 수 없고,
		 *     128 이상은 fd_table OOB.
		 *   - file_close 후 fd_table[fd] = NULL로 슬롯을 비워야
		 *     이후 같은 fd에 대한 read/write가 NULL 체크로 막힌다.
		 *     이 라인이 빠지면 dangling 포인터를 참조한다(use-after-free).
		 *   - 모든 분기에서 lock_release를 잊지 말 것 — early break 시
		 *     해제 누락하면 다음 syscall이 영원히 블록된다. */
		case SYS_CLOSE: {

			uint64_t fd = (uint64_t) f->R.rdi;

			lock_acquire(&filesys_lock);

			/* 음수(uint64_t로 캐스팅돼 거대한 값) 또는 128 이상은 OOB */
			if (fd < 2 || fd >= 128) {
				lock_release(&filesys_lock);
				break;
			} else {
				struct file *file = thread_current()->fd_table[fd];
				if (file == NULL) {
					/* 이미 close된 fd — 조용히 종료 (lock 해제 필수) */
					lock_release(&filesys_lock);
					break;
				}
				file_close(file);
				thread_current()->fd_table[fd] = NULL;  /* dangling 방지 */
			}
			lock_release(&filesys_lock);

			break;
		}

		/* SYS_READ: fd로부터 size 바이트를 buffer로 읽는다.
		 * 인자: rdi=fd, rsi=buffer(유저 포인터), rdx=size
		 * 반환: 읽은 바이트 수, 에러 시 -1
		 * 주의:
		 *   - fd 범위 체크가 stdin/stdout 분기보다 먼저 와야 한다.
		 *     음수 fd가 fd==0/1 분기를 우회해 fd_table[음수]로 빠지면
		 *     커널 메모리를 인덱싱해 panic 발생.
		 *   - fd=0(stdin)은 input_getc()로 한 글자씩 키보드 폴링 (블로킹).
		 *   - fd=1(stdout)은 read 불가 — write-only이므로 -1.
		 *   - fd>=2는 fd_table에서 file*을 꺼내 file_read 호출. */
		case SYS_READ: {
			int            fd     = (int) f->R.rdi;
			const void    *buffer = (const void *) f->R.rsi;
			unsigned       size   = (unsigned) f->R.rdx;

			/* stage 0: KERN_BASE 체크만 (요구사항대로 최소화) */
			validate_user_addr(buffer);

			lock_acquire(&filesys_lock);

			if (fd < 0 || fd >= 128){
				/* 범위 밖 fd는 stdin/stdout 분기보다 먼저 차단해야
				 * fd_table[음수] 같은 잘못된 인덱싱이 막힌다. */
				f->R.rax = -1;
				lock_release(&filesys_lock);
				break;
			} else if (fd == 0) {
				/* fd=0 (stdin): input_getc()는 한 글자만 반환하므로
				 * size만큼 반복해야 요청 길이를 채울 수 있다. */
				for(int i = 0; i < size; i++) {
					((char *)buffer)[i] = input_getc(); // buffer에 저장
				}
				f->R.rax = size;
				lock_release(&filesys_lock);
				break;
			} else if(fd == 1){
				/* stdout은 입력 채널이 아님 → 표준에 따라 에러 */
				f->R.rax = -1;
				lock_release(&filesys_lock);
				break;
			} else if(fd >= 2) {
				/* 일반 파일 읽기 */
				struct file *file = thread_current()->fd_table[fd];
				if (file == NULL) {
					lock_release(&filesys_lock);
					break;
				}
#ifdef VM
				// writable 체크 (VM 빌드에서만 의미가 있음 — 지연 로딩/RO 페이지 판별)
				struct page *p = spt_find_page(&thread_current()->spt, pg_round_down((void *)buffer));
				if (p == NULL) {
					// 유저 스택 범위인지 확인
					uintptr_t cur_rsp = thread_current()->user_rsp;
					if ((uintptr_t)buffer < cur_rsp - 8) {
						lock_release(&filesys_lock);
						thread_current()->exit_status = -1;
						thread_exit();
					}
				// 스택 범위 안 → 통과 (page fault로 처리될 것)
				} else if (!p->writable) {
					lock_release(&filesys_lock);
					thread_current()->exit_status = -1;
					thread_exit();
					}
#endif
				f->R.rax = file_read(file, buffer, size);
				lock_release(&filesys_lock);
			}
			break;
		}

		/* SYS_WRITE: fd로 size 바이트를 buffer에서 출력한다.
		 * 인자: rdi=fd, rsi=buffer(유저 포인터), rdx=size
		 * 반환: 쓴 바이트 수, 에러 시 -1
		 * 주의:
		 *   - stage 0에서는 fd=1(stdout) 한 가지만 지원했고,
		 *     이후 일반 파일(fd>=2) 분기를 추가했다.
		 *     처음부터 모든 분기를 만들면 디버깅이 어려워서 점진 확장.
		 *   - fd=1은 putbuf()로 출력 — 유저 printf의 최종 종착지.
		 *     이 분기가 없으면 유저 프로그램의 모든 출력이 사라진다.
		 *   - fd=0(stdin)에는 쓸 수 없음 → -1. */
		case SYS_WRITE: {
			int            fd     = (int) f->R.rdi;
			const void    *buffer = (const void *) f->R.rsi;
			unsigned       size   = (unsigned) f->R.rdx;

			/* stage 0: KERN_BASE 체크만 (요구사항대로 최소화) */
			validate_user_addr(buffer);

			lock_acquire(&filesys_lock);

			if (fd < 0 || fd >= 128){
				f->R.rax = -1;
				lock_release(&filesys_lock);
				break;
			} else if(fd == 0){
				/* stdin은 출력 채널이 아님 */
				f->R.rax = -1;
				lock_release(&filesys_lock);
				break;
			}
			else if (fd == 1) {
				/* fd=1 (stdout): putbuf로 콘솔 출력.
				 *   - putbuf는 내부 console lock으로 한 호출분을 보호하여
				 *     다른 출력과 섞이지 않는다.
				 *   - 유저 모드 printf()의 최종 종착지가 바로 이 분기.
				 *     이게 없으면 유저 프로그램의 모든 출력이 사라진다. */
				putbuf(buffer, size);
				f->R.rax = size;     /* 쓴 바이트 수 반환 (stdout은 size 그대로) */
				lock_release(&filesys_lock);
			} else if (fd >= 2) {
				/* 일반 파일 쓰기 */
				struct file *file = thread_current()->fd_table[fd];
				if (file == NULL) {
					lock_release(&filesys_lock);
					break;
				}
				f->R.rax = file_write(file, buffer, size);
				lock_release(&filesys_lock);
			}
			break;
		}

		/* SYS_FILESIZE: fd가 가리키는 파일의 크기를 반환한다.
		 * 인자: rdi=fd
		 * 반환: 파일 크기(바이트), 에러 시 -1
		 * 주의:
		 *   - read-normal 테스트가 read 전에 filesize를 호출해 버퍼 크기를
		 *     잡으므로 read보다 먼저 구현되어야 했다.
		 *   - 일부 테스트는 check_file() 헬퍼 안에서 filesize를 호출해
		 *     파일 무결성을 검사하므로 이 syscall이 빠지면 read 자체가 깨진다.
		 *   - file_length는 stdin/stdout(fd 0/1)에 의미가 없으므로 fd>=2만 허용. */
		case SYS_FILESIZE : {
			int            fd     = (int) f->R.rdi;

			lock_acquire(&filesys_lock);

			/* fd_table[fd] 직전 범위 가드 — 음수/오버플로 fd로 인한 OOB 차단 */
			if (fd < 2 || fd >= 128) {
				f->R.rax = -1;
				lock_release(&filesys_lock);
				break;
			}

			struct file *file = thread_current()->fd_table[fd];
			if (file == NULL) {
				f->R.rax = -1;
				lock_release(&filesys_lock);
				break;
			}

			f->R.rax = file_length(file);
			lock_release(&filesys_lock);
			break;
		}

		/* 프로세스 관련 */
		/* SYS_EXEC: 현재 프로세스의 이미지를 새 실행 파일로 교체한다.
		 * 인자: rdi=실행할 파일 이름 + 인자(공백 구분, 유저 포인터)
		 * 반환: 성공 시 절대 돌아오지 않는다 (do_iret으로 유저 모드 진입).
		 *       실패 시 현재 프로세스를 exit(-1)로 종료.
		 * 주의:
		 *   - process_exec()는 내부에서 palloc_free_page(file_name)을 호출하므로
		 *     반드시 palloc으로 할당된 메모리를 넘겨야 한다. 유저 스택의 포인터를
		 *     그대로 넘기면 free 시 panic.
		 *   - PAL_ZERO로 페이지를 0초기화한 뒤 strlcpy로 PGSIZE 한도 안에서 복사.
		 *   - exec()는 "현재 프로세스를 교체"하는 의미이므로 실패 시 호출자에게
		 *     -1을 반환하는 것이 아니라, 현재 프로세스를 -1로 종료시켜야 한다.
		 *     (테스트 exec-missing이 이 동작을 직접 검증) */
		case SYS_EXEC: {
			/* 유저가 넘긴 파일명 포인터 검증 */
			const char *filename = (const void *) f->R.rdi;
			validate_user_addr(filename);

			/* process_exec는 palloc 페이지를 free하므로 유저 포인터를
			 * 그대로 넘기지 않고 새 페이지에 복사해서 전달한다. */
			char *fn_copy = palloc_get_page(PAL_ZERO);
			strlcpy(fn_copy, filename, PGSIZE);

			/* 성공 시 do_iret이 새 유저 컨텍스트로 점프 → 여기로 안 돌아옴.
			 * 실패 시 result=-1로 빠져나오므로 현재 프로세스를 -1로 종료.
			 * (exec 의미상 "원래 프로세스로 복귀해서 -1 반환"은 잘못된 동작) */
			int result = process_exec(fn_copy);
			thread_current()->exit_status = -1;
			thread_exit();
			NOT_REACHED();
		}

		/* SYS_FORK: 현재 프로세스를 복제해 자식을 생성한다.
		 * 인자: rdi=자식 스레드 이름(유저 포인터, 예: fork("child")의 "child")
		 * 반환: 부모에게는 자식 tid, 자식에게는 0 (자식은 __do_fork에서 R.rax=0 세팅)
		 * 주의:
		 *   - rdi를 (const void*)로 받는 이유: 유저 fork("child") 호출의 "child"
		 *     문자열 포인터가 ABI상 rdi 레지스터로 들어온다.
		 *   - fork_sema로 부모를 블록하는 이유: 자식이 메모리/fd 복제를 끝내기 전에
		 *     부모가 먼저 진행하면 race가 발생한다 (예: 부모가 fd를 close하면
		 *     아직 file_duplicate 안 끝난 자식이 dangling 포인터 복제).
		 *     자식이 __do_fork 끝에서 sema_up(fork_sema)로 깨운다. */
		case SYS_FORK: {
			const char *name = (const void *) f->R.rdi;  /* 자식 이름 */

			/* 이전 fork 의 값이 남아 있을 수 있으니 명시적으로 reset.
			 * 자식이 __do_fork 안에서 성공/실패에 맞춰 다시 세팅한다. */
			thread_current()->fork_success = false;

			tid_t tid = process_fork(name, f);          /* f가 곧 if_ */
			if (tid == TID_ERROR) {
				/* thread_create 자체가 실패 — sema_up 한 적이 없으므로
				 * sema_down 으로 들어가면 영원히 블록. 그냥 -1 반환. */
				f->R.rax = -1;
				break;
			}
			sema_down(&thread_current()->fork_sema);   /* 자식 복제 완료 대기 */

			/* __do_fork 내부 (pml4_create 실패 / 페이지 복제 실패 / fd 복제 실패 등)
			 * 에서 깨졌으면 fork_success = false. 이 검사가 없으면 부모는 valid
			 * 해 보이는 tid 를 받고 wait 에서 default exit_status=0 을 회수,
			 * multi-oom 의 "crashed child should return -1" 가 fail. */
			f->R.rax = thread_current()->fork_success ? tid : -1;

			break;
		}

		case SYS_SEEK: {
			int fd = (int) f->R.rdi;
			off_t position = (off_t) f->R.rsi;
			
			lock_acquire(&filesys_lock);
			if (fd >= 2 && fd < 128) {
				struct file *file = thread_current()->fd_table[fd];
				if (file != NULL)
					file_seek(file, position);
			}
			lock_release(&filesys_lock);
			break;
		}

		case SYS_TELL: {
			int fd = (int) f->R.rdi;
			lock_acquire(&filesys_lock);
			if (fd >= 2 && fd < 128) {
				struct file *file = thread_current()->fd_table[fd];
				if (file != NULL)
					f->R.rax = file_tell(file);
				else
					f->R.rax = -1;
			} else {
				f->R.rax = -1;
			}
			lock_release(&filesys_lock);
			break;
		}

		/* SYS_WAIT: 자식 tid가 종료할 때까지 대기 후 exit_status 회수.
		 * 인자: rdi=자식 tid
		 * 반환: 자식의 exit_status, 잘못된 tid이거나 이미 wait한 경우 -1
		 * 주의: 실제 동기화는 process_wait()의 sema_down(wait_sema) 흐름이 담당. */
		case SYS_WAIT: {
			tid_t pid = (tid_t) f->R.rdi;
			f->R.rax = process_wait(pid);
			break;
		}

		case SYS_HALT:
			/* 머신을 즉시 종료한다.
			 * power_off()는 QEMU/Bochs에 shutdown 신호를 보내며 NO_RETURN이다. */
			power_off ();
			NOT_REACHED ();

		case SYS_EXIT: {
			/* rdi에 담긴 종료 코드를 thread 구조체에 저장한 뒤 종료한다.
			 * exit_status는 process_wait()가 회수할 때 쓰이며,
			 * process_exit()의 종료 메시지 출력에도 사용된다.
			 * thread_exit()가 process_exit()를 호출하므로 별도 호출 불필요. */
			int status = (int) f->R.rdi;
			// printf("SYS_EXIT called: status=%d\n", status);
			thread_current ()->exit_status = status;
			thread_exit ();
			NOT_REACHED ();
		}

		default:
			/* 아직 라우팅 안 된 시스템 콜.
			 * 디버깅 가시성을 위해 한 줄 찍고 종료. */
			printf("[stage0] unhandled syscall: %llu\n",
			       (unsigned long long) sysno);
			thread_exit();
	}
}

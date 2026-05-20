#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "threads/malloc.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);




#define ARGV_MAX 64     /* 인자 개수 상한 (args-many 테스트 기준 22개로 충분) */

/* __do_fork()에서는 parent 스레드도 필요하고 parent_if도 필요해서 구조체를 묶기 */
struct fork_args {
    struct thread *parent;
    struct intr_frame *parent_if;
};

/* 유저 스택에 argv 문자열과 포인터 배열을 세팅한다.
 *
 * 스택 레이아웃 (낮은 주소 방향으로 성장):
 *   [argv[0] 문자열] … [argv[argc-1] 문자열]
 *   [8-byte 패딩 (word-align)]
 *   [NULL sentinel  (8 bytes)]
 *   [argv[argc-1] 포인터] … [argv[0] 포인터]  ← rsi 여기 (argv[] 배열 시작)
 *   [fake return address = 0  (8 bytes)]        ← rsp 여기
 *
 * rdi = argc, rsi = argv[] 배열의 시작 주소를 intr_frame에 직접 기록한다.
 * rsi를 (rsp+8)로 계산하지 않고 명시적으로 저장하는 이유:
 *   do_iret 이후 %rsi 레지스터가 곧바로 main(argc, argv)로 전달되므로
 *   스택 상의 정확한 포인터 위치를 가리켜야 한다. */
static void
argument_stack (char **argv, int argc, struct intr_frame *_if) {
	uintptr_t arg_addr[ARGV_MAX];
	uint8_t *rsp = (uint8_t *) _if->rsp;
	int i;

	/* 1. 문자열 데이터를 스택에 복사 (argv[argc-1] → argv[0] 순서로 push).
	 *    역순으로 push하면 argv[0]가 가장 낮은 주소에 위치해
	 *    디버깅 시 hex-dump가 읽기 쉽다. */
	for (i = argc - 1; i >= 0; i--) {
		size_t len = strlen (argv[i]) + 1;  /* null terminator 포함 */
		rsp -= len;
		memcpy (rsp, argv[i], len);
		arg_addr[i] = (uintptr_t) rsp;     /* 나중에 포인터로 재사용 */
	}

	/* 2. word-align: ABI는 rsp가 16-byte 정렬된 상태로 call을 요구한다.
	 *    포인터 push 전에 8-byte 단위로 맞춰두면 NULL sentinel + 짝수 개
	 *    포인터가 늘 16-byte 정렬을 만족시킨다. */
	rsp = (uint8_t *) ((uintptr_t) rsp & ~(uintptr_t) 7);

	/* 3. NULL sentinel: argv[argc] = 0 (C 표준 요구사항) */
	rsp -= sizeof (uintptr_t);
	*(uintptr_t *) rsp = 0;

	/* 4. argv 포인터 배열을 역순으로 push (argv[argc-1] → argv[0]).
	 *    push 후 rsp == &argv[0] on stack. */
	for (i = argc - 1; i >= 0; i--) {
		rsp -= sizeof (uintptr_t);
		*(uintptr_t *) rsp = arg_addr[i];
	}

	/* rsi = argv[] 배열 시작 주소를 지금 기록한다.
	 * fake return address push 이후에는 rsp가 한 칸 더 내려가므로
	 * (rsp+8)과 달리 여기서 캡처해야 argv[0] 포인터를 정확히 가리킨다. */
	_if->R.rsi = (uint64_t) rsp;
	_if->R.rdi = (uint64_t) argc;

	/* 5. fake return address: main()의 반환 주소 자리를 0으로 채운다.
	 *    실제로 main이 return하면 SYS_EXIT를 거치므로 값은 의미 없다. */
	rsp -= sizeof (uintptr_t);
	*(uintptr_t *) rsp = 0;

	/* 6. 최종 rsp 반영 */
	_if->rsp = (uintptr_t) rsp;
}

/* initd와 다른 프로세스를 위한 공통 프로세스 초기화 함수. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* FILE_NAME에서 첫 userland 프로그램인 "initd"를 로드해 시작한다.
 * 새 스레드는 process_create_initd()가 반환되기 전에 스케줄될 수 있고,
 * 심지어 종료될 수도 있다. initd의 thread id를 반환하며, 스레드를 만들 수
 * 없으면 TID_ERROR를 반환한다. 이 함수는 반드시 한 번만 호출되어야 한다. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* FILE_NAME의 복사본을 만든다.
	 * 그렇지 않으면 호출자와 load() 사이에 race가 생긴다. */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* FILE_NAME을 실행할 새 스레드를 만든다. */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* 첫 유저 프로세스를 시작하는 스레드 함수. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();
	thread_current()->parent = NULL;  /* initd는 부모가 없음 */
	if (process_exec(f_name) < 0)
    	PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* 현재 프로세스를 `name`이라는 이름으로 복제한다. 새 프로세스의 thread id를
 * 반환하며, 스레드를 만들 수 없으면 TID_ERROR를 반환한다. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {

	/* __do_fork를 위해서 새로 구조체를 넘기는 부분 */
	struct fork_args *args = malloc(sizeof(struct fork_args));
	args->parent = thread_current();
	args->parent_if = if_;

	/* 현재 스레드를 새 스레드로 복제한다. */
	return thread_create (name,
			PRI_DEFAULT, __do_fork, args);
}

#ifndef VM
/* 이 함수를 pml4_for_each에 넘겨 부모의 주소 공간을 복제한다.
 * 이 코드는 project 2에서만 사용된다. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: parent_page가 kernel page라면 즉시 반환한다. */
	if (is_kernel_vaddr(va))
    	return true;

	/* 2. 부모의 page map level 4에서 VA를 해석한다. */
	parent_page = pml4_get_page (parent->pml4, va);

	/* 3. TODO: 자식을 위한 새 PAL_USER page를 할당하고 결과를 NEWPAGE에
	 *    TODO: 저장한다. */
	newpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (newpage == NULL)
		return false;

	/* 4. TODO: 부모의 page 내용을 새 page로 복사하고, 부모 page가 writable인지
	 *    TODO: 확인한다. 그 결과에 따라 WRITABLE을 설정한다. */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);


	/* 5. 자식의 page table에 주소 VA로 새 page를 추가하고 WRITABLE 권한을
	 *    적용한다. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: page 삽입에 실패하면 오류 처리를 수행한다. */
		if (!pml4_set_page(current->pml4, va, newpage, writable)) {
			palloc_free_page(newpage);
			return false;
		}

	}
	return true;
}
#endif

/* 부모의 실행 context를 복사하는 스레드 함수.
 * 힌트) parent->tf는 프로세스의 userland context를 보관하지 않는다.
 *       따라서 process_fork의 두 번째 인자를 이 함수로 넘겨야 한다. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *current = thread_current ();
	/* TODO: parent_if를 어떤 방식으로든 전달한다. 즉 process_fork()의 if_. */
	/* aux 캐스팅 */
	struct fork_args *args = (struct fork_args *) aux;
	struct thread *parent = args->parent;
	struct intr_frame *parent_if = args->parent_if;

	bool succ = true;

	/* 1. CPU context를 local stack으로 읽어온다. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 자식을 부모의 children 리스트에 추가하는 코드 (thread_create에서 이미 등록되었으므로 중복 호출 금지) */
	current->parent = parent;

	/* arg 메모리 해제 */
	free(args);


	/* 2. page table을 복제한다. */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: 여기에 코드를 작성하세요.
	 * TODO: 힌트) file 객체를 복제하려면 include/filesys/file.h의
	 * TODO:       `file_duplicate`를 사용한다. 이 함수가 부모의 자원을
	 * TODO:       성공적으로 복제하기 전까지 부모는 fork()에서 반환되면 안 된다. */

	/* 부모의 fd_table을 자식에게 복사 */
	for (int i = 2; i < 128; i++) {
		if (parent->fd_table[i] != NULL) {
			current->fd_table[i] = file_duplicate(parent->fd_table[i]);
			
		}
	}

	current->fd_next = parent->fd_next;
	sema_up(&parent->fork_sema);
	if_.R.rax = 0;
	
	process_init ();

	/* 마지막으로 새로 생성된 프로세스로 전환한다. */
	if (succ)
		do_iret (&if_);
error:
	sema_up(&parent->fork_sema);
	thread_exit ();
	if_.R.rax = 0;
}

/* 현재 실행 context를 f_name으로 전환한다.
 * 실패하면 -1을 반환한다. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* strtok_r 용 변수: file_name 페이지가 살아있는 동안 파싱 완료해야 한다. */
	char *argv[ARGV_MAX];
	int   argc = 0;
	char *token, *save_ptr;

	/* thread 구조체 안의 intr_frame은 사용할 수 없다.
	 * 현재 스레드가 다시 스케줄될 때 실행 정보가 그 멤버에 저장되기 때문이다. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* 인자 파싱: strtok_r은 file_name을 in-place 수정하므로
	 * palloc_free_page 이전에 수행한다.
	 * argv[0] = 프로그램 이름, argv[1..] = 인자들. */
	for (token = strtok_r (file_name, " ", &save_ptr);
	     token != NULL && argc < ARGV_MAX;
	     token = strtok_r (NULL, " ", &save_ptr))
		argv[argc++] = token;

	/* 스레드 이름을 프로그램 이름으로 갱신.
	 * thread_create에는 cmdline 전체가 들어가 있어 16자 한계로
	 * "args-single one"처럼 잘리고, process_exit의 종료 메시지가 깨진다. */

	strlcpy (thread_current ()->name, argv[0],
	         sizeof thread_current ()->name);

	/* load()에는 프로그램 이름(argv[0])만 넘긴다.
	 * 나머지 인자는 argument_stack()에서 유저 스택에 직접 쓴다. */
	success = load (argv[0], &_if);


	if (!success) {
		palloc_free_page (file_name);
		return -1;
	}

	/* argument_stack()이 argv[i] 포인터(file_name 페이지 내부)를 읽으므로
	 * 스택 세팅이 완전히 끝난 뒤에 해제한다. */
	argument_stack (argv, argc, &_if);
	palloc_free_page (file_name);

	do_iret (&_if);
	NOT_REACHED ();
}


/* thread TID가 종료될 때까지 기다리고 exit status를 반환한다. kernel에 의해
 * 종료되었다면, 즉 exception 때문에 kill되었다면 -1을 반환한다. TID가
 * 유효하지 않거나 호출 프로세스의 자식이 아니거나, 해당 TID에 대해
 * process_wait()가 이미 성공적으로 호출된 적이 있다면 기다리지 않고 즉시
 * -1을 반환한다.
 *
 * 이 함수는 problem 2-2에서 구현된다. 지금은 아무 일도 하지 않는다. */
/* 자식 프로세스 종료 대기 후 exit_status를 회수해 반환한다.
 *
 * 동기화 흐름 (자식 thread t가 종료될 때):
 *   1) 부모는 children list에서 child_tid를 찾는다 (없으면 -1).
 *   2) sema_down(&t->wait_sema): 자식이 process_exit에서 sema_up할 때까지 블록.
 *   3) 깨어난 시점에 자식은 자신의 exit_sema에서 BLOCKED 상태이므로
 *      struct thread 본체와 t->exit_status가 아직 살아있다 → 안전하게 회수.
 *   4) list_remove로 좀비 엔트리 제거.
 *   5) sema_up(&t->exit_sema): 자식이 마지막 do_schedule(DYING)으로 진입.
 * 
 * 동일 child_tid에 대해 두 번째 호출되면 list_remove 이후 검색 실패 → -1.
 * 자식이 아닌 tid나 잘못된 tid도 검색 실패 → -1.
 */
int
process_wait (tid_t child_tid) {
	struct thread *cur = thread_current ();
	struct thread *child = NULL;
	struct list_elem *e;
	int exit_status;

	for (e = list_begin (&cur->children); e != list_end (&cur->children);
	     e = list_next (e)) {
		struct thread *t = list_entry (e, struct thread, child_elem);
		if (t->tid == child_tid) {
			child = t;
			break;
		}
	}
	if (child == NULL)
		return -1;

	sema_down (&child->wait_sema);
	exit_status = child->exit_status;
	list_remove (&child->child_elem);
	child->wait_called = true;
	sema_up (&child->exit_sema);

	return exit_status;
}

/* 프로세스를 종료한다. 이 함수는 thread_exit()에서 호출된다.
 *
 * 동기화 흐름:
 *   1) 종료 메시지 출력 (유저 프로세스만; 커널 스레드는 pml4=NULL).
 *   2) process_cleanup으로 pml4 회수.
 *   3) sema_up(wait_sema): 부모의 process_wait를 깨운다.
 *   4) sema_down(exit_sema): 부모가 exit_status를 회수할 때까지 블록.
 *      이 동안 thread 구조체가 살아있으므로 부모가 안전하게 읽을 수 있다.
 *      부모가 sema_up하면 thread_exit로 복귀해 do_schedule(DYING) 진입.
 */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	bool is_user_process = (curr->pml4 != NULL);

	if (is_user_process)
		printf ("%s: exit(%d)\n", curr->name, curr->exit_status);

	process_cleanup ();

	if (is_user_process) {

		/* 부모가 wait 안 한 자식들을 풀어줌 */
		struct list_elem *e;
		for (e = list_begin(&curr->children);
			e != list_end(&curr->children);
			e = list_next(e)) {
			struct thread *child = list_entry(e, struct thread, child_elem);
			sema_up(&child->exit_sema);
		}

		sema_up (&curr->wait_sema);

		// if (curr->parent != NULL){
		sema_down (&curr->exit_sema); /* 무조건 대기 */
		// }
	}
}

/* 현재 프로세스의 자원을 해제한다. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* 현재 프로세스의 page directory를 파괴하고 kernel-only page directory로
	 * 되돌아간다. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* 여기서는 순서가 매우 중요하다. page directory를 전환하기 전에
		 * cur->pagedir를 NULL로 설정해야 timer interrupt가 프로세스의
		 * page directory로 다시 전환하지 못한다. 또한 프로세스의 page
		 * directory를 파괴하기 전에 base page directory를 활성화해야 한다.
		 * 그렇지 않으면 현재 활성 page directory가 이미 해제되고 지워진
		 * page directory가 될 수 있다. */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* 다음 스레드에서 user code를 실행할 수 있도록 CPU를 설정한다.
 * 이 함수는 context switch마다 호출된다. */
void
process_activate (struct thread *next) {
	/* 스레드의 page table을 활성화한다. */
	pml4_activate (next->pml4);

	/* interrupt 처리에 사용할 스레드의 kernel stack을 설정한다. */
	tss_update (next);
}

/* 우리는 ELF binary를 로드한다. 아래 정의들은 ELF 명세 [ELF1]에서 거의
 * 그대로 가져온 것이다. */

/* ELF 타입. [ELF1] 1-2를 참고한다. */
#define EI_NIDENT 16

#define PT_NULL    0            /* 무시한다. */
#define PT_LOAD    1            /* 로드 가능한 segment. */
#define PT_DYNAMIC 2            /* 동적 linking 정보. */
#define PT_INTERP  3            /* 동적 loader 이름. */
#define PT_NOTE    4            /* 보조 정보. */
#define PT_SHLIB   5            /* 예약됨. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* 실행 가능. */
#define PF_W 2          /* 쓰기 가능. */
#define PF_R 4          /* 읽기 가능. */

/* 실행 파일 header. [ELF1] 1-4부터 1-8까지를 참고한다.
 * ELF binary의 가장 앞에 위치한다. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* 약어 */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* FILE_NAME의 ELF 실행 파일을 현재 스레드로 로드한다.
 * 실행 파일의 entry point를 *RIP에 저장하고 초기 stack pointer를 *RSP에
 * 저장한다. 성공하면 true, 실패하면 false를 반환한다. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* 기존 pml4 백업 */
	uint64_t *old_pml4 = t->pml4;  

	/* page directory를 할당하고 활성화한다. */
	uint64_t *new_pml4 = pml4_create();
	if (new_pml4 == NULL)
		goto done;

	t->pml4 = new_pml4;  /* 임시로 교체 */
	process_activate (thread_current ());

	/* 실행 파일을 연다. */
	file = filesys_open (file_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* 실행 파일 header를 읽고 검증한다. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* program header들을 읽는다. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* 이 segment는 무시한다. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* 일반 segment.
						 * 앞부분은 disk에서 읽고 나머지는 0으로 채운다. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* 전체가 0이다.
						 * disk에서 아무것도 읽지 않는다. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}
	
	/* stack을 설정한다. */
	if (!setup_stack (if_))
		goto done;

	/* 시작 주소. */
	if_->rip = ehdr.e_entry;   

	/* TODO: 여기에 코드를 작성하세요.
	 * TODO: argument passing을 구현한다. project2/argument_passing.html 참고. */

	success = true;

done:
	/* load 성공 여부와 관계없이 여기로 도달한다. */
	file_close(file);
    if (!success) {
        if (t->pml4 != old_pml4) {
            pml4_destroy(t->pml4);
        }
        t->pml4 = old_pml4;       /* 원래 pml4로 복원 */
        pml4_activate(old_pml4);  /* 원래 pml4 재활성화 */
    }
    return success;
}


/* PHDR이 FILE 안의 유효하고 로드 가능한 segment를 설명하는지 확인한다.
 * 그렇다면 true, 아니면 false를 반환한다. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset과 p_vaddr는 같은 page offset을 가져야 한다. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset은 FILE 안을 가리켜야 한다. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz는 적어도 p_filesz만큼 커야 한다. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* segment는 비어 있으면 안 된다. */
	if (phdr->p_memsz == 0)
		return false;

	/* 가상 메모리 영역의 시작과 끝은 모두 user address space 범위 안에
	   있어야 한다. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* 이 영역은 kernel virtual address space를 넘어 "wrap around"되면 안 된다. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* page 0 mapping을 금지한다.
	   page 0을 mapping하는 것은 좋지 않을 뿐 아니라, 허용할 경우 user code가
	   system call에 null pointer를 넘겼을 때 memcpy() 등의 null pointer
	   assertion을 통해 kernel panic을 일으킬 가능성이 높다. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* 유효하다. */
	return true;
}

#ifndef VM
/* 이 블록의 코드는 project 2 동안에만 사용된다.
 * project 2 전체에서 사용할 함수를 구현하려면 #ifndef macro 바깥에 구현한다. */

/* load() 보조 함수. */
static bool install_page (void *upage, void *kpage, bool writable);

/* FILE의 offset OFS에서 시작하는 segment를 주소 UPAGE에 로드한다.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리를 다음과 같이 초기화한다.
 *
 * - UPAGE부터 READ_BYTES 바이트는 FILE의 offset OFS부터 읽어야 한다.
 *
 * - UPAGE + READ_BYTES부터 ZERO_BYTES 바이트는 0으로 채워야 한다.
 *
 * WRITABLE이 true라면 이 함수로 초기화된 page는 user process가 쓸 수 있어야
 * 하며, 그렇지 않다면 read-only여야 한다.
 *
 * 성공하면 true를 반환하고, 메모리 할당 오류나 disk 읽기 오류가 발생하면
 * false를 반환한다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 page를 어떻게 채울지 계산한다.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고, 마지막 PAGE_ZERO_BYTES
		 * 바이트는 0으로 채운다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* 메모리 page 하나를 얻는다. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* 이 page를 로드한다. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* 이 page를 프로세스의 주소 공간에 추가한다. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* 다음 page로 진행한다. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* USER_STACK 위치에 0으로 채운 page를 mapping해서 최소 stack을 만든다. */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* user virtual address UPAGE에서 kernel virtual address KPAGE로 가는 mapping을
 * page table에 추가한다.
 * WRITABLE이 true이면 user process가 page를 수정할 수 있고, 그렇지 않으면
 * read-only다.
 * UPAGE는 아직 mapping되어 있으면 안 된다.
 * KPAGE는 palloc_get_page()로 user pool에서 얻은 page여야 한다.
 * 성공하면 true를 반환하고, UPAGE가 이미 mapping되어 있거나 메모리 할당에
 * 실패하면 false를 반환한다. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* 해당 가상주소에 아직 page가 없는지 확인한 뒤, 그 위치에 page를 mapping한다. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* 여기부터의 코드는 project 3 이후에 사용된다.
 * project 2 전용 함수를 구현하려면 위쪽 블록에 구현한다. */

 static void __do_fork (void *);
struct lazy_load_aux {   //lazy_load_segment()가 frame을 채울 수 있도록 이 정보를 aux로 넘긴다
	struct file *file; //실행 파일 객체
	off_t ofs; //page가 파일에서 읽기 시작할 offset
	size_t page_read_bytes; //이 page에 파일에서 읽을 byte 수
	size_t page_zero_bytes; //파일에서 읽은 뒤 나머지를 0으로 채울 byte 수
};

static bool
lazy_load_segment (struct page *page, void *aux) { 
	void *kva;
	struct lazy_load_aux * segment_aux = aux;
	kva = page->frame->kva;
	file_seek(segment_aux->file, segment_aux->ofs);  //파일을 읽거나쓸 시작 위치를 바꾸기
	off_t read_bytes = file_read(segment_aux->file, kva, segment_aux->page_read_bytes);
	if(read_bytes != (off_t)segment_aux->page_read_bytes){
		file_close(segment_aux->file);
		free(segment_aux);
		return false;
	}
	memset((uint8_t *)kva + segment_aux->page_read_bytes, 0, segment_aux->page_zero_bytes);
	file_close(segment_aux->file);
    free(segment_aux);
	
	return true;
	/* TODO: 파일에서 segment를 로드한다. */
	/* TODO: VA 주소에서 첫 page fault가 발생했을 때 호출된다. */
	/* TODO: 이 함수를 호출할 때 VA를 사용할 수 있다. */
}

/* FILE의 offset OFS에서 시작하는 segment를 주소 UPAGE에 로드한다.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리를 다음과 같이 초기화한다.
 *
 * - UPAGE부터 READ_BYTES 바이트는 FILE의 offset OFS부터 읽어야 한다.
 *
 * - UPAGE + READ_BYTES부터 ZERO_BYTES 바이트는 0으로 채워야 한다.
 *
 * WRITABLE이 true라면 이 함수로 초기화된 page는 user process가 쓸 수 있어야
 * 하며, 그렇지 않다면 read-only여야 한다.
 *
 * 성공하면 true를 반환하고, 메모리 할당 오류나 disk 읽기 오류가 발생하면
 * false를 반환한다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 page를 어떻게 채울지 계산한다.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고, 마지막 PAGE_ZERO_BYTES
		 * 바이트는 0으로 채운다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		struct lazy_load_aux *segment_aux;
		segment_aux  = malloc(sizeof(struct lazy_load_aux));
		if(segment_aux == NULL){
			return false;
		}

		segment_aux->file = file_reopen(file);
		if(segment_aux->file == NULL){
			free(segment_aux);
			return false;
		}
		segment_aux->ofs = ofs;
		segment_aux->page_read_bytes = page_read_bytes;
		segment_aux->page_zero_bytes = page_zero_bytes; 
		
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, segment_aux)){ //이 page가 나중에 fault될 때 lazy_load_segment가 사용할 정보
			free(segment_aux);
			return false;
		}
		/* 다음 page로 진행한다. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += page_read_bytes; //다음 파일 위치로 이동
	}
	return true;
}

/* USER_STACK에 stack PAGE를 만든다. 성공하면 true를 반환한다. */
static bool
setup_stack (struct intr_frame *if_) {
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);
	bool alloc_success;
	bool claim_success;
	alloc_success = vm_alloc_page(VM_ANON, stack_bottom, true);
	if(alloc_success){
		claim_success = vm_claim_page(stack_bottom);
	}
	else{
		return false;
	}
	if(claim_success){
		if_->rsp = USER_STACK;
		return true;
	}
	else{
		return false;
	}
}
#endif /* VM */

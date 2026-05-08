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
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

#define ARGV_MAX 64     /* 인자 개수 상한 (args-many 테스트 기준 22개로 충분) */

/* __do_fork()로 부모 정보를 전달하기 위한 묶음 구조체.
 * thread_create()의 aux는 void* 한 개만 받을 수 있는데,
 * fork에서는 (1) 부모 스레드 포인터(children 리스트 등록용)와
 * (2) 부모의 intr_frame(레지스터 컨텍스트 복사용) 두 가지가 모두 필요하다.
 * 따라서 두 포인터를 한 구조체로 묶어 aux에 단일 포인터로 전달한다. */
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

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
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

/* 현재 프로세스를 복제해 `name` 이름의 자식 스레드를 만든다.
 * 인자: name=자식 이름, if_=부모의 인터럽트 프레임(syscall 시점 레지스터)
 * 반환: 자식 tid, 실패 시 TID_ERROR
 *
 * 주의:
 *   - fork_args를 malloc(힙)으로 할당하는 이유: process_fork는 호출 직후 리턴해
 *     자신의 스택 프레임이 사라진다. 만약 args를 스택(지역 변수)에 두면
 *     자식 스레드가 __do_fork에서 그 메모리를 읽기 시도할 때 이미 invalid.
 *     힙에 두면 자식이 free할 때까지 안전하게 살아있다.
 *   - thread_create의 aux는 단일 void*이므로 parent와 parent_if를 묶기 위해
 *     fork_args 구조체가 필요하다. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {

	/* 자식 스레드 시작 시점에도 살아있어야 하므로 힙에 할당 */
	struct fork_args *args = malloc(sizeof(struct fork_args));
	args->parent = thread_current();
	args->parent_if = if_;

	/* 자식 스레드 생성 — 시작 함수는 __do_fork, 인자는 args.
	 * 자식이 __do_fork 안에서 args를 free하므로 여기서는 free하지 않는다. */
	return thread_create (name,
			PRI_DEFAULT, __do_fork, args);
}

#ifndef VM
/* pml4_for_each의 콜백: 부모의 한 페이지(va)를 자식의 페이지 테이블로 복제.
 * 인자: pte=부모의 페이지 테이블 엔트리, va=가상 주소, aux=부모 thread*
 * 반환: 성공 true, 실패 false (false면 pml4_for_each가 즉시 중단)
 *
 * 핵심 흐름:
 *   1) 커널 영역(va)이면 복제 불필요 → 즉시 true.
 *      커널 페이지는 모든 프로세스가 같은 매핑을 공유하므로 자식의 새 pml4를
 *      만들 때 자동으로 들어와 있다. 따로 복사하면 이중 매핑/권한 충돌 발생.
 *   2) PAL_USER | PAL_ZERO로 새 유저 페이지 할당 — USER 풀에서 잡고 0 초기화.
 *      0 초기화는 잔재 데이터 노출을 막기 위한 안전장치(어차피 memcpy로 덮음).
 *   3) memcpy로 부모 페이지 내용을 그대로 복사 — Project 2는 eager copy.
 *   4) is_writable(pte)로 부모 페이지의 W비트를 읽어 자식에도 동일 권한 부여.
 *      읽기 전용 코드 세그먼트가 자식에서 쓰기 가능해지면 보호가 깨진다. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. 커널 페이지는 자식의 pml4에 자동 매핑되므로 스킵 */
	if (is_kernel_vaddr(va))
    	return true;

	/* 2. 부모의 va에서 실제 커널 가상 주소(매핑된 프레임) 획득 */
	parent_page = pml4_get_page (parent->pml4, va);

	/* 3. 자식용 새 페이지 할당 (USER 풀, 0 초기화) */
	newpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (newpage == NULL)
		return false;

	/* 4. 부모 페이지 내용 복사 + W권한 그대로 승계 */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);


	/* 5. 자식의 pml4에 va → newpage 매핑 추가 */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. 매핑 실패 시 — 할당했던 페이지 반납 후 실패 통보 */
		if (!pml4_set_page(current->pml4, va, newpage, writable)) {
			palloc_free_page(newpage);
			return false;
		}

	}
	return true;
}
#endif

/* fork된 자식 스레드의 시작 함수: 부모의 실행 컨텍스트/메모리/fd를 복제한다.
 * 인자: aux = process_fork에서 malloc한 fork_args 포인터
 *
 * 함수 끝에서 do_iret으로 유저 모드 진입 → 부모의 syscall 직후 명령어로 점프.
 *
 * 동기화 핵심:
 *   - 부모는 SYS_FORK에서 sema_down(fork_sema)로 블록 중.
 *   - 자식은 메모리/fd 복제를 모두 끝낸 뒤 sema_up(fork_sema)로 부모를 깨운다.
 *     이 순서를 어기면 (예: pml4 복사 도중 깨우면) 부모가 먼저 진행해 race 발생.
 *
 * 함정:
 *   - free(args)는 memcpy로 parent_if 내용을 if_ 로컬에 복사한 직후에 해야 한다.
 *     너무 빨리 free하면 parent_if dangling, 너무 늦게 free하면 메모리 누수.
 *   - children 리스트 등록은 부모의 pml4 복사 전에 마쳐야 한다 — 그래야 부모가
 *     깨어나서 wait를 호출했을 때 자식을 찾을 수 있다.
 *   - if_.R.rax = 0은 do_iret 직전에 세팅 — 자식의 fork() 반환값 0을 만든다. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *current = thread_current ();
	/* aux 캐스팅 — process_fork에서 만든 묶음 구조체 풀기 */
	struct fork_args *args = (struct fork_args *) aux;
	struct thread *parent = args->parent;
	struct intr_frame *parent_if = args->parent_if;

	bool succ = true;

	/* 1. 부모 인터럽트 프레임을 자식의 로컬 if_로 복사.
	 * 이 복사 후에야 args 포인터를 free해도 안전. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 부모-자식 관계 등록.
	 *
	 * 주의: thread_create()(thread.c)가 이미 새 스레드를
	 *   t->parent = thread_current ();
	 *   list_push_back (&thread_current ()->children, &t->child_elem);
	 * 로 부모의 children 리스트에 등록한다. 따라서 여기서 다시
	 * list_push_back을 호출하면 같은 child_elem이 이중 등록되어
	 * 리스트 prev/next 포인터가 자기 자신을 가리키도록 깨진다.
	 *
	 * 그 결과 wait-twice 같은 케이스에서:
	 *   - 첫 번째 wait는 정상 동작 (sema_up→sema_down 한 사이클).
	 *   - list_remove도 elem.prev/next가 모두 elem 자신이라 무력화 →
	 *     리스트에서 child가 사라지지 않는다.
	 *   - 두 번째 wait는 child를 또 찾아 sema_down(wait_sema)로
	 *     영원히 블록 → TIMEOUT.
	 *
	 * thread_create의 등록만으로 충분하므로 list_push_back은 호출하지 않는다.
	 * parent 대입은 thread_create와 동일 값이라 redundant지만 명시성을 위해 유지. */
	current->parent = parent;

	/* parent_if 내용은 이미 if_에 복사됐고 parent 포인터도 캐싱됨 → free 안전 */
	free(args);


	/* 2. 자식 전용 페이지 테이블 생성 + 활성화 */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	/* pml4_for_each + duplicate_pte로 부모의 모든 페이지를 자식에 복사.
	 * 단순 pml4 포인터 공유가 아닌 deep copy여야 fork 의미를 만족한다. */
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* 부모의 fd_table을 자식에 복사.
	 * 주의: 단순 포인터 복사로 끝내면 부모/자식이 같은 file* 객체를 공유해
	 *       offset/ref count가 꼬인다. file_duplicate로 새 file 객체를
	 *       만들어 inode만 공유하고 file pos는 독립시켜야 한다. */
	for (int i = 2; i < 128; i++) {
		if (parent->fd_table[i] != NULL) {
			current->fd_table[i] = file_duplicate(parent->fd_table[i]);

		}
	}

	/* 부모의 다음 발급 fd 번호도 복사 — 자식이 새 파일을 열 때
	 * 부모와 같은 시점에서 이어서 발급하기 위함. */
	current->fd_next = parent->fd_next;

	/* 모든 복제(메모리 + fd_table + fd_next) 완료 후에 부모를 깨운다.
	 * 이 시점 이전에 sema_up하면 부모가 먼저 실행돼 race 발생. */
	sema_up(&parent->fork_sema);

	/* 자식의 fork() 반환값은 0 — do_iret 시 rax 레지스터로 들어간다. */
	if_.R.rax = 0;

	process_init ();

	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret (&if_);
error:
	/* 실패 시에도 부모를 깨워야 영원히 블록되지 않는다. */
	sema_up(&parent->fork_sema);
	thread_exit ();
	if_.R.rax = 0;
}

/* 현재 스레드의 유저 컨텍스트를 f_name이 가리키는 새 실행 파일로 교체한다.
 * 인자: f_name = palloc된 페이지 (cmdline 전체, 공백으로 인자 구분)
 * 반환: 실패 시 -1, 성공 시 do_iret으로 진입해 절대 돌아오지 않음
 *
 * 주의:
 *   - process_cleanup() 호출이 사라진 이유: 새로 추가된 load() 내부에서
 *     pml4_create + 임시 활성화로 새 주소 공간을 직접 만들고, 실패 시 원래
 *     pml4로 복원하는 책임을 진다. 따라서 여기서 미리 cleanup하면 exec 실패 시
 *     "원래 프로세스로 복귀할 수 없게" 되어 버린다.
 *   - load() 실패 시 process_cleanup 없이 -1만 반환 — 원래 유저 프로세스의
 *     주소 공간을 그대로 유지해서 SYS_EXEC 호출자가 계속 살아있도록. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* strtok_r 용 변수: file_name 페이지가 살아있는 동안 파싱 완료해야 한다. */
	char *argv[ARGV_MAX];
	int   argc = 0;
	char *token, *save_ptr;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* 인자 파싱: strtok_r은 file_name을 in-place로 수정(공백을 \0으로)하므로
	 * 반드시 palloc_free_page(file_name) 이전, 즉 페이지가 살아있는 동안
	 * 파싱과 argument_stack 호출을 모두 끝내야 한다.
	 * argv[0] = 프로그램 이름, argv[1..] = 인자들. */
	for (token = strtok_r (file_name, " ", &save_ptr);
	     token != NULL && argc < ARGV_MAX;
	     token = strtok_r (NULL, " ", &save_ptr))
		argv[argc++] = token;

	/* 스레드 이름 갱신 코드를 비활성화한 이유:
	 *   exec("child")는 "현재 프로세스의 이미지를 child로 교체"하는 것이지,
	 *   "현재 스레드 이름을 child로 바꾸는" 게 아니다. 부모 프로세스가 exec로
	 *   자식 이미지를 로드해도 process_exit의 종료 메시지에는 원래 프로세스
	 *   이름이 찍혀야 한다. 이 strlcpy를 살리면 exec-once 같은 테스트에서
	 *   종료 메시지의 프로세스 이름이 바뀌어 fail. */
	strlcpy (thread_current ()->name, argv[0],
	         sizeof thread_current ()->name);

	/* load()에는 프로그램 이름(argv[0])만 넘긴다.
	 * 나머지 인자는 argument_stack()에서 유저 스택에 직접 쓴다. */
	success = load (argv[0], &_if);


	if (!success) {
		/* load() 실패 — pml4 복원은 load() 내부 done: 레이블이 처리.
		 * 여기서는 cmdline 페이지만 반납하고 -1로 SYS_EXEC에 복귀.
		 * process_cleanup을 부르면 원래 프로세스 주소공간이 박살난다. */
		palloc_free_page (file_name);
		return -1;
	}

	/* argument_stack()이 argv[i] 포인터(file_name 페이지 내부)를 읽으므로
	 * 스택 세팅이 완전히 끝난 뒤에 해제한다. */
	argument_stack (argv, argc, &_if);
	palloc_free_page (file_name);

	// thread_current()->parent = NULL;
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
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

	/* children 리스트를 선형 탐색해 child_tid에 해당하는 자식 찾기.
	 * 이 리스트는 __do_fork에서 list_push_back으로 등록된 직계 자식들.
	 * 자식이 아니거나 이미 wait된(list_remove된) tid는 검색 실패 → -1. */
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

	/* 자식이 process_exit에서 sema_up(wait_sema)로 깨워줄 때까지 블록 */
	sema_down (&child->wait_sema);

	/* 자식은 exit_sema에서 BLOCKED 상태로 살아있으므로 안전하게 회수 가능 */
	exit_status = child->exit_status;

	/* 좀비 엔트리 제거 — 같은 tid로 두 번째 wait 시 NULL 반환 보장 */
	list_remove (&child->child_elem);

	/* "부모가 wait를 호출했다"는 플래그 — 자식이 exit_sema에서 깨어나도
	 * 안전한지 판단하는 기준 (process_exit 참고). */
	child->wait_called = true;

	/* 자식에게 "이제 정리해도 됨" 신호 — 자식은 do_schedule(DYING)으로 진입 */
	sema_up (&child->exit_sema);

	return exit_status;
}

/* Exit the process. This function is called by thread_exit ().
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

	/* pml4가 NULL이면 커널 스레드 — 종료 메시지/세마포어 처리 모두 스킵.
	 * 유저 프로세스만 wait/exit 동기화 흐름에 참여한다. */
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

		/* 1) 부모를 깨운다 — 부모가 process_wait에서 sema_down(wait_sema)
		 *    중이면 여기서 깨어나 exit_status를 회수. */
		sema_up (&curr->wait_sema);

		/* 2) 부모가 wait를 호출한 경우에만 exit_sema에서 블록.
		 *    조건이 두 개인 이유:
		 *    - parent != NULL: 고아 프로세스(부모가 먼저 죽음)는 대기할
		 *      대상이 없으므로 즉시 정리해야 한다. 안 그러면 영원히 BLOCKED.
		 *    - wait_called: 부모가 살아있어도 wait를 호출하지 않았다면
		 *      누구도 sema_up(exit_sema)를 호출해주지 않아 영원히 블록.
		 *    두 조건이 모두 참일 때만 부모의 exit_status 회수가 끝나길 기다린다. */
		// if (curr->parent != NULL){
		sema_down (&curr->exit_sema); /* 무조건 대기 */
		// }
	}
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
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

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* ELF 실행 파일을 현재 스레드의 주소 공간에 로드한다.
 * 인자: file_name=프로그램 경로, if_=유저 진입 시 사용할 인터럽트 프레임
 * 반환: 성공 true, 실패 false
 *
 * exec 안전성 핵심 (이 구조가 process_exec의 -1 반환을 가능하게 한다):
 *   - 진입 시 t->pml4(원래 주소공간)를 old_pml4에 백업.
 *   - new_pml4를 만들어 임시로 활성화한 뒤 ELF를 로드.
 *   - 어느 단계에서든 실패하면 done: 레이블에서 new_pml4를 파괴하고
 *     old_pml4를 복원한다 → exec 호출자의 원래 프로세스가 살아남는다.
 *
 * 함정:
 *   - new_pml4로 NULL 체크하는 이유: t->pml4로 체크하면 initd 첫 호출
 *     시점에는 old_pml4가 NULL이므로 정상 케이스에서도 즉시 실패한다.
 *     "방금 만들려고 시도한 페이지 테이블"이 NULL인지를 봐야 함. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* 기존 pml4 백업 — 실패 시 복원해 exec 실패 안전성 보장 */
	uint64_t *old_pml4 = t->pml4;

	/* 새 페이지 디렉토리 생성. NULL이면 즉시 done(원래 pml4 유지). */
	uint64_t *new_pml4 = pml4_create();
	if (new_pml4 == NULL)
		goto done;

	t->pml4 = new_pml4;  /* 임시로 교체 — 이후 ELF 세그먼트가 여기로 매핑됨 */
	process_activate (thread_current ());

	/* Open executable file. */
	file = filesys_open (file_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* Read and verify executable header. */
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

	/* Read program headers. */
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
				/* Ignore this segment. */
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
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
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

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	success = true;

done:
	/* 성공/실패 어느 경우든 도달.
	 * 실패 분기: new_pml4를 파괴하고 old_pml4를 복원해야 SYS_EXEC가 -1을
	 * 받고 원래 프로세스에서 계속 실행할 수 있다. */
	file_close(file);
    if (!success) {
        if (t->pml4 != old_pml4) {
            /* 새로 만들었던 페이지 테이블이 활성화돼 있으면 파괴 */
            pml4_destroy(t->pml4);
        }
        t->pml4 = old_pml4;       /* 원래 pml4로 복원 */
        pml4_activate(old_pml4);  /* 원래 pml4 재활성화 */
    }
    return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
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

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */

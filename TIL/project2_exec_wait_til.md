# Pintos Project 2 — exec-missing 7단계와 wait-twice 동기화 디버깅

> KAIST 64bit Pintos Project 2 — User Programs 단계 마무리 회고.
> SYS_EXEC 의 `exec-missing` 테스트가 통과하기까지 거쳤던 7번의 시행착오와,
> 모든 fork/exec/wait 테스트 중 마지막까지 남은 `wait-twice` TIMEOUT 의
> 진짜 원인을 자료구조 corruption 한 줄로 추적해 들어간 디버깅 기록.
>
> | 섹션 | 주제 | 무게중심 |
> |---|---|---|
> | §1 | EXEC 동작 모델 정리 | "exec 실패 = 현재 프로세스 교체 실패" 시맨틱 |
> | §2 | exec-missing 7단계 시행착오 | 함수 4곳이 한 시맨틱을 향해 정렬되어야 함 |
> | §3 | 최종 수정 4곳 | load / process_exec / SYS_EXEC / initd |
> | §4 | wait-twice TIMEOUT 디버깅 | 자료구조 invariant — list 중복 등록의 기하학적 영향 |
> | §5 | 시도했다가 버린 6가지 가설 | 증상이 같아도 원인이 다르면 모두 실패한다 |
> | §6 | 핵심 교훈 정리 | "한 번은 되고 두 번은 깨지는" 버그의 시그니처 |
>
> 이번 두 사건의 공통 교훈은 **증상이 같아 보여도 원인의 층위가 다르다**는
> 것이었다. exec-missing 은 함수 간 책임 경계가 어긋난 문제였고, wait-twice
> 는 자료구조 invariant 가 깨진 문제였다. 같은 "TIMEOUT" 으로 보였지만
> 고치는 도구가 완전히 달랐다.

---

## 1. EXEC 의 동작 모델

### 1.1 fork() + exec() 가 아니다

리눅스의 `fork()` + `execve()` 패턴에 익숙하면 처음 헷갈리는 부분이다.
Pintos 의 `exec()` 은 새 프로세스를 만드는 게 아니라 **현재 프로세스의
이미지를 다른 실행 파일로 교체**한다.

```
fork()  → 자식 프로세스 새로 만듦. 부모는 계속 실행.
exec()  → 현재 프로세스를 새 이미지로 교체. 성공 시 돌아오지 않음.
```

이 차이가 실패 처리 시맨틱을 결정한다.

### 1.2 exec 실패 시 무엇을 해야 하는가

exec 가 "프로세스 교체" 라면, 교체에 실패했을 때 두 가지 선택지가 있다:

**(a) 호출자에게 -1 반환**
- 의미: "교체 실패했으니 원래 프로세스에서 계속 실행해라"
- 가능하려면: load() 가 실패해도 원래 주소공간을 그대로 유지해야 함
- exec-missing 테스트의 두 번째 허용 출력이 이 케이스

**(b) 현재 프로세스를 -1로 종료**
- 의미: "교체 실패 = 현재 프로세스도 끝"
- 단순하고 안전. 좀비/dangling 가능성 0.
- exec-missing 테스트의 첫 번째 허용 출력이 이 케이스

KAIST Pintos 는 **두 케이스 모두 허용**한다. 우리가 택한 것은 **(b) 종료**.
이유는 (a) 가 load() 의 실패 분기에서 새 pml4 를 정확히 파괴하고 원래
pml4 를 복원해야 하는데, 이 복원 로직이 한 함수 안에서 끝나지 않고
load → process_exec → SYS_EXEC → initd 4 함수에 걸쳐 일관되어야 하기
때문이다 (§2 에서 그 어려움을 7단계로 정리).

### 1.3 exec-missing 테스트가 검증하는 것

```c
/* tests/userprog/exec-missing.c */
void
test_main (void)
{
  msg ("exec(\"no-such-file\"): %d", exec ("no-such-file"));
}
```

`test_main()` 은 `exec()` 의 반환값을 출력만 하고 정상 리턴한다. 그
래서 wrapper 인 `main()` 이 결국 `exit(0)` 을 호출한다.

그런데 테스트가 기대하는 출력은 둘 중 하나:

```
[허용 1]                          [허용 2]
(exec-missing) begin              (exec-missing) begin
load: no-such-file: open failed   (exec-missing) exec("no-such-file"): -1
exec-missing: exit(-1)            exec-missing: exit(-1)
```

**둘 다 `exit(-1)` 로 끝나야 한다**. 즉 테스트 프레임워크는 exec 실패가
프로세스 종료를 트리거한다고 가정한다. 우리 구현이 (a) 호출자 반환을
선택했다면 `exit(0)` 으로 끝나서 둘 다 fail.

처음에 (a) 로 시도해서 7단계나 헤맨 이유 — 시맨틱 결정을 미루고
"page fault 만 안 나면 OK" 라고 착각했기 때문.

---

## 2. exec-missing 7단계 시행착오

각 시도마다 증상과 원인을 한 줄로 정리. 7회 모두 다른 곳에서 실패했다는
점이 핵심.

### 시도 1: load 실패 시 단순히 `return -1`

```c
/* process_exec() */
success = load (argv[0], &_if);
if (!success) {
    palloc_free_page (file_name);
    return -1;            /* ← 단순 반환 */
}
```

**증상**: `Page fault at 0x400b86: not present error reading page in user context.`
첫 page fault 가 유저 코드 시작 주소(0x400b86 = 프로그램 entry 부근)에서 발생.

**원인**: process_exec() 진입부에서 이미 `process_cleanup()` 으로 원래
pml4 를 destroy 해버린 상태. load() 도 실패해서 새 pml4 도 없음.
`return -1` 로 SYS_EXEC 까지 돌아왔지만, 유저 모드에서 실행할 페이지
테이블 자체가 없어서 어떤 명령어를 실행하려 해도 page fault.

### 시도 2: `process_cleanup()` 을 load() 이후로 이동

```c
success = load (argv[0], &_if);
if (!success) {
    palloc_free_page (file_name);
    return -1;
}
argument_stack (argv, argc, &_if);
palloc_free_page (file_name);
process_cleanup ();          /* ← 여기로 이동 */
do_iret (&_if);
```

**증상**: 같은 패턴의 page fault. 이번엔 do_iret 직후.

**원인**: 성공 케이스에서 `process_cleanup()` 이 **방금 load 한 새 pml4 까지**
destroy 해버림. load() 가 내부에서 `t->pml4 = pml4_create()` 로 새 페이지
테이블을 활성화했는데, do_iret 직전에 그걸 또 cleanup. 결과적으로 유저
모드 진입 시 페이지 테이블 없음.

### 시도 3: `process_cleanup()` 완전 제거

```c
/* process_exec 에서 process_cleanup 호출 자체를 삭제 */
success = load (argv[0], &_if);
if (!success) {
    palloc_free_page (file_name);
    return -1;            /* 여전히 단순 반환 */
}
argument_stack (argv, argc, &_if);
palloc_free_page (file_name);
do_iret (&_if);            /* cleanup 없이 바로 진입 */
```

**증상**: exec-missing 의 page fault 는 사라졌지만, 출력이 `exit(0)` 으로 끝남.

**원인**: load() 가 실패하면 새 pml4 가 절반쯤 만들어진 상태로 남아있을 수
있음. 그리고 (a) 시맨틱 (호출자에게 -1 반환) 자체가 테스트 기대와 어긋남.
exec("no-such-file") 이 -1 을 반환하지만, 그 후 유저 프로그램이 정상
실행을 이어가서 `exit(0)`. 테스트는 `exit(-1)` 을 기대.

> 여기서 처음으로 **시맨틱 자체가 잘못됐다**는 의심이 들었다.
> "load 실패 → 호출자에게 -1" 이 아니라 "load 실패 → 현재 프로세스 종료"
> 가 맞는 게 아닌가?

### 시도 4: load() 안에 old_pml4 백업/복원 추가

```c
static bool
load (const char *file_name, struct intr_frame *if_) {
    struct thread *t = thread_current ();
    uint64_t *old_pml4 = t->pml4;       /* 백업 */
    t->pml4 = pml4_create();
    if (t->pml4 == NULL)                /* ← 잘못된 NULL 체크 */
        goto done;
    ...
done:
    if (!success) {
        if (t->pml4 != NULL && t->pml4 != old_pml4)
            pml4_destroy(t->pml4);
        t->pml4 = old_pml4;
        if (old_pml4 != NULL)
            pml4_activate(t);             /* ← 타입 오류 */
    }
    return success;
}
```

**증상**: 컴파일 에러 — `pml4_activate` 가 `uint64_t*` 를 받는데
`thread*` 를 넘김.

**원인**: API 잘못 사용. `pml4_activate(old_pml4)` 가 맞다.

### 시도 5: API 수정 + `t->pml4 == NULL` NULL 체크

```c
if (t->pml4 == NULL)        /* ← 여전히 잘못 — 의도는 new_pml4 체크 */
    goto done;
```

**증상**: initd 첫 호출 시 즉시 load 실패. 출력이 전혀 안 나옴.

**원인**: initd 의 첫 process_exec 호출 시점에 `old_pml4 = t->pml4` 가 NULL.
새 pml4 를 만들고 `t->pml4 = new_pml4` 로 교체했지만, NULL 체크 변수가
잘못됐다. `t->pml4 == NULL` 은 "교체 직후" 가 아니라 "교체 전 백업값" 을
보고 있어서, 정상 케이스에서도 즉시 goto done 으로 빠짐.

### 시도 6: NULL 체크를 `new_pml4 == NULL` 로 수정

```c
uint64_t *new_pml4 = pml4_create();
if (new_pml4 == NULL)        /* ← 올바른 체크 */
    goto done;
t->pml4 = new_pml4;
```

**증상**: load 자체는 동작. exec-missing 흐름:
```
(exec-missing) begin
load: no-such-file: open failed
(exec-missing) exec("no-such-file"): -1
(exec-missing) end
exec-missing: exit(0)         ← 여전히 exit(0)
```

**원인**: 메모리 관리는 모두 정확해졌지만, **시맨틱 (a)** 를 그대로 가져가고
있어서 테스트가 기대하는 `exit(-1)` 이 나오지 않음. 시도 3 에서 의심했던
시맨틱 문제로 다시 돌아옴.

### 시도 7: SYS_EXEC 에서 `thread_exit()` 호출

```c
case SYS_EXEC: {
    ...
    int result = process_exec(fn_copy);
    /* 여기까지 왔으면 process_exec 실패 */
    thread_current()->exit_status = -1;
    thread_exit();           /* ← 시맨틱 (b): 프로세스 종료 */
    NOT_REACHED();
}
```

**증상**: `Kernel panic: PANIC at process.c: Fail to launch initd`

**원인**: exec-missing 자체를 로드할 때도 `initd()` 가 `process_exec("exec-missing")`
을 호출한다. 만약 SYS_EXEC 가 thread_exit 으로 종료된다면, initd 의
`process_exec` 도 실패 시 panic 으로 가버림.

initd 는 부팅 직후 첫 유저 프로그램을 띄우는 한 번만 호출되는 함수.
여기서 panic 나면 시스템 전체가 죽음.

### 최종: initd 의 PANIC 분기는 유지, SYS_EXEC 만 exit_status=-1

```c
/* initd 는 그대로 둠 — 첫 부팅 실패는 진짜 panic 이 맞음 */
static void
initd (void *f_name) {
    process_init();
    if (process_exec(f_name) < 0)
        PANIC("Fail to launch initd\n");
    NOT_REACHED();
}

/* SYS_EXEC 는 유저가 부른 exec 의 실패를 처리 */
case SYS_EXEC: {
    const char *filename = (const void *) f->R.rdi;
    validate_user_addr(filename);
    char *fn_copy = palloc_get_page(PAL_ZERO);
    strlcpy(fn_copy, filename, PGSIZE);

    int result = process_exec(fn_copy);
    /* process_exec 가 실패해서 돌아옴 (성공 시 do_iret 으로 진입해 안 돌아옴) */
    thread_current()->exit_status = -1;
    thread_exit();
    NOT_REACHED();
}
```

**핵심 인사이트**: `process_exec` 의 호출자가 두 종류라는 점이 함정이었다.

| 호출자 | 실패 시 행동 |
|---|---|
| `initd()` | PANIC (시스템 부팅 실패) |
| `SYS_EXEC` (유저가 부른 exec) | 현재 프로세스 종료 (exit_status=-1) |

같은 `process_exec` 함수가 호출 컨텍스트에 따라 다르게 처리되어야 한다는
사실을 7번째 시도에야 깨달았다.

---

## 3. 최종 수정 4곳

이 한 시맨틱 ("exec 실패 → 현재 프로세스 종료") 을 만들기 위해 4 함수가
정렬되어야 했다.

### 3.1 `load()` — old_pml4 백업/복원

```c
static bool
load (const char *file_name, struct intr_frame *if_) {
    struct thread *t = thread_current ();
    uint64_t *old_pml4 = t->pml4;       /* 1. 백업 */

    uint64_t *new_pml4 = pml4_create();
    if (new_pml4 == NULL)                /* 2. new_pml4 로 NULL 체크 */
        goto done;

    t->pml4 = new_pml4;                  /* 3. 임시로 교체 */
    process_activate (thread_current ());

    file = filesys_open (file_name);
    ... /* ELF 로드 */

    success = true;

done:
    file_close(file);
    if (!success) {
        /* 4. 실패 시 새 pml4 파괴, 원래 pml4 복원 */
        if (t->pml4 != old_pml4)
            pml4_destroy(t->pml4);
        t->pml4 = old_pml4;
        pml4_activate(old_pml4);
    }
    return success;
}
```

**왜 이 구조가 필요한가**: load() 는 프로세스 교체의 가장 위험한
구간 — 새 pml4 활성화와 ELF 로드 사이에서 어느 단계든 실패 가능. 이
함수가 자신의 실패를 스스로 정리(`pml4_destroy` + 복원)해야 호출자가
"실패하면 원래대로" 라는 invariant 를 가정할 수 있다.

### 3.2 `process_exec()` — process_cleanup 제거

```c
int
process_exec (void *f_name) {
    ...
    success = load (argv[0], &_if);

    if (!success) {
        /* load() 가 이미 pml4 복원함. 여기선 cmdline 페이지만 free */
        palloc_free_page (file_name);
        return -1;
    }

    argument_stack (argv, argc, &_if);
    palloc_free_page (file_name);
    /* process_cleanup() 제거됨 */
    do_iret (&_if);
    NOT_REACHED ();
}
```

원래 KAIST 템플릿엔 do_iret 직전 `process_cleanup()` 이 있었지만, load()
가 새 pml4 로 교체하는 시점에 이미 원래 주소공간이 자연스럽게 대체되므로
별도 cleanup 불필요. 오히려 호출하면 새 pml4 까지 destroy 됨 (시도 2).

### 3.3 `SYS_EXEC` — 실패 시 exit(-1)

```c
case SYS_EXEC: {
    const char *filename = (const void *) f->R.rdi;
    validate_user_addr(filename);
    char *fn_copy = palloc_get_page(PAL_ZERO);
    strlcpy(fn_copy, filename, PGSIZE);

    int result = process_exec(fn_copy);
    /* 여기까지 왔으면 실패 */
    thread_current()->exit_status = -1;
    thread_exit();
    NOT_REACHED();
}
```

`int result = process_exec(...)` 자체는 의미 없는 코드 (실패만 돌아오므로
result 는 항상 -1, 사용 안 함). 단지 컴파일러가 "여기서 끝났다" 는 걸
알게 하기 위한 호출.

### 3.4 `initd()` — PANIC 유지

```c
static void
initd (void *f_name) {
    process_init();
    if (process_exec(f_name) < 0)
        PANIC("Fail to launch initd\n");
    NOT_REACHED();
}
```

부팅 첫 프로그램 로드 실패는 시스템 자체의 문제이므로 PANIC 이 적절.
유저가 부른 exec 의 실패와는 다른 층위.

---

## 4. wait-twice TIMEOUT — 자료구조 corruption 디버깅

exec 가 통과한 직후, fork/exec/wait 16 개 테스트 중 **wait-twice 만**
TIMEOUT 으로 떨어졌다. 다른 모든 테스트가 PASS 인데 한 개만 안 되는
경우가 가장 디버깅이 어렵다 — 일반적인 결함이 아니라 특정 패턴이
트리거하는 corner case 이기 때문.

### 4.1 테스트 코드와 증상

```c
/* tests/userprog/wait-twice.c */
void
test_main (void)
{
  pid_t child;
  if ((child = fork ("child-simple"))) {
    msg ("wait(exec()) = %d", wait (child));    /* 1차 */
    msg ("wait(exec()) = %d", wait (child));    /* 2차 — 같은 pid */
  } else {
    exec ("child-simple");
  }
}
```

테스트가 기대하는 동작:
- 1 차 wait: 자식 종료까지 블록 → exit_status (=81) 반환
- 2 차 wait: 같은 pid 는 이미 회수됨 → 즉시 -1 반환

실제 출력:
```
(wait-twice) begin
(child-simple) run
child-simple: exit(81)
(wait-twice) wait(exec()) = 81     ← 1 차 성공
                                    ← 2 차에서 멈춤. TIMEOUT.
```

1 차는 정상이지만 2 차가 영원히 안 돌아옴.

### 4.2 다른 fork 테스트는 왜 통과했나

이 질문이 디버깅의 핵심 단서였다. fork-once / fork-multiple /
fork-recursive 등 모두 PASS. wait-twice 만 다른 패턴은 무엇인가?

```
fork-once:       wait() 1번 호출
fork-multiple:   wait() N번 호출 (각각 다른 pid)
fork-recursive:  wait() 다중 호출 (각각 다른 pid)
wait-twice:      wait() 2번 호출 (같은 pid)
```

**wait-twice 만 같은 pid 로 두 번 호출**한다. 즉 `process_wait` 함수의
"두 번째 호출 경로" 가 깨져 있다는 뜻.

### 4.3 `process_wait` 의 의도된 동작

```c
int
process_wait (tid_t child_tid) {
    /* 1. children 리스트에서 tid 검색 */
    for (e = list_begin(&cur->children); ...; e = list_next(e)) {
        if (list_entry(e, struct thread, child_elem)->tid == child_tid) {
            child = ...;
            break;
        }
    }
    if (child == NULL)
        return -1;             /* ← 같은 pid 두 번째 호출은 여기로 빠져야 함 */

    /* 2. 자식 종료 대기, status 회수 */
    sema_down (&child->wait_sema);
    exit_status = child->exit_status;
    list_remove (&child->child_elem);    /* ← 1 차에서 리스트에서 제거 */
    sema_up (&child->exit_sema);
    return exit_status;
}
```

**의도**: 1 차 wait 의 `list_remove` 가 child 를 children 리스트에서
빼주므로, 2 차 wait 의 검색 루프에서 못 찾고 `child == NULL` → `-1`
반환.

**실제 동작**: 2 차 wait 에서 children 리스트에 child 가 **여전히 남아
있어서** 검색이 성공하고, `sema_down(wait_sema)` 에서 영원히 블록.
자식은 이미 종료된 상태라 누가 sema_up 해줄 사람이 없음 → TIMEOUT.

> 즉 `list_remove` 가 효과를 발휘하지 못하고 있다.

### 4.4 진짜 원인 — 같은 elem 이 두 번 push_back

`list_remove` 가 무력화된 이유를 추적해 들어가니, 자식 생성 시
`child_elem` 이 **두 군데서** push_back 되고 있었다.

**등록 지점 1 — `thread_create()` (`threads/thread.c:207-208`)**

```c
#ifdef USERPROG
    t->parent = thread_current ();
    list_push_back (&thread_current ()->children, &t->child_elem);
#endif
```

`thread_create` 는 main 이 initd 를 만들 때부터 fork 자식을 만들 때까지
**모든 새 스레드 생성 경로의 단일 진입점**. 여기서 자식의 child_elem
을 부모의 children 에 자동 등록.

**등록 지점 2 — `__do_fork()` 안 (`userprog/process.c`)**

```c
static void
__do_fork (void *aux) {
    ...
    /* 부모-자식 관계 등록 — process_wait 가 children 리스트를 순회하므로
     * pml4 복사 전에 확실히 연결해 둔다. */
    current->parent = parent;
    list_push_back (&parent->children, &current->child_elem);   /* ★ 중복 */
    ...
}
```

내가 fork 구현하면서 "parent 연결을 명시적으로 하자" 는 의도로 추가.
하지만 thread_create 가 이미 같은 일을 해놓은 상태라 **같은 child_elem
이 children 리스트에 두 번 들어감**.

### 4.5 같은 elem 을 두 번 push_back 하면 어떻게 되나

Pintos 의 `struct list_elem` 은 intrusive doubly-linked list 노드:

```c
struct list_elem {
    struct list_elem *prev;
    struct list_elem *next;
};
```

`list_push_back` 의 동작 (간략화):

```c
void
list_push_back (struct list *list, struct list_elem *elem) {
    elem->prev = list->tail.prev;
    elem->next = &list->tail;
    list->tail.prev->next = elem;
    list->tail.prev = elem;
}
```

**같은 elem 으로 한 번 더 호출하면**:

```
1차 호출 후:
  list: head ↔ elem ↔ tail
  elem.prev = head, elem.next = tail

2차 호출:
  elem->prev = list->tail.prev          /* = elem 자신! */
  elem->next = &list->tail
  list->tail.prev->next = elem          /* elem->next = elem */
  list->tail.prev = elem
  
2차 호출 후:
  elem.prev = elem (자기 자신!)
  elem.next = tail (또는 elem)
  list 상태: head ↔ ... ↔ elem ↔ tail (구조 깨짐)
```

elem 의 prev 가 자기 자신을 가리키는 self-referential 상태가 된다.

### 4.6 self-referential elem 에서 list_remove 의 동작

```c
void
list_remove (struct list_elem *elem) {
    elem->prev->next = elem->next;
    elem->next->prev = elem->prev;
}
```

elem.prev = elem 이면:
```c
elem->prev->next = elem->next;    /* = elem->next = elem->next (자기 next 갱신) */
elem->next->prev = elem->prev;    /* = elem->next->prev = elem (자기 prev 갱신) */
```

**결과**: elem 자체의 prev/next 만 바뀌고, **list 의 head ↔ tail 사슬에서
elem 이 빠지지 않는다**. 리스트를 순회하면 elem 이 여전히 발견됨.

이게 1 차 wait 의 `list_remove` 가 무력화된 이유다. 리스트는 그대로,
다음 wait 호출이 같은 child 를 또 찾고, 이미 종료된 자식의 wait_sema
는 카운트 0 이므로 sema_down 이 영원히 블록.

### 4.7 다른 fork 테스트가 통과한 이유

`wait()` 을 한 번씩만 호출하면:
- 1 차 wait 의 sema_down/sema_up 사이클까지는 정상 동작
- list_remove 가 무력화되어도 그 후 wait 가 없으니 영향 없음
- 자식이 sema_up(exit_sema) 받고 정상 종료

**자료구조가 깨져 있어도 한 번만 쓰면 들키지 않는 패턴**이었다.
wait-twice 만이 두 번째 순회를 강제했고, 그 시점에 깨진 리스트가
드러났다.

### 4.8 수정 — `__do_fork()` 의 list_push_back 한 줄 제거

```c
static void
__do_fork (void *aux) {
    ...
    current->parent = parent;
    /* list_push_back 제거 — thread_create() 에서 이미 등록됨 */
    ...
}
```

`current->parent = parent` 라인은 thread_create 의 값과 동일해 redundant
지만 명시성을 위해 유지. list_push_back 한 줄만 삭제.

수정 후 wait-twice 포함 모든 테스트 PASS.

---

## 5. 시도했다가 버린 6가지 가설

wait-twice TIMEOUT 의 진짜 원인을 찾기 전 시도했던 가설들. 각각이 왜
틀렸는지 정리하면 디버깅에서 어떤 신호를 따라가야 했는지가 보인다.

### 5.1 가설: `process_exit` 에서 `parent != NULL` 일 때만 sema_down

**가설**: 부모가 NULL 인 프로세스는 sema_down(exit_sema) 에서 막힘.
조건부로 스킵하면 됨.

**왜 틀렸나**: initd 의 parent 가 thread_create 에서 main 으로 세팅되어
있어서 항상 non-NULL. wait-twice 부모 (= initd 가 exec 한 프로세스) 도
parent 가 main 으로 남아있음. 조건이 작동하지 않음.

**남긴 것**: process_exit 에서 parent 체크 자체는 좀비 누수 방지에
필요할 수 있어 일단 둠.

### 5.2 가설: `wait_called` 플래그 도입

**가설**: 부모가 wait 를 호출했는지 자식에게 알려주면 자식이 exit_sema
에서 안전하게 종료 여부를 판단할 수 있음.

```c
/* process_exit 에서 */
if (curr->parent != NULL && curr->wait_called)
    sema_down(&curr->exit_sema);
```

**왜 틀렸나**: race condition.
```
자식: sema_up(wait_sema)         ← 부모 깨움
자식: if (wait_called) ...       ← 아직 false (부모가 못 세팅)
자식: 그냥 종료 → struct 해제
부모: sema_down 깨어남 → use-after-free
```

자식의 wait_called 체크와 부모의 wait_called=true 세팅 사이에 race.

**남긴 것**: 동기화 문제는 race 가능성을 항상 시뮬레이션해서 봐야 한다는 교훈.

### 5.3 가설: `thread_create` 의 `list_push_back` 제거

**가설**: 등록 지점이 두 곳이라 중복이라면, `__do_fork` 의 push_back 만
남기고 thread_create 의 것을 제거하면 됨.

**왜 틀렸나**: thread_create 의 등록은 **모든 스레드 생성 경로의 단일
진입점** 역할. 제거하면 fork 가 아닌 다른 경로 (예: `run_task` 가
initd 를 만들 때) 에서 children 리스트 등록이 안 됨 → `process_wait
(initd_tid)` 가 initd 를 못 찾고 즉시 -1 반환 → Pintos 가 initd 실행
전에 종료. 출력이 전혀 안 나옴.

**남긴 것**: 자료구조 등록은 "공통 경로" 와 "특수 경로" 중 하나에서
해야 한다. 둘 다 하면 중복, 둘 다 안 하면 누락. **공통 경로에 두는
게 정답**이었고, `__do_fork` 의 추가 등록이 잘못이었다.

### 5.4 가설: `process_exec` 에서 `parent = NULL` 세팅

**가설**: exec 으로 교체된 프로세스는 새 프로세스이므로 parent 를 NULL
로 끊으면 process_exit 에서 sema_down 을 스킵할 수 있음.

**왜 틀렸나**: fork() 로 만든 자식이 exec() 을 호출하면 그 자식의 부모
관계도 끊겨버림. 부모가 wait(child) 호출하면 자식의 parent 가 NULL 이라
sema_up 흐름이 깨짐.

**남긴 것**: 같은 프로세스라도 "어떻게 만들어졌나" (initd vs fork)에
따라 부모 관계 의미가 다르다. 일률적 NULL 처리는 위험.

### 5.5 가설: `initd()` 에서 `parent = NULL` 세팅

**가설**: initd 만 정확히 식별해서 parent 를 NULL 로 끊으면 됨.

**왜 틀렸나**: initd 의 parent 는 NULL 이 맞지만, 이건 wait-twice TIMEOUT
의 근본 원인이 아니었다. 증상의 일부 (process_exit 가 sema_down 에서 막힘)
는 해결되지만, 진짜 문제 (children 리스트가 깨져서 list_remove 무력화) 는
그대로 남음.

**남긴 것**: 증상 한 개만 보고 가설을 세우면 일부만 고친다. 여러 증상을
동시에 설명하는 가설을 찾아야 한다.

### 5.6 가설: 부모 `process_exit` 에서 자식들 exit_sema 풀어주기

**가설**: 부모가 종료할 때 wait 안 한 자식들의 exit_sema 를 일괄
sema_up 하면 자식이 영원히 블록되는 일이 없음.

```c
/* process_exit 에서 */
for (e = list_begin(&curr->children); ...; e = list_next(e)) {
    sema_up(&list_entry(e, struct thread, child_elem)->exit_sema);
}
```

**왜 틀렸나**: 좀비 누수는 막을 수 있지만, wait-twice 의 진짜 문제 (1차
wait 가 list_remove 에 실패해서 2차 wait 가 같은 자식을 또 찾는 것) 은
해결 못 함.

**남긴 것**: 이 정리 로직은 다른 시나리오 (부모가 wait 안 하고 종료) 에
유용하므로 코드는 유지. 단, wait-twice 의 해결책은 아니었다.

### 시도들이 공유한 함정

6 개 시도 모두 **`process_exit` 의 sema_down 이 막히는 증상**에 집중했다.
하지만 진짜 원인은 한 단계 위 — `process_wait` 의 `list_remove` 가 깨져서
**2 차 wait 가 일어나선 안 될 sema_down 을 호출하고 있던 것**.

증상의 가시적 위치 (process_exit 의 블록) 와 원인의 위치 (process_wait
의 list_remove 무력화) 가 분리되어 있었다. 그래서 가시적 위치에서
한 손씩 가설을 짜는 동안 진짜 원인은 손도 못 댐.

**호출 순서를 끝까지 따라가서, "왜 이 sema_down 이 호출되었는가" 를
역으로 추적**했어야 했다.

---

## 6. 두 사건이 남긴 교훈

### 6.1 "한 번은 되고 두 번은 깨진다" 는 자료구조 corruption 의 시그니처

wait-twice 가 다른 fork 테스트와 유일하게 다른 점이 "같은 pid 로 두
번 wait" 였다. 이런 비대칭은 거의 항상 자료구조 invariant 깨짐의
신호다.

| 시그니처 | 의심해야 할 것 |
|---|---|
| 한 번 호출은 OK, N 번째 깨짐 | 자료구조 corruption (이중 등록, dangling) |
| 매번 깨지지만 결과가 매번 다름 | race condition |
| 특정 입력에만 깨짐 | 경계 조건, off-by-one |
| 매번 같은 곳에서 같은 식으로 깨짐 | 단순 로직 버그 |

이번 wait-twice 는 1 번째 시그니처. fork 테스트 16 개 중 wait-twice
만 다르다는 사실이 자료구조 의심의 출발점이었어야 했는데, 그걸
인지하기까지 6 개 가설을 거쳤다.

### 6.2 한 시맨틱이 여러 함수에 걸쳐 정렬되어야 할 때

exec-missing 7 단계가 보여준 패턴: **"exec 실패 = 현재 프로세스 종료"**
라는 한 시맨틱을 만들기 위해 4 함수가 정확히 정렬되어야 한다.

```
load()        → 자기 실패는 자기가 정리 (pml4 복원)
process_exec → 호출자에게 -1 (state 보존)
SYS_EXEC     → -1 받으면 exit(-1) (시맨틱 결정)
initd()      → 실패는 PANIC (다른 컨텍스트라 다른 처리)
```

각 함수는 자기 책임만 본다. 하지만 한 시맨틱을 따라가려면 모두 같은
방향을 봐야 한다. **하나만 어긋나도 전체가 작동 안 함**.

> 7 단계의 본질은 "이 시맨틱을 어디까지 가져가야 하는지" 를 함수
> 경계 별로 발견해 가는 과정이었다. 처음엔 load() 한 곳만 고치면
> 될 줄 알았다.

### 6.3 등록은 한 곳에서만

자식을 children 리스트에 등록하는 책임은 **반드시 한 곳에만** 있어야
한다. thread_create 가 공통 경로이므로 거기서 하고, `__do_fork` 는
"이미 등록되어 있다" 는 invariant 를 신뢰해야 한다.

이중 등록은:
1. 명시성 욕심 ("내가 직접 등록해야 안전하지") 에서 비롯
2. 다른 등록 지점이 보이지 않아 "여기 없는 줄" 알고 추가
3. 한 번에는 안 깨지므로 발견이 늦음

**자료구조 변경 시점에 이미 그 자료구조에 손대는 다른 코드가 있는지
검색**하는 습관이 필요하다.

### 6.4 디버깅에서 "다른 테스트는 왜 통과했나" 가 가장 강한 신호

wait-twice 에서 결정적 단서는 "fork-once / fork-multiple 은 다 PASS 인데
이거만 fail" 이었다. 이 비대칭이:
- "wait 를 두 번 호출하는 차이가 무엇인가?" → list_remove 의 효과 검증
- "list_remove 가 효과 없다면?" → 리스트 자체가 깨져 있을 가능성
- "리스트가 어떻게 깨졌나?" → 등록 지점 추적

으로 이끌었다. 모든 테스트가 다 fail 인 상태였다면 일반적 동기화
문제로 의심했을 텐데, **15 개 PASS / 1 개 fail 의 비율 자체가 정보**
였다.

---

## 7. 최종 상태 — 모든 fork/exec/wait 테스트 PASS

```
fork-once         PASS
fork-multiple     PASS
fork-recursive    PASS
fork-read         PASS
fork-close        PASS
fork-boundary     PASS
exec-once         PASS
exec-arg          PASS
exec-missing      PASS    ← §1~§3 의 7단계 시행착오 끝에
exec-boundary     PASS
exec-bad-ptr      PASS
exec-read         PASS
wait-simple       PASS
wait-twice        PASS    ← §4 의 자료구조 corruption 수정 후
wait-killed       PASS
wait-bad-pid      PASS
```

---

## 8. 한 줄 요약

> exec-missing 은 한 시맨틱을 4 함수에 정렬하는 문제였고, wait-twice
> 는 자료구조에 같은 노드를 두 번 등록한 한 줄을 추적하는 문제였다.
> 두 사건 모두 "증상이 보이는 위치" 와 "원인이 있는 위치" 가
> 분리되어 있었고, 호출 흐름을 끝까지 거꾸로 따라가는 것만이
> 진짜 원인에 도달하는 길이었다.

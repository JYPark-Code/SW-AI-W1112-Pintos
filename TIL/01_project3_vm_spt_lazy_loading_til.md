# Pintos Project 3 — SPT 해시테이블 초기화와 Lazy Loading 설계

> KAIST 64bit Pintos Project 3 — Virtual Memory 첫 단계 회고.
> Supplemental Page Table(SPT)을 해시테이블로 구성하고,
> `vm_alloc_page_with_initializer` / `spt_insert_page` 까지 구현한 날의 기록.
> 코드를 짜기 전에 "왜 page fault 가 정상 동작인지", "SPT 가 pml4 와 무엇이
> 다른지", "uninit_new 가 initializer 를 왜 즉시 호출하지 않는지" 를
> 먼저 납득해야 다음으로 넘어갈 수 있었다.
>
> | 섹션 | 주제 | 무게중심 |
> |---|---|---|
> | §1 | 오늘 한 작업 요약 | 커밋 61394cc 가 만든 변화 5가지 |
> | §2 | Lazy Loading 의 설계 의도 | "page fault 는 버그가 아니라 약속이다" |
> | §3 | SPT vs pml4 | 두 테이블이 답하는 질문이 다르다 |
> | §4 | uninit_new 와 함수 포인터 저장 | 실행을 page fault 시점으로 미루는 이유 |
> | §5 | hash_entry 매크로의 동작 원리 | container_of 패턴, offsetof 역산, Project 1 condvar 와의 구조적 동일성 |
> | §6 | hash_find 에서 less 를 두 번 쓰는 이유 | 동등성을 "둘 다 작지 않다" 로 표현 |
> | §7 | `spt_insert_page` 와 hash_insert 시맨틱 | 중복 키 invariant |
> | §8 | `spt_find_page` — 임시 객체로 키 표현하기 | §6.2 패턴의 실제 적용 |
> | §9 | 핵심 개념 정리 | 한 줄 요약 모음 |
> | §10 | 다음에 할 일 | fault handler → claim → swap |

---

## 1. 오늘 한 작업 요약

커밋 [`61394cc`](../../../commit/61394cc) — *feat(vm): SPT 해시테이블 초기화 및
페이지 삽입 구현* 에서 다음 5가지가 변경됐다.

| # | 변경 | 위치 |
|---|---|---|
| 1 | `struct supplemental_page_table` 에 `struct hash pages` 추가 | `include/vm/vm.h` |
| 2 | `page_hash`, `page_less` 보조 함수 구현 | `vm/vm.c` |
| 3 | `supplemental_page_table_init` 에서 `hash_init` 호출 | `vm/vm.c` |
| 4 | `vm_alloc_page_with_initializer` 에서 타입별 initializer 분기 + `uninit_new` 호출 | `vm/vm.c` |
| 5 | `spt_insert_page` 를 `hash_insert` 로 구현 | `vm/vm.c` |

코드 라인 수는 적지만, 그 뒤의 설계 의도 — **Lazy Loading** — 를 받아들이는
데 시간이 더 걸렸다.

---

## 2. Lazy Loading 의 설계 의도

### 2.1 처음 한 오해

`vm_alloc_page_with_initializer` 만 봐서는 "페이지를 할당한다" 라는 이름인데
실제로 `palloc_get_page` 같은 물리 프레임 할당이 어디에도 없다. pml4 매핑도
안 한다. 그저 `malloc` 으로 `struct page` 만 만들어서 SPT 해시에 넣는다.

처음에는 이게 미완성이거나 잘못 짠 코드처럼 보였다. 실제로 프로세스가
이 가상주소를 읽으면 매핑이 없으니까 page fault 가 난다.

### 2.2 깨달은 것: 그게 정상이다

Lazy Loading 의 핵심은 **"등록은 미리, 로드는 나중에"** 다.

```
process_load()
   │
   ├─ vm_alloc_page_with_initializer(...)
   │     ├─ struct page 생성 (malloc)
   │     ├─ uninit_new()  ← initializer 만 저장
   │     └─ spt_insert_page() → SPT 에 등록
   │
   │    ※ 이 시점에 pml4 매핑 없음, 물리 프레임 없음
   │
   └─ schedule()
        │
        프로세스가 그 가상주소에 접근
        │
        ▼
   page fault!
        │
   vm_try_handle_fault()  ← page fault 가 "실제 로드" 의 트리거
        │
        └─ SPT 에서 페이지 찾아서 실제로 frame 할당, 매핑
```

**Page fault 는 버그 신호가 아니라 OS 한테 "이제 진짜로 올려달라" 고 말하는
공식 채널이다.** 이걸 한 번 받아들이고 나니 이후 코드가 자연스럽게 읽혔다.

---

## 3. SPT vs pml4 — 두 테이블은 답하는 질문이 다르다

처음에 헷갈렸던 부분: 왜 굳이 SPT 를 또 만드는가? pml4 가 이미 가상주소 →
물리주소 매핑을 가지고 있는데?

표로 정리하면 답이 명확해졌다.

| 질문 | pml4 | SPT |
|---|---|---|
| "이 가상주소가 **지금 매핑되어 있나**?" | 답한다 | 모른다 |
| "이 가상주소가 **유효한 주소이긴 한가**?" (아직 안 올라온 거라도) | 모른다 | 답한다 |
| "이 페이지는 어떤 타입이지? (anon/file/uninit)" | 모른다 | 답한다 |
| "디스크 어디서 로드해야 하지?" | 모른다 | 답한다 |
| "swap out 됐다면 swap slot 은 어디?" | 모른다 | 답한다 |

pml4 는 **현재 매핑된 것만** 안다. 그래서 page fault 가 났을 때 "이게 진짜
나쁜 접근인지, 아니면 lazy load 해줘야 할 페이지인지" 를 pml4 만으로는
구분할 수 없다. 그 판단을 위한 자료구조가 SPT 다.

> **한 줄 요약**: pml4 는 "지금 무엇이 매핑돼 있나", SPT 는 "이 프로세스가
> 무엇을 가질 수 있는가".

---

## 4. `uninit_new` 와 함수 포인터 저장

### 4.1 구현 코드

```c
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
                                vm_initializer *init, void *aux) {
    ASSERT (VM_TYPE(type) != VM_UNINIT)

    struct supplemental_page_table *spt = &thread_current ()->spt;
    struct page *page = (struct page *)malloc(sizeof(struct page));

    if (spt_find_page (spt, upage) == NULL) {
        if (page == NULL)
            goto err;

        /* 함수 포인터: 타입별 진짜 initializer 를 고른다 */
        bool (*page_initializer)(struct page *, enum vm_type, void *);
        switch (VM_TYPE(type)) {
        case VM_ANON:
            page_initializer = anon_initializer;
            break;
        case VM_FILE:
            page_initializer = file_backed_initializer;
            break;
        default:
            break;
        }

        /* uninit_new 는 initializer 를 page 에 "저장만" 한다.
         * 실제 호출은 page fault 시점까지 미뤄진다. */
        uninit_new(page, upage, init, type, aux, page_initializer);

        if (!spt_insert_page(spt, page))
            goto err;
        return true;
    }
err:
    free(page);
    return false;
}
```

### 4.2 왜 즉시 호출하지 않고 저장만 하는가

`uninit_new` 가 `anon_initializer` 를 *지금 바로* 호출해버리면 그건 lazy 가
아니라 eager 다. 그러면:

- 프로세스가 영영 안 건드릴 페이지까지 미리 frame 잡아둠 → 메모리 낭비
- 모든 페이지를 한꺼번에 올리느라 프로세스 시작이 느림
- swap 정책의 의미가 약해짐 (이미 다 올라온 상태에서 swap 만 함)

그래서 `uninit_new` 는 **"네가 page fault 로 다시 깨어났을 때 누구를
부를지" 만 page 에 적어두고** 끝낸다. 실제 호출은 fault handler 의 책임.

이게 함수 포인터(`page_initializer`) 를 굳이 저장하는 이유다. 함수 포인터는
"실행을 나중으로 미루는 도구" 다.

---

## 5. `hash_entry` 매크로의 동작 원리

`page_hash` / `page_less` 안에서 매번 쓰는 `hash_entry` 가 처음엔 마법
같았다. `hash_elem*` 을 넣으면 `page*` 가 나온다. 어떻게?

```c
#define hash_entry(HASH_ELEM, STRUCT, MEMBER)                   \
    ((STRUCT *) ((uint8_t *) &(HASH_ELEM)->list_elem            \
        - offsetof (STRUCT, MEMBER.list_elem)))
```

### 5.1 메모리 그림

```
struct page {
    const struct page_operations *operations;   ← +0
    void *va;                                   ← +8
    struct frame *frame;                        ← +16
    struct hash_elem spt_elem;                  ← +24  ← 이게 hash_elem
    ...
};
```

`offsetof(struct page, spt_elem.list_elem)` = **24 바이트**.

해시테이블이 들고 있는 건 `&page->spt_elem.list_elem` 의 주소다.
그 주소에서 24 를 빼면? `page` 의 시작 주소가 나온다.

### 5.2 container_of 패턴

이건 리눅스 커널에서 유명한 `container_of` 패턴이다. 핵심 아이디어:

> "구조체 멤버의 주소"만 알아도, 그 멤버가 구조체 내에서 차지하는
> **오프셋만큼 빼면** 바깥 구조체의 시작 주소로 돌아갈 수 있다.

이게 가능한 이유는 C 의 struct 레이아웃이 컴파일 타임에 결정되기 때문이다.
`offsetof` 는 컴파일러가 계산해주는 상수다.

이 패턴 덕분에 `hash_elem` 은 **어떤 구조체에든 박혀 들어갈 수 있다.**
list, hash, tree 가 동일한 트릭으로 generic 하게 동작한다.

### 5.3 page_hash / page_less

```c
static uint64_t
page_hash(const struct hash_elem *e, void *aux UNUSED) {
    struct page *p = hash_entry(e, struct page, spt_elem);
    return hash_bytes(&p->va, sizeof(p->va));
}

static bool
page_less(const struct hash_elem *a, const struct hash_elem *b,
          void *aux UNUSED) {
    struct page *pa = hash_entry(a, struct page, spt_elem);
    struct page *pb = hash_entry(b, struct page, spt_elem);
    return pa->va < pb->va;
}
```

해시테이블은 `hash_elem*` 만 보지만, 실제 비교하고 싶은 건 `page->va` 다.
`hash_entry` 가 그 다리를 놓아준다.

### 5.4 Project 1 condvar 와의 구조적 동일성 — 같은 패턴, 다른 옷

오늘 `spt_find_page` 를 짜다가 갑자기 깨달았다. 이 구조, **Project 1 의
condition variable 에서 이미 본 것**이다. 두 프로젝트가 데이터 구조는 다른데
*뼈대* 는 정확히 똑같다.

#### 두 구조의 모양

```
condvar  (Project 1)
└─ waiters (struct list)
       └─ list_elem (← list 가 들고 있는 노드)
              ▲
              └─ semaphore_elem  안에 박혀 있음
                     └─ struct semaphore

SPT  (Project 3)
└─ pages (struct hash)
       └─ hash_elem (← hash 가 들고 있는 노드)
              ▲
              └─ struct page  안에 박혀 있음
                     └─ void *va
```

컨테이너(list / hash)는 자기가 들고 있는 게 **그냥 elem 노드일 뿐**이라고
믿고 살아간다. 사용자 데이터(`semaphore_elem`, `page`)는 그 노드 주위에
"감싸여" 있고, 필요할 때 역산해서 꺼낸다.

#### 코드도 평행하다

| | Project 1 | Project 3 |
|---|---|---|
| 컨테이너 타입 | `struct list` | `struct hash` |
| 박힌 노드 | `struct list_elem elem` | `struct hash_elem spt_elem` |
| 역산 매크로 | `list_entry(e, T, MEMBER)` | `hash_entry(e, T, MEMBER)` |
| 실사용 | `sema = list_entry(e, struct semaphore_elem, elem);` | `page = hash_entry(e, struct page, spt_elem);` |
| 키 비교 기준 | `sema_priority(a) < sema_priority(b)` | `pa->va < pb->va` |

두 매크로의 본체도 똑같다 — **elem 주소에서 `offsetof` 만큼 빼서 바깥
구조체 시작 주소를 얻는다.**

```c
#define list_entry(LIST_ELEM, STRUCT, MEMBER) \
    ((STRUCT *) ((uint8_t *) &(LIST_ELEM)->next \
        - offsetof(STRUCT, MEMBER.next)))

#define hash_entry(HASH_ELEM, STRUCT, MEMBER) \
    ((STRUCT *) ((uint8_t *) &(HASH_ELEM)->list_elem \
        - offsetof(STRUCT, MEMBER.list_elem)))
```

#### 핵심 원리 (둘 다 공통)

```
바깥 구조체 시작 주소  =  elem 주소  -  offsetof(컨테이너_타입, 멤버)
```

이중 포인터 역참조도, 별도의 back-pointer 도 아니다. **컴파일 타임 상수인
`offsetof` 로 산술 연산만 하면** 바깥 구조체로 돌아간다. 이게 가능한 이유:
struct 의 메모리 레이아웃은 컴파일 타임에 확정되니까.

#### 왜 이게 중요한가

- Pintos 의 list / hash / (그리고 향후 frame table, mmap list 까지) **모든
  컨테이너가 동일한 트릭**으로 작동한다. 한 번 익히면 끝.
- `struct page` 가 *list 에 들어가야 할 수도, hash 에 들어가야 할 수도*
  있는데, 각각의 elem 멤버를 따로 박아두면 **같은 객체가 여러 컨테이너에
  동시에 소속될 수 있다.** (실제로 `struct thread` 가 `elem`, `allelem`
  두 개의 `list_elem` 을 갖는 게 같은 이유)
- "어떤 구조체에든 박혀 들어가는 노드" 라는 발상이 **C 에서 제네릭을
  흉내내는 표준 방식**이다. C++ 의 템플릿, Rust 의 trait 가 컴파일러
  지원으로 하는 일을, C 는 매크로 + offsetof 로 한다.

> **메타 교훈**: 새 프로젝트로 넘어왔다고 새 패턴이 나오는 게 아니다.
> 같은 패턴이 다른 옷을 입고 다시 등장한다. Project 1 에서 본 것을 다시
> 알아보는 능력이 Project 3 의 진짜 가속 요인이었다.

---

## 6. `hash_find` 에서 `less` 를 두 번 쓰는 이유

처음에 헷갈렸던 부분: 비교 함수가 왜 `less` 만 있고 `equal` 은 없지?

Pintos hash 의 시그니처는 이렇다.

```c
typedef bool hash_less_func (const struct hash_elem *a,
                             const struct hash_elem *b,
                             void *aux);
```

`a < b` 만 알려주는 함수다. 그런데 `hash_find` 는 "같은 키를 찾고 싶다."
어떻게?

### 6.1 `hash_find` 내부 동작

```c
/* hash_find 가 버킷을 훑으면서 매번 하는 비교 */
if (!h->less(hi, e, h->aux) && !h->less(e, hi, h->aux))
    return hi;
```

이게 전부다. **less 를 두 번 호출해서 동등성을 합성한다.**

### 6.2 왜 `==` 를 못 쓰는가

이 질문이 처음엔 안 와닿았다. 어차피 같은 키면 `hash_elem*` 도 같지 않나?
→ **틀렸다.** `==` 는 **포인터 주소 비교**이고, 탐색할 때 직접 만든 임시
`hash_elem` 의 주소와 SPT 안에 이미 들어있는 실제 `hash_elem` 의 주소는
서로 다르다.

`spt_find_page` 의 전형적인 구현 패턴을 떠올려보면 분명해진다.

```c
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
    struct page key;          /* ← 스택에 잠깐 만든 임시 page */
    key.va = pg_round_down(va);
    struct hash_elem *e = hash_find(&spt->pages, &key.spt_elem);
    return e ? hash_entry(e, struct page, spt_elem) : NULL;
}
```

여기서 `&key.spt_elem` 은 **스택 위의 주소**다. 진짜 page 의 `spt_elem` 은
**힙 어딘가의 주소**다. 둘은 같은 va 를 표현하지만 주소 자체는 절대 같지
않다.

### 6.3 구체 예시 — 주소는 다른데 키는 같은 상황

```
실제 page  (힙, 주소 0xAAAA1000)
  ├─ va       = 0x5000
  └─ spt_elem (주소 0xAAAA1010)   ← 해시 버킷이 들고 있는 elem

임시 page  (스택, 주소 0xBBBB2000)
  ├─ va       = 0x5000            ← 같은 키
  └─ spt_elem (주소 0xBBBB2010)   ← hash_find 에 넘긴 elem

==  비교:  0xAAAA1010 != 0xBBBB2010   → 못 찾음 ✗

less 두 번:
  !(0x5000 < 0x5000)  →  true
  !(0x5000 < 0x5000)  →  true
  ───────────────────────────────
  true && true        →  찾았다 ✓
```

`less` 를 두 번 쓰면 **포인터 주소가 달라도 `va` 값으로 동등 판단** 이
된다. 이게 핵심이다.

### 6.4 일반 원리

```
!(a < b) && !(b < a)   ⟹   a == b   (사용자가 정의한 키 의미상)
```

이 트릭의 장점은 사용자가 `less` 하나만 정의하면 정렬, 동등성, 정렬된
삽입까지 다 처리 가능하다는 것. C++ STL 의 `std::set` 도 같은 규약(strict
weak ordering)을 쓰고, Java 의 `Comparator` 도 결국 같은 역할이다. C 처럼
**제네릭이 없는 언어에서 해시/트리 컨테이너를 구현하는 관용적 패턴**이다.
Pintos 가 굳이 이상한 짓을 한 게 아니다.

> 다만 부작용: 한 번의 lookup 에 `less` 호출이 두 번 → 비교 비용이
> 비싼 타입이면 약간 손해. 우리 경우는 `void*` 두 개 비교라 무시 가능.

### 6.5 "그럼 `page_equal` 을 따로 만들면 한 번에 끝나지 않나?" — API 디자인 관점

좋은 후속 질문이다. **"`hash_find` 내부에서 `less` 를 두 번 부르지 말고,
어차피 `hash_entry` 로 풀어서 `pa->va == pb->va` 한 번만 비교하면 되잖아?"**
— 의미상 맞는 말이다. 하지만 그게 안 되는 이유는 효율이 아니라 **API 가
받는 함수가 `less` 하나뿐**이기 때문이다.

#### 두 종류의 `==` 를 다시 분리해서 보자

| 의미 | 가능한가? | 이유 |
|---|---|---|
| **포인터 ==** (`a == b` 가 elem 주소 비교) | ✗ | §6.2 그대로. 임시 elem 과 실제 elem 의 주소가 다름 |
| **값 ==** (`pa->va == pb->va`, hash_entry 로 푼 뒤 의미 비교) | ✓ 의미상 가능. 그러나 호출 통로가 없음 | hash API 가 `equal_func` 을 받지 않음 |

`hash_find` 의 본체를 다시 보면 명확하다.

```c
struct hash_elem *
hash_find (struct hash *h, struct hash_elem *e) {
    ...
    for (each hi in bucket)
        if (!h->less(hi, e, h->aux) && !h->less(e, hi, h->aux))
            return hi;
    ...
}
```

`h` 가 들고 있는 함수 포인터는 `hash` / `less` / `aux` 셋뿐. 사용자가
아무리 `page_equal` 을 잘 짜놔도 **라이브러리가 그걸 호출할 길이 없다.**

#### 왜 API 를 그렇게 좁게 만들었는가

API 디자이너 입장에서 보면 선택지는 둘이었다.

| 선택 A: `less` 만 받기 (Pintos 가 고름) | 선택 B: `less` + `equal` 둘 다 받기 |
|---|---|
| 사용자 부담: 함수 1개 | 사용자 부담: 함수 2개 |
| 일관성: less 하나로 모든 비교 유도 → 모순 불가능 | 일관성: 두 함수가 어긋날 위험 (실제 버그 빈발) |
| 성능: 비교 호출 ×2 | 성능: 비교 호출 ×1 |
| 라이브러리 코드: 단순 | 라이브러리 코드: 두 경로 유지 |

**핵심은 일관성 차단**이다. 만약 B 를 골랐다면, 사용자가 이런 모순된 한
쌍을 짤 수도 있다.

```c
/* 사용자 실수: equal 은 va 만 보고, less 는 thread_id 까지 본다 */
bool page_equal(a, b) { return pa->va == pb->va; }
bool page_less (a, b) {
    if (pa->va != pb->va) return pa->va < pb->va;
    return pa->tid < pb->tid;          /* ← 추가 키 */
}
```

이러면 hash 가 같은 키로 판정한 두 원소를 정렬 순서로는 다르게 보게 되어
**탐색이 무한히 빗나가는** 미묘한 버그가 생긴다. `less` 하나만 받는 API
에서는 이 모순 자체가 표현 불가능하다 — 일관성이 *문법적으로* 강제된다.

#### 같은 철학이 다른 언어에도 있다

- **C++ STL** 의 컨테이너(`std::set`, `std::map`, `std::sort`): `operator<`
  하나만 요구 (strict weak ordering). `==` 는 `!(a<b) && !(b<a)` 로 유도.
- **Java** `Comparable.compareTo` / `Comparator.compare`: 단일 함수에
  음수/0/양수 세 의미를 다 묶음. `equals` 와 일관성은 따로 명시적 계약.
- **Rust** `Ord` trait: `cmp` 하나로 전체 순서를 유도.

Pintos 가 "이상한 짓" 을 한 게 아니라, **단일 비교 함수로 동등성을
파생시키는** 표준 디자인을 따른 것뿐이다.

#### 성능은 정말 신경 안 써도 되나

`less` 한 번 더 호출의 비용을 따져보면:

- 비교 자체: `void*` 두 개 비교 — 명령어 한두 개.
- 함수 호출 오버헤드: 컴파일러가 `static inline` 처리하면 사라짐.
- `hash_entry` 의 offset 산술도 컴파일 타임 상수라 추가 비용 없음.

요컨대 **현실 워크로드에서 측정 가능한 차이가 안 난다.** 디자이너가
일관성을 위해 성능을 희생한 게 아니라, 희생할 성능 자체가 없었다.

> **메타 교훈**: "왜 더 효율적인 방식을 안 썼지?" 라는 질문에 답이
> "효율은 똑같고, 잘못 쓸 가능성을 줄였다" 인 경우가 의외로 많다. API
> 디자인은 자주 *능력 제한* 의 예술이다.

---

## 7. `spt_insert_page` 와 hash_insert 의 시맨틱

```c
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
                 struct page *page UNUSED) {
    int succ = false;
    if (hash_insert(&spt->pages, &page->spt_elem) == NULL) {
        succ = true;
    }
    return succ;
}
```

`hash_insert` 의 반환값 규약:

| 반환값 | 의미 |
|---|---|
| `NULL` | 중복 키 없음. 정상 삽입 완료 |
| `hash_elem*` | 같은 키가 이미 있었음. 기존 elem 반환, 삽입 안 됨 |

그래서 `== NULL` 일 때만 `succ = true`. 이게 의미적으로 **"같은 VA 의
페이지가 두 번 등록되지 않는다"** 라는 invariant 를 자연스럽게 보장한다.

---

## 8. `spt_find_page` — 임시 객체로 키 표현하기

커밋 [`1ff5a78`](../../../commit/1ff5a78) — *feat(vm): spt_find_page 해시
탐색 구현*. §6.2 에서 예고했던 **"임시 `struct page` 를 스택에 만들어
키 표현으로 쓰는"** 패턴을 실제 코드로 구현한 날이다.

### 8.1 구현 코드

```c
/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
    struct page *page = NULL;
    struct page temp;
    temp.va = pg_round_down(va);

    struct hash_elem *e = hash_find(&spt->pages, &temp.spt_elem);
    if (e == NULL)
        return NULL;

    page = hash_entry(e, struct page, spt_elem);
    return page;
}
```

### 8.2 세 줄 안에 오늘까지 배운 트릭이 다 들어간다

| 줄 | 트릭 | 어디서 정리했나 |
|---|---|---|
| `temp.va = pg_round_down(va)` | 페이지 경계로 키 정규화 | 페이지 단위 등록 규약 |
| `hash_find(..., &temp.spt_elem)` | "키만 채운 임시 객체" 패턴 | §6.2 |
| `hash_entry(e, struct page, spt_elem)` | 멤버 주소 → 컨테이너 역산 | §5 container_of |

### 8.3 `pg_round_down` — 왜 필요한가

한 줄로 말하면: **"등록 시점의 키 표기"** 와 **"조회 시점의 입력 표기"**
가 다르기 때문에 입력을 등록 표기로 맞춰주는 정규화 단계다. 이 한 줄이
빠지면 거의 모든 fault 가 미스가 된다. 왜 그런지 차근차근 풀어본다.

#### 8.3.1 페이지 단위 매핑이라는 약속

x86-64 의 가상메모리는 **4 KB(=`PGSIZE`=4096=2^12) 단위**로만 매핑된다.
pml4/pdpt/pd/pt 어느 레벨에서 봐도 하나의 엔트리가 가리키는 단위는 페이지
한 장. 그래서:

- **모든 페이지의 시작 주소는 하위 12 비트가 0** 이다. (`0x5000`, `0x6000`,
  `0x7F8A_3000`, ...)
- pml4 와 SPT 가 들고 있는 키도 전부 그 정렬된 주소다. 그 외 형태로는
  애초에 매핑이 불가능하다.

#### 8.3.2 등록 시점에는 이미 정렬되어 있다

`vm_alloc_page_with_initializer(type, upage, ...)` 의 `upage` 는 **호출자
책임으로 페이지 정렬돼서** 들어온다. 예를 들어 `load_segment` 가 ELF
세그먼트를 등록하는 루프는 이렇게 생겼다.

```c
while (read_bytes > 0 || zero_bytes > 0) {
    ...
    vm_alloc_page_with_initializer(VM_ANON, upage, ...);
    upage += PGSIZE;                  /* ← 항상 4KB 씩 증가 */
}
```

`upage` 는 매번 `PGSIZE` 만큼 더해지므로 SPT 에 들어가는 키는
`{0x400000, 0x401000, 0x402000, ...}` 처럼 **반드시 정렬된 값들의 집합**
이다.

#### 8.3.3 조회 시점은 정렬되어 있지 *않다*

문제는 `spt_find_page` 가 호출되는 상황이다. 누가 부르는가?

- **Page fault 핸들러**: CPU 가 알려주는 fault 주소 = 실제로 접근한 주소.
  사용자 코드가 `mov rax, [0x5048]` 했다면 fault 주소는 `0x5048` 이다.
  CPU 는 절대 "친절하게 0x5000 으로 정렬해서" 주지 않는다.
- **시스템콜 인자 검증**: `read(fd, buf, n)` 의 `buf` 는 사용자가 준
  임의 주소. `0x5048` 일 수도 있고 `0x5FFF` 일 수도 있다.

즉 **등록 키 집합은 정렬돼 있는데, 조회 키는 정렬돼 있지 않다.** 그대로
`hash_find` 에 넘기면 매치될 리가 없다.

#### 8.3.4 `pg_round_down` 의 동작 — 비트 마스크

```c
#define pg_round_down(va)  ((void *)((uintptr_t)(va) & ~PGMASK))
/* PGMASK = PGSIZE - 1 = 0xFFF */
```

하위 12 비트만 0 으로 만드는 1-cycle 비트 연산이다. 실제 값으로 보면:

```
va        = 0x0000_0000_0000_5048
            ─────────────────────
                          ... 0101 0000 0100 1000

~PGMASK   = 0xFFFF_FFFF_FFFF_F000
            ─────────────────────
                          ... 1111 0000 0000 0000

va & ~PGMASK  =  0x0000_0000_0000_5000
                                       ↑↑↑
                              하위 12비트가 항상 0
```

"내림" 이라는 이름이 정확하다. `0x5000` ≤ x < `0x6000` 인 모든 x 가
`0x5000` 으로 떨어진다.

#### 8.3.5 정규화를 안 했을 때 무슨 일이 일어나는가

가상의 시나리오: 사용자 코드가 `0x5048` 을 읽었고 fault 가 났다.

**(a) `pg_round_down` 없이 그대로 조회:**

```
spt_find_page(spt, 0x5048)
   └─ temp.va = 0x5048
   └─ hash_find(...) 내부:
        page_less(temp, real)
            → 0x5048 < 0x5000 ?  → false
        page_less(real, temp)
            → 0x5000 < 0x5048 ?  → true
        ⇒ "같지 않다" → 다른 버킷 탐색 → 결국 NULL
```

fault handler 는 NULL 을 보고 **"SPT 에도 없는 잘못된 접근"** 이라고
판정해 프로세스를 죽인다. → **정상적인 lazy load 케이스가 seg fault 로
오인된다.** 페이지 안의 첫 바이트(`0x5000`)에 접근한 fault 만 우연히
통과하니, 사실상 거의 모든 lazy load 가 실패한다.

**(b) `pg_round_down` 으로 정규화한 뒤 조회:**

```
spt_find_page(spt, 0x5048)
   └─ temp.va = pg_round_down(0x5048) = 0x5000
   └─ hash_find(...) 내부:
        !(0x5000 < 0x5000) && !(0x5000 < 0x5000)  →  true
        ⇒ 매치 ✓
```

§6 에서 정리한 `!less && !less` 동등성 합성이 비로소 발동한다.

#### 8.3.6 같은 정규화가 곳곳에서 또 나온다

`pg_round_down` 은 이 함수만의 처방이 아니다. **페이지 단위 자료구조를
다루는 모든 입구에서 같은 정규화가 반복된다.**

| 함수 | 어디서 페이지 정규화가 필요한가 |
|---|---|
| `spt_find_page` | 오늘 본 그 자리 |
| `spt_remove_page` | 삭제 키도 정렬되어야 함 |
| `vm_try_handle_fault` | fault 주소 → SPT 조회 직전 |
| `install_page` (pml4) | pml4 매핑은 본질적으로 페이지 단위 |
| `mmap` / `munmap` | 사용자 인자를 페이지 경계로 둥글림 |

> **메타 교훈**: 가상메모리 코드 어디서든 va 를 키로 쓰기 직전이라면
> `pg_round_down` 을 의심해라. "왜 안 찾아지지?" 의 절반은 이 한 줄이
> 빠진 거다.

### 8.4 왜 임시 `struct page` 인가 — §6.2 의 정확한 적용

`hash_find` 는 `hash_elem*` 만 받는다. 그런데 호출 시점에 우리가 가진 건
va 하나뿐. 그래서:

> "키만 채운 가짜 page" 를 스택에 잠깐 만들고, 그 안의 `spt_elem` 주소를
> `hash_find` 에 넘긴다. 비교는 `page_less` 가 `va` 만 보기 때문에 가짜
> elem 이라도 문제없다.

§6.2 에서 그림으로 그렸던 *"포인터 주소는 다르지만 va 는 같다 → less
두 번이 동등 처리"* 가 바로 이 줄에서 동작한다. 임시 객체는 함수가 리턴
하면 스택과 함께 사라지므로 `malloc` / `free` 가 필요 없다.

### 8.5 `hash_entry` 로 진짜 page 로 복원 — 비대칭 흐름

```
            ┌────────── 입력 ──────────┐
            │  스택의 가짜 page         │
            │    └─ spt_elem (key only)│
            └──────────────┬───────────┘
                           │ hash_find
                           ▼
            ┌────────── 매치 ──────────┐
            │  SPT 안 진짜 hash_elem   │
            └──────────────┬───────────┘
                           │ hash_entry (= -offsetof)
                           ▼
                  진짜 struct page *
```

**들어갈 때는 임시 객체, 나올 때는 진짜 객체.** 이 비대칭이 이 함수의
묘미다. 같은 `hash_elem*` 타입이지만 전자는 스택에 잠시 살았다 사라지고,
후자는 SPT 가 들고 있는 영구 객체다.

### 8.6 NULL 처리 — fault handler 가 의존할 시맨틱

`hash_find` 가 `NULL` 을 돌려주면 그대로 `NULL` 을 반환한다. 이게 단순해
보이지만 의미가 크다. 다음 단계인 `vm_try_handle_fault` 는 이 NULL 을
보고 **"SPT 에도 없는 진짜 잘못된 접근"** 이라고 판단해 프로세스를 죽이게
된다. 즉 `spt_find_page` 의 반환값은 *"이건 lazy load 케이스인가, 진짜
seg fault 인가"* 를 가르는 첫 분기점이다.

---

## 9. 핵심 개념 정리

- **Lazy Loading**: 처음엔 종이에 "여기 페이지 있을 예정" 이라고만 적어둔다.
  실제로 누가 찾아오면 그제서야 진짜로 가져온다. Page fault 는 종업원을
  부르는 호출 벨이지 사고가 아니다.

- **SPT 가 pml4 와 별개로 필요한 이유**: pml4 는 "지금 책상 위에 무슨 책이
  있나" 만 안다. SPT 는 "내가 빌려둔 책 목록 전체" 를 안다. 빌려뒀지만
  아직 안 가져온 책도 SPT 에 있다.

- **uninit_new 가 함수 포인터를 저장만 하는 이유**: "그때 가서 부를 사람"
  의 명함만 받아두는 것. 미리 부르면 lazy 가 아니게 된다.

- **hash_entry / container_of**: 구조체 안에 박힌 멤버의 주소에서 멤버의
  오프셋을 빼면 바깥 구조체로 돌아온다. C 의 generic 컨테이너는 거의 다
  이 트릭을 쓴다.

- **less 만으로 동등성 만들기**: `!(a<b) && !(b<a)` 면 같다. 사용자는 less
  하나만 짜면 hash 가 알아서 동등성을 합성한다.

---

## 10. 다음에 할 일

`spt_find_page` 까지는 끝났다. 이제 page fault 를 **받아서** "lazy load 인가
seg fault 인가" 를 판단하고, 진짜로 frame 을 잡아 매핑하는 단계로 간다.

| 함수 | 해야 할 일 | 의존 |
|---|---|---|
| `vm_try_handle_fault` | fault 주소 → `spt_find_page` 조회 → `vm_do_claim_page` 호출 | 스택 확장 / read-only write / kernel 영역 접근 등 분기 필요 |
| `vm_do_claim_page` | frame 할당 → pml4 매핑 → `swap_in` 호출로 진짜 데이터 채움 | `vm_get_frame`, `pml4_set_page` |
| `swap_in / swap_out` | anon/file 별로 시작. swap disk 는 나중 단계 | `anon_initializer` 가 채워줄 ops 와 연결 |

흐름을 한 줄로 그리면:

```
fault → vm_try_handle_fault → spt_find_page ✓ → vm_do_claim_page
                                                       │
                                                       ├─ vm_get_frame
                                                       ├─ pml4_set_page
                                                       └─ swap_in (= uninit_initialize → page_initializer → init)
```

`vm_alloc_page_with_initializer` 가 저장해둔 함수 포인터들이 **바로 그
마지막 줄에서 처음 실행된다.** 오늘 만든 `spt_find_page` 는 이 흐름의
**첫 분기 — "SPT 에 있는가?"** 를 답하는 함수다. 있으면 lazy load 의 길로,
없으면 프로세스 종료의 길로 — 이 분기가 다음 작업의 출발점이다.

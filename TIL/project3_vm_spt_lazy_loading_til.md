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
> | §5 | hash_entry 매크로의 동작 원리 | container_of 패턴, offsetof 역산 |
> | §6 | hash_find 에서 less 를 두 번 쓰는 이유 | 동등성을 "둘 다 작지 않다" 로 표현 |
> | §7 | 다음에 할 일 | spt_find_page → fault handler → claim 까지 |

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

내가 헷갈렸던 부분: 왜 굳이 SPT 를 또 만드는가? pml4 가 이미 가상주소 →
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

해시테이블은 `hash_elem*` 만 보지만, 내가 비교하고 싶은 건 `page->va` 다.
`hash_entry` 가 그 다리를 놓아준다.

---

## 6. `hash_find` 에서 `less` 를 두 번 쓰는 이유

내가 처음에 헷갈렸던 부분: 비교 함수가 왜 `less` 만 있고 `equal` 은 없지?

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
→ **틀렸다.** `==` 는 **포인터 주소 비교**이고, 탐색할 때 내가 만든 임시
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

## 8. 핵심 개념 정리 (내 언어로)

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

## 9. 다음에 할 일

| 함수 | 해야 할 일 | 의존 |
|---|---|---|
| `spt_find_page` | `hash_find` 로 va 매칭 page 검색 | 임시 `struct page` 에 va 만 세팅 → spt_elem 으로 find → hash_entry 로 복원 |
| `vm_try_handle_fault` | fault 주소 → SPT 조회 → `vm_do_claim_page` 호출 | 유효성 검증 (스택 확장 / read-only write 등) 분기 필요 |
| `vm_do_claim_page` | frame 할당 → pml4 매핑 → `swap_in` 호출로 진짜 데이터 채움 | `vm_get_frame`, `pml4_set_page` |
| `swap_in / swap_out` | anon/file 별로 시작. swap disk 는 나중 단계 | `anon_initializer` 가 채워줄 ops 와 연결 |

흐름을 한 줄로 그리면:

```
fault → vm_try_handle_fault → spt_find_page → vm_do_claim_page
                                                    │
                                                    ├─ vm_get_frame
                                                    ├─ pml4_set_page
                                                    └─ swap_in (= uninit_initialize → page_initializer → init)
```

오늘 만든 `vm_alloc_page_with_initializer` 가 저장해둔 함수 포인터들이
**바로 그 마지막 줄에서 처음 실행된다.** 등록과 실행 사이의 시간차 ——
그게 이 프로젝트의 정신이다.

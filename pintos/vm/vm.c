/* vm.c: 가상 메모리 객체를 위한 일반 인터페이스. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"

/* 각 하위 시스템의 초기화 코드를 호출해서 가상 메모리 하위 시스템을
 * 초기화한다. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* 프로젝트 4용 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* 위쪽 줄은 수정하지 마세요. */
	/* TODO: 여기에 코드를 작성하세요. */
}

/* page의 타입을 얻는다. 이 함수는 page가 초기화된 뒤 어떤 타입이 될지
 * 알고 싶을 때 유용하다.
 * 이 함수는 이미 완전히 구현되어 있다. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* 보조 함수들 */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);
static bool page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux); 
static uint64_t spt_hash (const struct hash_elem *e, void *aux);

/* initializer를 포함한 대기 상태의 page 객체를 만든다. page를 만들고
 * 싶다면 직접 만들지 말고 이 함수나 `vm_alloc_page`를 통해 만들어라. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT);

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* upage가 이미 사용 중인지 확인한다. */
	bool (*page_initializer)(struct page *, enum vm_type, void *); //page fault시 호출할 page 종류별 initializer를 담을 함수 포인터
	if(spt_find_page (spt, upage) == NULL){ //
		struct page *page = malloc(sizeof(struct page)); // page 정보를 담을 struct page 할당
		if(page == NULL){  //할당에 실패하면 page 등록 도전도 못함
			return false;
		}
		if(VM_TYPE(type) == VM_ANON){  //anonymous page라면 
			page_initializer = anon_initializer;	
		}
        else if(VM_TYPE(type) == VM_FILE){ //file-backed page라면
			page_initializer = file_backed_initializer;
		}
		else{   //그 외라면 할당한 page 해제시키고 실패처리
			free(page);
			return false; 
		}

		uninit_new(page, upage, init, type, aux, page_initializer); //page를 uninit상태로 만들어주고 초기 정보 저장(첫 page fault시 사용)
		page->writable = writable;

		if(spt_insert_page(spt, page)){  //완성된 page 정보 spt에 삽입
			return true;
		}
		else{  //실패하면 역시 할당 해제하고 실패처리
			free(page);
			return false;
		}
		
	}
err:
	return false;
}

/* spt에서 VA에 해당하는 page를 찾아 반환한다. 오류가 있으면 NULL을 반환한다. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	void *page_va=pg_round_down(va); // 가상 주소를 page의 시작 가상 주소로 내린다.
	struct page temp_page; //hash table에서 검색할 대 사용할 임시 page 구조체
	temp_page.va=page_va; //va만 임시 page에 넣어두기
	struct hash_elem *found = hash_find(&spt->page_spt, &temp_page.spt_elem); //spt hash table에 같은 va의 page가 있는지 검사
	if(found == NULL){  //이 주소에 대한 page 정보가 spt에 등록되어 있지 않다는 뜻
		return NULL;
	}
	return hash_entry(found, struct page, spt_elem); //아니라면 찾아서 struct page를 반환
}

/* 검증을 거쳐 PAGE를 spt에 삽입한다. */
bool
spt_insert_page (struct supplemental_page_table *spt,
	struct page *page) {
	struct hash_elem *ins_result;
    ins_result = hash_insert(&spt->page_spt, &page->spt_elem); //page의 hase_elem을 spt hash table에 삽입
	if(ins_result == NULL){ //중복 없이 삽입에 성공
		return true;
	}

	return false; //이미 같은 va의 page가 있어서 삽입 실패
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	struct hash_elem *del;
	del = hash_delete(&spt->page_spt, &page->spt_elem); //hash table에서 원소값을 제거하고 그 제거한 주소를 반환
	if(del != NULL){     //즉 제거를 했다면 제거된 주소를 가지고 있을 것
	    vm_dealloc_page (page); //page도 제거
	}
}

/* 쫓아낼 struct frame을 얻는다. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: eviction 정책은 직접 정한다. */

	return victim;
}

/* page 하나를 쫓아내고 그에 대응하는 frame을 반환한다.
 * 오류가 있으면 NULL을 반환한다. */
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: victim을 swap out하고 쫓겨난 frame을 반환한다. 
	return NULL;


/* palloc()으로 frame을 얻는다. 사용할 수 있는 page가 없으면 page를
 * 쫓아낸 뒤 반환한다. 이 함수는 항상 유효한 주소를 반환한다. 즉 user pool
 * 메모리가 가득 찬 경우, 사용 가능한 메모리 공간을 얻기 위해 frame을
 * 쫓아낸다. */
}
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: 이 함수를 채우세요. */
	void *kva;  //반환된 kva을 담을 포인터 변수
	kva = palloc_get_page(PAL_USER); // frame을 확보하고 그 frame에 커널이 접근할 수 있는 주소를 반환
	if(kva == NULL){ //주소가 실제로 존재하는지 검사
		return NULL;
	}

	frame = malloc(sizeof(struct frame));  //frame 안에 메타데이터를 넣을 공간 할당
	if(frame == NULL){
		palloc_free_page(kva);
		return NULL;
	}
	else if(frame != NULL){
		frame->kva = kva;
		frame->page = NULL;
		return frame;
	}
	return NULL;

// 	ASSERT (frame != NULL);
// 	ASSERT (frame->page == NULL);
// 	return frame;
}

/* stack을 증가시킨다. */
static void
vm_stack_growth (void *addr) {
	void *stack_page;
	bool alloc_success ;
	stack_page = pg_round_down(addr);
	alloc_success = vm_alloc_page(VM_ANON, stack_page, true);
	if(alloc_success == false){
		return;
	}
	else{
		vm_claim_page(stack_page);
		return;
	}
}

/* write-protected page에서 발생한 fault를 처리한다. */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* 성공하면 true를 반환한다. */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr,
		bool user, bool write, bool not_present) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = NULL;
	if(addr == NULL){  //주소가 존재하는지 확인
		return false;
	}
	if(is_kernel_vaddr(addr)){  //그 주소가 user가 아닌 kernel 영역 주소인지 확인
		return false;
	}
	page = spt_find_page(spt, addr); //spt에 등록된 주소가 있는지 확인
	if(page == NULL){
		if((uintptr_t)addr < (uintptr_t)USER_STACK && (uintptr_t)addr >= ((uintptr_t)USER_STACK - (1024*1024)) && (uintptr_t)addr >= ((uintptr_t)f->rsp - 8)){
			vm_stack_growth(addr);
			return true;
		}
		return false;
	}
	if(write == true && page->writable == false){  //쓰기 권한이 false인데 쓰기를 요청했는지
		return false;
	}

	return vm_do_claim_page (page);
}

/* page를 해제한다.
 * 이 함수는 수정하지 마세요. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* VA에 할당된 page를 claim한다. */
bool
vm_claim_page (void *va) { //va에 해당하는 page를 찾고 그 page가 spt에 등록되어 있는지 확인하는 함수
	struct supplemental_page_table *cur_spt; //현재 실행중인 thread의 spt정보를 담아두는 포인터 변수
	struct page *cur_page; // 현재 실행준인 thread page의 가상 주소를 담아두는 포인터 변수
	cur_spt = &thread_current()->spt; //현재 thread의 spt를 가져온다
	cur_page = spt_find_page(cur_spt,va); // va에 해당하는 page를 찾는다
	if(cur_page == NULL){  //찾지 못했다
		return false;
	}
	else if(cur_page != NULL){  //찾았다
	    return vm_do_claim_page(cur_page);
    }
	return false;
}

/* PAGE를 claim하고 mmu를 설정한다. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();  //물리 frame하나를 확보
	if(frame == NULL){ //frame 할당 확인
		return false;
	}
	frame->page = page;  //페이지 연결 작업
	page->frame = frame;
	if(!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)){ //page->va와 frame->kva매핑
        return false;
	}
	else{
		return swap_in (page, frame->kva);
	}
}
static uint64_t spt_hash(const struct hash_elem *e, void *aux){
	uint64_t hash_value=0;
	struct page *page = hash_entry(e, struct page, spt_elem); //hash_elem을 이용하여 원래 struct page 받아오기
	hash_value = hash_bytes(&page->va, sizeof(page->va)); //page의 시작 가상주소 va를 기준으로 hash값 만들기
	return hash_value; // 계산된 hash값 반환
}
static bool page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux){
	struct page *pa = hash_entry(a, struct page, spt_elem); // a의 hash_elem을 이용해서 struct page 받아오기
	struct page *pb = hash_entry(b, struct page, spt_elem); // b의 hash_elem을 이용해서 struct page 받아오기
	return pa->va < pb->va;  //hash table에서 hash collision을 정렬하기 위해 사용(오름차순 정렬)
}
/* 새 supplemental page table을 초기화한다. */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {

	hash_init(&spt->page_spt, spt_hash, page_less, NULL); //page_spt hash table을 초기화
}

/* supplemental page table을 src에서 dst로 복사한다. */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* supplemental page table이 보유한 자원을 해제한다. */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: thread가 보유한 모든 supplemental_page_table을 파괴하고,
	 * TODO: 수정된 모든 내용을 저장소에 writeback한다. */
}

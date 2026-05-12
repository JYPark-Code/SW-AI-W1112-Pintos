/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

// va를 해시값으로 변환
static uint64_t page_hash(const struct hash_elem *e, void *aux UNUSED){
	struct page *p = hash_entry(e, struct page, spt_elem);
    return hash_bytes(&p->va, sizeof(p->va));
}
// va 기준으로 크기 비교
static bool page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED){
	struct page *pa = hash_entry(a, struct page, spt_elem);
    struct page *pb = hash_entry(b, struct page, spt_elem);
    return pa->va < pb->va;
}


/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
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

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = (struct page *)malloc(sizeof(struct page));

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		
		if (page == NULL){
			goto err;
		}

		bool (*page_initializer)(struct page *, enum vm_type, void *); /* 함수 포인터 선언 */
		switch (VM_TYPE(type))
		{
		case VM_ANON:
			page_initializer = anon_initializer;
			break;

		case VM_FILE:
			page_initializer = file_backed_initializer;
			break;
		
		default:
			break;
		}
		
		uninit_new(page, upage, init, type, aux, page_initializer);
		page->writable = writable;
		/* TODO: Insert the page into the spt. */
		if (!spt_insert_page(spt, page))
    		goto err;
		return true;
		
	}
err:
	free(page);
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function. */
	struct page temp;
	temp.va = pg_round_down(va);

	struct hash_elem *e = hash_find(&spt->pages, &temp.spt_elem);
	if (e == NULL)
		return NULL;

	page = hash_entry(e, struct page, spt_elem);

	return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	int succ = false;
	/* TODO: Fill this function. */
	if (hash_insert(&spt->pages, &page->spt_elem) == NULL){
		succ = true;
	}
	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = malloc(sizeof(struct frame));
	/* TODO: Fill this function. */
	frame->kva = palloc_get_page(PAL_USER);
	
	if (frame->kva == NULL)
    	PANIC("todo: swap out");

	frame->page = NULL;

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack.
* addr을 포함하는 페이지를 새로 할당하여 스택을 확장한다.
* VM_MARKER_0으로 스택 페이지임을 표시하고,
* pg_round_down으로 페이지 경계에 맞춰 할당한다. */
static void
vm_stack_growth (void *addr UNUSED) {
	vm_alloc_page(VM_ANON | VM_MARKER_0, pg_round_down(addr), true);
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	/* 
	1. addr이 유효한 주소인지 확인 (NULL, 커널 주소)
	2. SPT에서 page 찾기
	3. page 없으면 → 스택 확장 가능한지 판단
	4. page 있으면 → vm_do_claim_page()
	*/
	
	
	// 1. 유효하지 않은 주소 체크
	if (addr == NULL || is_kernel_vaddr(addr)) {
		return false;
	}

	// 2. protection fault는 처리 안 함 (읽기전용 페이지에 쓰기)
    if (!not_present)
        return false;

	// 3. rsp 타입 맞추기
	void *rsp = (void *)f->rsp;
	
	// 4. SPT에서 page 찾기
	page = spt_find_page(spt, addr);

	// 5. page 없으면 스택 확장 가능한지 판단
	if(page == NULL && addr >= (void *)((uintptr_t)rsp - 8)){
		vm_stack_growth(addr);
		return true;
	}
	
	if (page == NULL){
		return false;
	}
	
	return vm_do_claim_page (page);


}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function */
	struct supplemental_page_table *spt = &thread_current ()->spt;

	page = spt_find_page(spt, va);

	if (page == NULL){
		return false;
	}

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	if(!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)){
		return false;
	}
	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init(&spt->pages, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}

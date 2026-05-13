/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"

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

/* SPT hash table의 key인 page->va로 hash 값을 계산한다. */
static unsigned
page_hash (const struct hash_elem *e, void *aux UNUSED)
{
	/* hash_elem을 포함하고 있는 struct page를 꺼낸다. */
	const struct page *p = hash_entry (e, struct page, hash_elem);

	/* page의 시작 가상 주소를 기준으로 hash 값을 만든다. */
	return hash_bytes (&p->va, sizeof p->va);
}

/* SPT hash table 안에서 두 page를 가상 주소 기준으로 비교한다. */
static bool
page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
	/* 첫 번째 hash_elem이 들어 있는 struct page를 꺼낸다. */
	const struct page *pa = hash_entry (a, struct page, hash_elem);
	/* 두 번째 hash_elem이 들어 있는 struct page를 꺼낸다. */
	const struct page *pb = hash_entry (b, struct page, hash_elem);

	/* page의 시작 가상 주소가 더 작은 page를 앞에 둔다. */
	return pa->va < pb->va;
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* SPT hash table에서 page->va를 기준으로 hash 값을 만든다. */
static unsigned page_hash (const struct hash_elem *e, void *aux);

/* SPT hash table에서 page->va를 기준으로 page들을 비교한다. */
static bool page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		/* TODO: Insert the page into the spt. */
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
/*  SPT에서 특정 viruatl address에 해당하는 page를 찾는다. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	/* 검색 key로 사용할 임시 page */
	struct page page;
	/* TODO: Fill this function. */
	/* hash_find 결과를 받을 hash_elem */
	struct hash_elem *e;

	/* 주소를 page 시작 주소로 맞춘다. */
	page.va = pg_round_down (va);

	/* 같은 va를 가진 page를 찾는다. */
	e = hash_find (&spt->pages, &page.hash_elem);

	//return page;
	return e != NULL ? hash_entry (e, struct page, hash_elem) : NULL;
}

/* Insert PAGE into spt with validation. */
/* 새 page를 SPT hash table에 등록한다. */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	//int succ = false;
	/* TODO: Fill this function. */

	//return succ;
	/* page->hash_elem을 SPT hash table에 넣는다. 중복 va가 없으면 성공한다. */
	return hash_insert (&spt->pages, &page->hash_elem) == NULL;
}

/* page를 SPT에서 제거하고 page 자원을 해제한다. */
void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	/* SPT hash table에서 page를 제거한다. */
	hash_delete (&spt->pages, &page->hash_elem);

	/* page 종류별 자원을 정리하고 struct page 메모리를 해제한다. */
	vm_dealloc_page (page);
	//return true;
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
	struct frame *frame = NULL;
	/* TODO: Fill this function. */

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
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

	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
/* 새 프로세스의 SPT를 빈 hash table로 초기화한다. */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	/* page->va 기준 hash/비교 함수를 사용해 SPT hash table을 초기화한다. */
	hash_init (&spt->pages, page_hash, page_less, NULL);
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

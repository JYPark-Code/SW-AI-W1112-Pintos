/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

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

/* Create the pending page object with initializer. If you want to create a page, do not create it directly and make it through this function or `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {
	/* 이 함수에는 최종 page 종류가 들어와야 하므로 VM_UNINIT이면 안 된다. */
	ASSERT (VM_TYPE(type) != VM_UNINIT);

	/* 현재 thread, 즉 현재 프로세스의 SPT를 가져온다. */
	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* VM_UNINIT page가 나중에 실제 page로 바뀔 때 사용할 초기화 함수를 저장할 변수 */
	bool (*initializer) (struct page *, enum vm_type, void *) = NULL;

	/* SPT는 page 단위로 관리하므로, 주소를 page 시작 주소로 맞춘다. */
	upage = pg_round_down (upage);

	/* Check wheter the upage is already occupied or not. */
	/* 해당 가상 주소의 page가 아직 SPT에 등록되어 있지 않은 경우에만 새로 만든다. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		/* SPT에 저장할 struct page 공간을 동적으로 할당 */
		struct page *page = malloc (sizeof *page);

		/* page 구조체 할당에 실패하면 실패 처리로 이동 */
		if (page == NULL) {
			goto err;
		}

		/* 최종 page 종류에 따라 나중에 사용할 initializer를 선택 */
		switch (VM_TYPE (type))
		{
			/* anonymous page라면 anon_initializer를 사용 */
			case VM_ANON:
				initializer = anon_initializer;
				break;
			/* file page라면 file_backed_intioalizer를 사용 */
			case VM_FILE:
				initializer = file_backed_initializer;
				break;
			/* 처리할 수 없는 page type이면 할당한 page를 해제하고 실패 */
			default:
				free (page);
				goto err;
		}
		/* page를 아직 로드되지 않은 VM_UNINIT page로 초기화 */
		uninit_new (page, upage, init, type, aux, initializer);

		/* 나중에 PML4에 연결할 때 사용할 writable 정보를 page에 저장 */
		page->writable = writable;

		/* TODO: Insert the page into the spt. */
		/* 완성된 VM_UNINIT page를 현재 프로세스의 SPT에 등록 */
		if (spt_insert_page (spt, page)) {
			return true;
		}
		/* SPT 등록에 실패했으므로 할당했던 pagep 구조체를 해제 */
		free (page);
	}
err:
	/* 중복 page가 있거나, 할당/초기화/삽입 중 실패하면 false 반환*/
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
/* user page를 올릴 physiccal frame 하나를 확보해서 struct frame으로 반환하는 함수 */
static struct frame *
vm_get_frame (void) {
	/* frame 정보를 담을 구조체를 동적으로 할당 */
	struct frame *frame = malloc (sizeof *frame);
	/* TODO: Fill this function. */

	/* frame 구조체 할당에 실패하면 frame을 얻을 수 없다. */
	if (frame == NULL) {
		return NULL;
	}

	/* user pool에서 실제 RAM frame으로 사용할 page를 하나 얻는다. */
	frame->kva = palloc_get_page (PAL_USER | PAL_ZERO);

	/* user pool에 남은 frame이 없으면 eviction으로 frame을 확보해야 함 */
	if (frame->kva == NULL)
	{
		/* 방금 만든 frame 구조체는 사용할 수 없으므로 해제 */
		free (frame);

		/* 기존 frame 하나를 내보내고 빈 frame을 얻는다. */
		frame = vm_evict_frame ();

		/* eviction도 실패하면 frame 확보 실패 */
		if (frame == NULL) {
			return NULL;
		}
	}
	else 
	{
		/* 새 frame은 아직 어떤 page와도 연결되지 않았따. */
		frame->page = NULL;
	}

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
	/* 현재 thread, 즉 현재 프로세스의 SPT를 가져온다. */
	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* SPT에서 찾은 page를 저장할 변수 */
	struct page *page = NULL;
	/* TODO: Fill this function */
	/* SPT는 page 단위로 관리하므로 주소를 page 시작 주소로 맞춘다. */
	va = pg_round_down (va);

	/* SPT에서 해당 가상 주소의 page를 찾는다. */
	page = spt_find_page (spt, va);

	/* SPT에 등록된 page가 없으면 claim할 수 없다. */
	if (page == NULL) {
		return false;
	}

	/* 찾은 page를 실제 frame에 올리고 PML4에 연결 */
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	/* page를 올릴 실제 RAM frame을 하나 얻는다. */
	struct frame *frame = vm_get_frame ();

	/* frame을 얻지 못하면 claim에 실패 */
	if (frame == NULL) {
		return false;
	}

	/* Set links */
	/* frame이 어떤 page를 담고 있는지 연결 */
	frame->page = page;
	/* page가 어떤 frame에 올라갔는지 연결 */
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	/* page의 가상 주소와 frame의 실제 메모리를 PML4에 연결 */
	if (!pml4_set_page (thread_current ()->pml4, page->va, frame->kva, page->writable))
	{
		/* PML4 연결에 실패했으므로 frame과 page의 연결 되돌리기 */
		frame->page = NULL;
		/* PML4 연결에 실패했으므로 page와 frame의 연결 되돌리기 */
		page->frame = NULL;

		/* 할당했던 실제 RAM frame 반납 */
		palloc_free_page (frame->kva);

		/* frame 관리 구조체 해제 */
		free (frame);

		/* claim 실패 */
		return false;
	}

	/* page 내용을 frame에 채운다. VM_UNINIT이면 lazy loading이 실행 */
	if (!swap_in (page, frame->kva))
	{
		/* swap_in 실패 시 PML4에 등록했던 매핑 제거 */
		pml4_clear_page (thread_current ()->pml4, page->va);

		/* 실패했으므로 frame과 page의 연결 되돌리기 */
		frame->page = NULL;
		/* 실패했으므로 page와 frame의 연결 되돌리기 */
		page->frame = NULL;

		/* 할당했던 실제 RAM frame 반납 */
		palloc_free_page (frame->kva);

		/* frame 관리 구조제 해제 */
		free (frame);

		/* claim 실패 */
		return false;
	}
	/* page를 frame에 올리고 PML4 연결하는 데 성공*/
	return true;
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

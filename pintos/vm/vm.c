/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

#include <string.h>
#include "filesys/file.h"

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
static uint64_t
page_hash (const struct hash_elem *e, void *aux UNUSED)
{
	const struct page *p = hash_entry (e, struct page, hash_elem);

	return hash_bytes (&p->va, sizeof p->va);
}

/* SPT hash table 안에서 두 page를 가상 주소 기준으로 비교한다. */
static bool
page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
	const struct page *pa = hash_entry (a, struct page, hash_elem);
	const struct page *pb = hash_entry (b, struct page, hash_elem);

	return pa->va < pb->va;
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

static uint64_t page_hash (const struct hash_elem *e, void *aux);
static bool page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux);
static void spt_destroy_page (struct hash_elem *e, void *aux);

/* Create the pending page object with initializer. If you want to create a page, do not create it directly and make it through this function or `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {
	ASSERT (VM_TYPE(type) != VM_UNINIT);
	struct supplemental_page_table *spt = &thread_current ()->spt;
	bool (*initializer) (struct page *, enum vm_type, void *) = NULL;

	upage = pg_round_down (upage);

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		struct page *page = malloc (sizeof *page);

		if (page == NULL) {
			goto err;
		}


		switch (VM_TYPE (type))
		{
			case VM_ANON:
				initializer = anon_initializer;
				break;
			case VM_FILE:
				initializer = file_backed_initializer;
				break;
			default:
				free (page);
				goto err;
		}
		uninit_new (page, upage, init, type, aux, initializer);

		page->writable = writable;

		/* TODO: Insert the page into the spt. */
		if (spt_insert_page (spt, page)) {
			return true;
		}
		free (page);
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
/*  SPT에서 특정 viruatl address에 해당하는 page를 찾는다. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	struct page page;
	struct hash_elem *e;

	page.va = pg_round_down (va);

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
	return hash_insert (&spt->pages, &page->hash_elem) == NULL;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	hash_delete (&spt->pages, &page->hash_elem);

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
	struct frame *frame = malloc (sizeof *frame);
	/* TODO: Fill this function. */

	if (frame == NULL) {
		return NULL;
	}

	frame->kva = palloc_get_page (PAL_USER | PAL_ZERO);

	if (frame->kva == NULL)
	{
		free (frame);

		frame = vm_evict_frame ();

		if (frame == NULL) {
			return NULL;
		}
	}
	else
	{
		frame->page = NULL;
	}

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
/* fault 주소를 기준으로 새 stack page를 만들고 실제 frame에 올린다. */
static bool
vm_stack_growth (void *addr) {
	void *stack_bottom = pg_round_down (addr);

	return vm_alloc_page (VM_ANON | VM_MARKER_0, stack_bottom, true)
		&& vm_claim_page (stack_bottom);
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
	return false;
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	uint64_t rsp;
	uint64_t stack_limit = (uint64_t) USER_STACK - (1 << 20);
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	if (addr == NULL || is_kernel_vaddr (addr)) {
		return false;
	}

	rsp = user ? f->rsp : thread_current ()->user_rsp;

	if (!not_present) {
		return false;
	}

	page = spt_find_page (spt, addr);

	if (page == NULL) {
		uint64_t fault_addr = (uint64_t) addr;
		if (fault_addr >= rsp - 8
				&& fault_addr >= stack_limit
				&& fault_addr < (uint64_t) USER_STACK) {
			return vm_stack_growth (addr);
		}
		return false;
	}

	if (write && !page->writable) {
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
	struct supplemental_page_table *spt = &thread_current ()->spt;

	struct page *page = NULL;
	/* TODO: Fill this function */
	va = pg_round_down (va);

	page = spt_find_page (spt, va);

	if (page == NULL) {
		return false;
	}

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	if (frame == NULL) {
		return false;
	}

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	if (!pml4_set_page (thread_current ()->pml4, page->va, frame->kva, page->writable))
	{
		frame->page = NULL;
		page->frame = NULL;

		palloc_free_page (frame->kva);

		free (frame);

		return false;
	}

	if (!swap_in (page, frame->kva))
	{
		pml4_clear_page (thread_current ()->pml4, page->va);

		frame->page = NULL;
		page->frame = NULL;

		palloc_free_page (frame->kva);
		free (frame);

		return false;
	}
	return true;
}

/* Initialize new supplemental page table */
/* 새 프로세스의 SPT를 빈 hash table로 초기화한다. */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init (&spt->pages, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
/* 부모 SPT(src)의 page들을 자식 SPT(dst)로 복사 */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src) {
	struct hash_iterator i;

	hash_first (&i, &src->pages);

	while (hash_next (&i) != NULL) {
		struct page *src_page = hash_entry (hash_cur (&i), struct page, hash_elem);

		enum vm_type type = page_get_type (src_page);

		if (src_page->operations->type == VM_UNINIT)
		{
			void *aux = NULL;

			if (src_page->uninit.aux != NULL)
			{
				struct lazy_load_arg *src_aux = src_page->uninit.aux;
				struct lazy_load_arg *dst_aux = malloc (sizeof *dst_aux);

				if (dst_aux == NULL)
				{
					return false;
				}

				dst_aux->file = file_reopen (src_aux->file);

				if (dst_aux->file == NULL)
				{
					free (dst_aux);
					return false;
				}

				dst_aux->ofs = src_aux->ofs;
				dst_aux->read_bytes = src_aux->read_bytes;
				dst_aux->zero_bytes = src_aux->zero_bytes;
				dst_aux->mmap_page_cnt = src_aux->mmap_page_cnt;
				aux = dst_aux;
			}

			if (!vm_alloc_page_with_initializer (src_page->uninit.type, src_page->va,
						src_page->writable, src_page->uninit.init, aux))
			{
				if (aux != NULL)
				{
					struct lazy_load_arg *dst_aux = aux;

					file_close (dst_aux->file);
					free (dst_aux);
				}
				return false;
			}
		}
		else
		{
			if (!vm_alloc_page (type, src_page->va, src_page->writable))
				return false;

			if (!vm_claim_page (src_page->va))
				return false;

			struct page *dst_page = spt_find_page (dst, src_page->va);

			if (dst_page == NULL || dst_page->frame == NULL || src_page->frame == NULL)
				return false;

			memcpy (dst_page->frame->kva, src_page->frame->kva, PGSIZE);
		}
	}
	return true;
}

/* SPT hash table 안의 page 하나를 정리 */
static void
spt_destroy_page (struct hash_elem *e, void *aux UNUSED)
{
	struct page *page = hash_entry (e, struct page, hash_elem);
	vm_dealloc_page (page);
}

/* Free the resource hold by the supplemental page table */
/* 프로세스 종료 시 SPT가 가지고 있는 모든 page를 정리 */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	hash_destroy (&spt->pages, spt_destroy_page);
}

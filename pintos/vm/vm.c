/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void)
{
	vm_anon_init();
	vm_file_init();
#ifdef EFILESYS /* For project 4 */
	pagecache_init();
#endif
	register_inspect_intr();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type(struct page *page)
{
	int ty = VM_TYPE(page->operations->type);
	switch (ty)
	{
	case VM_UNINIT:
		return VM_TYPE(page->uninit.type);
	default:
		return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

// SPT init helper
static uint64_t hash_page_address(const struct hash_elem *e, void *aux);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
									vm_initializer *init, void *aux)
{

	ASSERT(VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page(spt, upage) == NULL)
	{
		// 1. 페이지 생성 후, VM 타입에 맞는 initializer 가져오기
		struct page *page = malloc(sizeof *page);
		if (page == NULL)
			goto err;

		bool (*initializer)(struct page *page, enum vm_type type, void *kva);
		switch (VM_TYPE(type))
		{
		case VM_ANON:
			initializer = anon_initializer;
			break;
		case VM_FILE:
			initializer = file_backed_initializer;
			break;
		default:
			goto err;
		}
		// 2. uninit_new 호출해서 "uninit" 페이지 구조체 생성
		uninit_new(page, upage, init, type, aux, initializer);

		// 3. page 쓰기 권한 지정
		page->writable = writable;

		// 4. 페이지를 spt에 삽입.
		// 삽입 결과를 return
		bool insert_result = spt_insert_page(spt, page);
		return insert_result;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page(struct supplemental_page_table *spt, void *va)
{
	struct page temp_page;
	struct page *return_page = NULL;
	/* TODO: Fill this function. */
	// 가상 주소 정렬
	temp_page.va = pg_round_down(va);
	// 해당 주소가 존재하는지를 spt->hash_table에서 탐색
	struct hash_elem *el = hash_find(&spt->hash_table, &temp_page.hash_elem);
	if (el != NULL)
		return_page = hash_entry(el, struct page, hash_elem);
	return return_page;
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table *spt,
					 struct page *page)
{
	struct hash_elem *insert_result;
	int succ = false;
	/* TODO: Fill this function. */
	insert_result = hash_insert(&spt->hash_table, &page->hash_elem);
	if (insert_result == NULL)
		succ = true;
	return succ;
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page)
{
	// 해시 테이블에서 delete
	hash_delete(&spt->hash_table, &page->hash_elem);
	vm_dealloc_page(page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim(void)
{
	struct frame *victim = NULL;
	/* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame(void)
{
	struct frame *victim UNUSED = vm_get_victim();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc()으로 frame을 가져온다.
 * 사용 가능한 페이지가 없다면, 어떤 페이지를 eviction(축출)한 뒤 반환한다.
 *
 * 이 함수는 항상 유효한 주소를 반환해야 한다.
 * 즉, user pool 메모리가 가득 찼다면,
 * 사용 가능한 메모리 공간을 확보하기 위해 frame을 eviction 해야 한다.
 */
static struct frame *
vm_get_frame(void)
{
	/* TODO: Fill this function. */
	// 1. frame 객체를 동적 할당
	// 실제 물리 메모리 페이지를 할당 후, 커널 가상 주소를 kva에 저장
	struct frame *frame = malloc(sizeof *frame);
	frame->kva = palloc_get_page(PAL_USER);
	if (frame->kva == NULL)
	{
		// eviction 로직(아직 미구현)
		free(frame);
		kill();
	}
	// 2. 빈 프레임을 할당하는 것이므로, page는 NULL로 초기화
	// 실제 요청(vm_do_claim_page)에서 실제로 연결한다.
	frame->page = NULL;
	// 3. 할당한 frame return
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth(void *addr UNUSED)
{
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp(struct page *page UNUSED)
{
	// 1. read-only 페이지에 대해서 write 접근했을때 -> 이건 죽여야 됨
	// 2. COW
}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f, void *addr,
						 bool user, bool write, bool not_present)
{
	// 주소가 NULL이거나, 커널 가상 주소라면 kill
	if (addr == NULL || is_user_vaddr(addr) == false)
	{
		return false;
	}

	struct supplemental_page_table *spt = &thread_current()->spt;
	struct page *page = NULL;

	// CASE1: not_present == true (페이지가 없을 때 or present == 0)
	if (not_present == true)
	{
		// SPT 검사
		page = spt_find_page(spt, addr);
		// 1) SPT에 존재 O -> Lazy loading / swap in
		if (page != NULL)
		{
			return vm_do_claim_page(page);
		}
		// 2) SPT에 존재 X -> stack growth 가능하면 수행, 아니면 kill
		else
		{
			// stack growth 조건 판단
			// if 맞으면 -> stack_growth + spt_find_page + return vm_do_claim_page(page);
			// 아니면 return false
		}
	}

	// CASE2: not_present == false (페이지가 존재)
	if (not_present == false)
	{
		// 1) write = true -> vm_handle_wp(page) 호출
		if (write == true)
			return vm_handle_wp(page);
		// 2) write = false, user = true -> 유저 모드에서 읽기도 실패 -> 커널 주소 읽기 시도 -> kill
		// 사실 2번 문제는 이미 위에서 걸러진다.
		// 3) write = false/true, user = false -> 커널 모드에서 읽기/쓰기 실패 -> 커널 권한 문제 -> kill
		else
			return false;
	}
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page)
{
	destroy(page);
	free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void *va)
{
	struct page *page = NULL;
	/* TODO: Fill this function */
	// SPT에서 va에 해당하는 page 찾기
	page = spt_find_page(&thread_current()->spt, va);
	return vm_do_claim_page(page);
}

/* 페이지를 요청하고, 페이지 테이블에 등록한다. */
static bool
vm_do_claim_page(struct page *page)
{
	struct frame *frame = vm_get_frame();

	/* Set links */
	// frame 객체의 page는 NULL이었음
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable);

	return swap_in(page, frame->kva);
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt)
{
	hash_init(&spt->hash_table, hash_page_address, compare_page_less, NULL);
}

static uint64_t hash_page_address(const struct hash_elem *e, void *aux)
{
	struct page *page = hash_entry(e, struct page, hash_elem);
	return hash_bytes(&page->va, sizeof page->va);
}

static bool compare_page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux)
{
	struct page *page_a = hash_entry(a, struct page, hash_elem);
	struct page *page_b = hash_entry(b, struct page, hash_elem);
	return page_a->va < page_b->va;
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED,
								  struct supplemental_page_table *src UNUSED)
{
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED)
{
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}

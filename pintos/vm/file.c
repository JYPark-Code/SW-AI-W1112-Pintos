/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "userprog/process.h"

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void)
{
}

/* Initialize the file backed page */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva)
{
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in(struct page *page, void *kva)
{
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out(struct page *page)
{
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy(struct page *page)
{
	struct file_page *file_page UNUSED = &page->file;
}

/* Do the mmap */
void *
do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset)
{
	// file이 close 되어도 사용할 수 있도록 파일 구조체 선언
	struct file *mmap_file;
	// 페이지에 등록할 바이트 수
	size_t page_read_bytes, page_zero_bytes;
	// 시작 주소
	void *start_addr = addr;

	// 1. 실패 조건
	if (addr == NULL || addr == 0 || length == 0 || addr != pg_round_down(addr) || offset != pg_round_down(offset))
		return NULL;

	// 2. file이 close 되어도 사용할 수 있도록 reopen
	if (file != NULL)
	{
		mmap_file = file_reopen(file);
	}
	else
		return NULL;

	// 3. length만큼 페이지 단위로 반복
	// 각 페이지를 spt에 file-backed page로 등록
	while (length > 0)
	{
		page_read_bytes = length < PGSIZE ? length : PGSIZE;
		page_zero_bytes = PGSIZE - page_read_bytes;

		struct lazy_load_info *aux = malloc(sizeof *aux);
		aux->file = mmap_file;
		aux->ofs = offset;
		aux->page_read_bytes = page_read_bytes;
		aux->page_zero_bytes = page_zero_bytes;

		if (!vm_alloc_page_with_initializer(VM_FILE, addr, writable, lazy_load_segment, aux))
		{
			free(aux);
			return NULL;
		};

		length -= page_read_bytes;
		offset += page_read_bytes;
		addr += PGSIZE;
	}
	// 4. 성공시 시작 addr 반환
	return start_addr;
}

/* Do the munmap */
void do_munmap(void *addr)
{
	// 1. SPT에서 제거(mmap 영역 전체 page에 대해)
	// 2. 페이지 테이블(PML4) 매핑 제거
	// 3.
}
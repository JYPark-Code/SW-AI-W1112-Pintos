/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include <string.h>
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/thread.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);
static bool lazy_load_mmap (struct page *page, void *aux);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type UNUSED, void *kva UNUSED) {
	/* Set up the handler */
	/* file-backed page가 사용할 함수 묶음을 연결 */
	page->operations = &file_ops;

	/* file-backed page 전용 데이터 영역을 가리킴. 현재는 추가 초기화가 없다. */
	struct file_page *file_page = &page->file;
	file_page->file = NULL;
	file_page->ofs = 0;
	file_page->read_bytes = 0;
	file_page->zero_bytes = 0;
	file_page->mmap_page_cnt = 0;

	/* file-backed page 초기화가 성공했음을 알림 */
	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;

	if (file_page->file == NULL)
		return false;

	if (file_read_at (file_page->file, kva, file_page->read_bytes,
				file_page->ofs) != (int) file_page->read_bytes)
		return false;

	memset ((uint8_t *) kva + file_page->read_bytes, 0, file_page->zero_bytes);
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;

	if (page->frame == NULL || file_page->file == NULL)
		return true;

	if (pml4_is_dirty (thread_current ()->pml4, page->va)) {
		if (file_write_at (file_page->file, page->frame->kva,
					file_page->read_bytes, file_page->ofs)
				!= (int) file_page->read_bytes)
			return false;
		pml4_set_dirty (thread_current ()->pml4, page->va, false);
	}

	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;

	file_backed_swap_out (page);

	if (page->frame != NULL) {
		pml4_clear_page (thread_current ()->pml4, page->va);
		palloc_free_page (page->frame->kva);
		free (page->frame);
		page->frame = NULL;
	}

	if (file_page->file != NULL) {
		file_close (file_page->file);
		file_page->file = NULL;
	}
}

static bool
lazy_load_mmap (struct page *page, void *aux_) {
	struct lazy_load_arg *aux = aux_;
	struct file_page *file_page = &page->file;

	file_page->file = aux->file;
	file_page->ofs = aux->ofs;
	file_page->read_bytes = aux->read_bytes;
	file_page->zero_bytes = aux->zero_bytes;
	file_page->mmap_page_cnt = aux->mmap_page_cnt;

	free (aux);
	return file_backed_swap_in (page, page->frame->kva);
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	void *start_addr = addr;
	size_t page_cnt;
	size_t page_idx = 0;

	if (addr == NULL || length == 0 || file == NULL
			|| pg_ofs (addr) != 0 || offset % PGSIZE != 0
			|| file_length (file) == 0)
		return NULL;

	page_cnt = (length + PGSIZE - 1) / PGSIZE;

	for (size_t i = 0; i < page_cnt; i++) {
		void *upage = (uint8_t *) addr + i * PGSIZE;

		if (!is_user_vaddr (upage)
				|| spt_find_page (&thread_current ()->spt, upage) != NULL)
			return NULL;
	}

	while (length > 0) {
		size_t page_read_bytes = length < PGSIZE ? length : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;
		struct lazy_load_arg *aux = malloc (sizeof *aux);

		if (aux == NULL)
			goto fail;

		aux->file = file_reopen (file);
		if (aux->file == NULL) {
			free (aux);
			goto fail;
		}
		aux->ofs = offset;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;
		aux->mmap_page_cnt = page_idx == 0 ? page_cnt : 0;

		if (!vm_alloc_page_with_initializer (VM_FILE, addr, writable,
					lazy_load_mmap, aux)) {
			file_close (aux->file);
			free (aux);
			goto fail;
		}

		length -= page_read_bytes;
		offset += page_read_bytes;
		addr = (uint8_t *) addr + PGSIZE;
		page_idx++;
	}

	return start_addr;

fail:
	do_munmap (start_addr);
	return NULL;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = spt_find_page (spt, addr);
	size_t page_cnt = 1;

	if (page == NULL || page_get_type (page) != VM_FILE)
		return;

	if (page->operations->type == VM_UNINIT && page->uninit.aux != NULL) {
		struct lazy_load_arg *aux = page->uninit.aux;
		page_cnt = aux->mmap_page_cnt != 0 ? aux->mmap_page_cnt : 1;
	} else {
		page_cnt = page->file.mmap_page_cnt != 0 ? page->file.mmap_page_cnt : 1;
	}

	for (size_t i = 0; i < page_cnt; i++) {
		void *upage = (uint8_t *) addr + i * PGSIZE;
		page = spt_find_page (spt, upage);

		if (page == NULL || page_get_type (page) != VM_FILE)
			break;

		spt_remove_page (spt, page);
	}
}

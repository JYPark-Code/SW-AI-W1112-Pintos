/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/mmu.h"
#include "threads/thread.h"
#include "lib/kernel/bitmap.h"


/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static struct bitmap *swap_table; 
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1, 1);
    size_t pages = disk_size(swap_disk) / 8;  // 8 sector = 1 page
    swap_table = bitmap_create(pages);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;
	anon_page->swap_slot = SIZE_MAX;  // 추가

	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;

	if (anon_page->swap_slot == SIZE_MAX)
        return true;  // 첫 fault — swap에 데이터 없음

	/*
	1. anon_page->swap_slot에서 slot 번호 읽기
	2. disk_read 8회로 디스크에서 kva로 읽어오기
	3. bitmap_set으로 slot 해제 (false = free)
	4. swap_slot = SIZE_MAX로 초기화
	*/
	size_t slot = anon_page->swap_slot;
	for (int i = 0; i < 8; i++) {
    	disk_read(swap_disk, slot * 8 + i, (uint8_t *)page->frame->kva + i * 512);
	}
	bitmap_set(swap_table, slot, false);
	anon_page->swap_slot = SIZE_MAX;
	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	/*
	1. bitmap_scan_and_flip으로 free slot 찾기
	2. disk_write 8회로 페이지 내용 디스크에 쓰기
	3. pml4_clear_page로 매핑 해제
	4. anon_page->swap_slot에 slot 번호 저장
	*/
	size_t slot = bitmap_scan_and_flip(swap_table, 0, 1, false);
	if (slot == BITMAP_ERROR){
		return false;
	}
	for (int i = 0; i < 8; i++) {
    	disk_write(swap_disk, slot * 8 + i, (uint8_t *)page->frame->kva + i * 512);
	}
	// pml4_clear_page(thread_current()->pml4, page->va);
	pml4_clear_page(page->owner->pml4, page->va);

	anon_page->swap_slot = slot;
	return true;

}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	size_t slot = anon_page->swap_slot;

	if(anon_page->swap_slot != SIZE_MAX){
		bitmap_set(swap_table, slot, false);
	}
}

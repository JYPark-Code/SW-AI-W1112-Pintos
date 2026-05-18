#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
struct page;
enum vm_type;

struct anon_page {
    size_t swap_slot;   /* SIZE_MAX = 메모리에 있음 / 아니면 bitmap 인덱스 */
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);
/* fork 시 부모 anon page 가 swap 에 있을 때 부모 슬롯에서 dst kva 로 직접 읽어온다.
 * 부모 슬롯은 그대로 두어 부모의 매핑은 계속 유효. */
void anon_clone_from_swap (struct page *src_page, void *dst_kva);

#endif

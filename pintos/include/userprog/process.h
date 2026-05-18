#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "filesys/off_t.h"

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);

/* lazy_load_segment에 전달할 파일 정보.
 * owns_file: aux 가 file 의 단독 소유자인지 (true 면 lazy_load / uninit_destroy
 * 끝에서 file_close 책임을 진다). 부모의 load_segment 에서는 running_file 을
 * 그대로 공유(false), fork 시 supplemental_page_table_copy 가 file_duplicate
 * 로 새로 만든 자식 aux 만 owns_file=true 로 둔다. */
struct lazy_load_aux {
    struct file *file;
    off_t offset;
    size_t read_bytes;
    size_t zero_bytes;
    bool owns_file;
};

#endif /* userprog/process.h */

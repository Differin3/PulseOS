#ifndef MM_PAGING_H
#define MM_PAGING_H

#include <stdint.h>

void paging_init(void);
void paging_load_cr3(uint32_t cr3);
uint32_t paging_kernel_cr3(void);
int paging_enabled(void);

/* Map physical MMIO/FB range into kernel page directory (4MB PSE). */
void paging_map_physical(uint32_t phys, uint32_t bytes);

/* Private identity-mapped page directories (PSE 4MB). Returns phys addr of PDE or 0. */
uint32_t paging_create_identity_dir(void);
uint32_t paging_clone_dir(uint32_t src_cr3);
void paging_free_dir(uint32_t cr3);

/* Test helpers: unmap/remap one 4MB PDE in a given dir (not kernel dir preferred). */
void paging_unmap_pde(uint32_t cr3, uint32_t pde_index);
int paging_pde_present(uint32_t cr3, uint32_t pde_index);

/* Identity-map smoke + intentional #PF recovery. Returns 0 on success. */
int paging_autotest(void);

/* Flat GDT + user segments; iret to stub; syscall back. Returns 0 ok. */
int paging_ring3_autotest(void);

/* Per-task aspace isolation smoke. Returns 0 on success. */
int paging_aspace_autotest(void);

extern "C" void page_fault_handler_main(uint32_t error_code);
extern "C" void paging_ring3_finish(void);

#endif

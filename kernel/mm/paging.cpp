#include "mm/paging.h"
#include "serial_log.h"
#include "idt.h"
#include <stddef.h>

#define PDE_PRESENT  0x001
#define PDE_RW       0x002
#define PDE_USER     0x004
#define PDE_PSE      0x080

#define PAGE_DIR_ENTRIES 1024
#define IDENTITY_MB      128
#define FAULT_PDE        15   /* 60MB — left unmapped until #PF */

static uint32_t g_page_dir[PAGE_DIR_ENTRIES] __attribute__((aligned(4096)));
static uint32_t g_kernel_cr3 = 0;
static int g_paging_on = 0;
static volatile uint32_t g_pf_count = 0;
static volatile uint32_t g_ring3_flag = 0;

#define PAGING_ASDIR_MAX 8
static uint32_t g_asdirs[PAGING_ASDIR_MAX][PAGE_DIR_ENTRIES] __attribute__((aligned(4096)));
static uint8_t g_asdir_used[PAGING_ASDIR_MAX];

static uint32_t g_ring3_cont_esp = 0;
static uint32_t g_ring3_cont_eip = 0;

static void paging_fill_identity(uint32_t* dir, int skip_fault_pde) {
    for (int i = 0; i < PAGE_DIR_ENTRIES; i++) dir[i] = 0;
    for (int i = 0; i < (IDENTITY_MB / 4); i++) {
        if (skip_fault_pde && i == FAULT_PDE) continue;
        uint32_t addr = (uint32_t)i * 0x400000u;
        dir[i] = addr | PDE_PRESENT | PDE_RW | PDE_PSE | PDE_USER;
    }
}

static uint32_t* paging_dir_ptr(uint32_t cr3) {
    if (!cr3) cr3 = g_kernel_cr3;
    return (uint32_t*)(cr3 & ~0xFFFu);
}

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t gran;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct tss_entry {
    uint32_t prev;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t unused[23];
} __attribute__((packed));

static struct gdt_entry g_gdt[6];
static struct gdt_ptr g_gp;
static struct tss_entry g_tss;
static uint8_t g_user_stack[4096] __attribute__((aligned(16)));
static uint8_t g_kernel_irq_stack[4096] __attribute__((aligned(16)));

static void gdt_set(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    g_gdt[num].base_low = (uint16_t)(base & 0xFFFF);
    g_gdt[num].base_mid = (uint8_t)((base >> 16) & 0xFF);
    g_gdt[num].base_high = (uint8_t)((base >> 24) & 0xFF);
    g_gdt[num].limit_low = (uint16_t)(limit & 0xFFFF);
    g_gdt[num].gran = (uint8_t)((limit >> 16) & 0x0F);
    g_gdt[num].gran |= (uint8_t)(gran & 0xF0);
    g_gdt[num].access = access;
}

static void gdt_install(void) {
    g_gp.limit = (uint16_t)(sizeof(g_gdt) - 1);
    g_gp.base = (uint32_t)&g_gdt;

    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF); /* kernel code 0x08 */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF); /* kernel data 0x10 */
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF); /* user code 0x18 */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF); /* user data 0x20 */

    for (unsigned i = 0; i < sizeof(g_tss) / 4; i++) ((uint32_t*)&g_tss)[i] = 0;
    g_tss.ss0 = 0x10;
    g_tss.esp0 = (uint32_t)(g_kernel_irq_stack + sizeof(g_kernel_irq_stack));
    gdt_set(5, (uint32_t)&g_tss, sizeof(g_tss) - 1, 0x89, 0x00); /* TSS 0x28 */

    asm volatile ("lgdt %0" : : "m"(g_gp));
    asm volatile (
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "ljmp $0x08, $1f\n"
        "1:\n"
        "mov $0x28, %%ax\n"
        "ltr %%ax\n"
        : : : "ax", "memory"
    );

    /* IDT selectors must match new GDT (kernel code = 0x08) */
    idt_init();
}

void paging_load_cr3(uint32_t cr3) {
    if (!cr3) cr3 = g_kernel_cr3;
    if (!cr3) return;
    asm volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

uint32_t paging_kernel_cr3(void) {
    return g_kernel_cr3;
}

int paging_enabled(void) {
    return g_paging_on;
}

void paging_init(void) {
    if (g_paging_on) return;

    for (int i = 0; i < PAGING_ASDIR_MAX; i++) g_asdir_used[i] = 0;
    paging_fill_identity(g_page_dir, 1);

    g_kernel_cr3 = (uint32_t)&g_page_dir;

    uint32_t cr4;
    asm volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x10; /* PSE */
    asm volatile ("mov %0, %%cr4" : : "r"(cr4));

    asm volatile ("mov %0, %%cr3" : : "r"(g_kernel_cr3) : "memory");

    uint32_t cr0;
    asm volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u; /* PG */
    asm volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");

    g_paging_on = 1;
    log_msg(LOG_INFO, "mm", "paging on");
}

void paging_map_physical(uint32_t phys, uint32_t bytes) {
    if (!g_paging_on || bytes == 0) return;
    uint32_t start = phys & ~0x3FFFFFu;
    uint32_t end = (phys + bytes + 0x3FFFFFu) & ~0x3FFFFFu;
    for (uint32_t addr = start; addr < end; addr += 0x400000u) {
        uint32_t pde = addr >> 22;
        if (pde >= PAGE_DIR_ENTRIES) break;
        g_page_dir[pde] = addr | PDE_PRESENT | PDE_RW | PDE_PSE | PDE_USER;
        for (int i = 0; i < PAGING_ASDIR_MAX; i++) {
            if (!g_asdir_used[i]) continue;
            g_asdirs[i][pde] = g_page_dir[pde];
        }
    }
    asm volatile ("mov %0, %%cr3" : : "r"(g_kernel_cr3) : "memory");
}

uint32_t paging_create_identity_dir(void) {
    if (!g_paging_on) paging_init();
    for (int i = 0; i < PAGING_ASDIR_MAX; i++) {
        if (g_asdir_used[i]) continue;
        g_asdir_used[i] = 1;
        paging_fill_identity(g_asdirs[i], 0); /* full identity, including FAULT_PDE */
        /* Copy high MMIO/FB mappings from kernel directory */
        for (int p = (IDENTITY_MB / 4); p < PAGE_DIR_ENTRIES; p++) {
            g_asdirs[i][p] = g_page_dir[p];
        }
        return (uint32_t)&g_asdirs[i][0];
    }
    return 0;
}

uint32_t paging_clone_dir(uint32_t src_cr3) {
    uint32_t* src = paging_dir_ptr(src_cr3);
    if (!src) return 0;
    uint32_t dst_cr3 = paging_create_identity_dir();
    if (!dst_cr3) return 0;
    uint32_t* dst = paging_dir_ptr(dst_cr3);
    for (int i = 0; i < PAGE_DIR_ENTRIES; i++) dst[i] = src[i];
    return dst_cr3;
}

void paging_free_dir(uint32_t cr3) {
    if (!cr3 || cr3 == g_kernel_cr3) return;
    for (int i = 0; i < PAGING_ASDIR_MAX; i++) {
        if ((uint32_t)&g_asdirs[i][0] == cr3) {
            g_asdir_used[i] = 0;
            return;
        }
    }
}

void paging_unmap_pde(uint32_t cr3, uint32_t pde_index) {
    uint32_t* dir = paging_dir_ptr(cr3);
    if (!dir || pde_index >= PAGE_DIR_ENTRIES) return;
    dir[pde_index] = 0;
    uint32_t cur;
    asm volatile ("mov %%cr3, %0" : "=r"(cur));
    if ((cur & ~0xFFFu) == ((cr3 ? cr3 : g_kernel_cr3) & ~0xFFFu)) {
        asm volatile ("mov %0, %%cr3" : : "r"(cur) : "memory");
    }
}

int paging_pde_present(uint32_t cr3, uint32_t pde_index) {
    uint32_t* dir = paging_dir_ptr(cr3);
    if (!dir || pde_index >= PAGE_DIR_ENTRIES) return 0;
    return (dir[pde_index] & PDE_PRESENT) ? 1 : 0;
}

extern "C" void page_fault_handler_main(uint32_t error_code) {
    (void)error_code;
    uint32_t fault_addr;
    asm volatile ("mov %%cr2, %0" : "=r"(fault_addr));
    g_pf_count++;

    uint32_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint32_t* dir = (uint32_t*)(cr3 & ~0xFFFu);

    uint32_t pde_i = fault_addr >> 22;
    if (pde_i < PAGE_DIR_ENTRIES && !(dir[pde_i] & PDE_PRESENT)) {
        uint32_t addr = pde_i * 0x400000u;
        dir[pde_i] = addr | PDE_PRESENT | PDE_RW | PDE_PSE | PDE_USER;
        asm volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
        log_fmt3(LOG_INFO, "mm", "pf map", "addr", fault_addr, "pde", pde_i, "n", g_pf_count);
        return;
    }
    log_fmt3(LOG_ERR, "mm", "pf panic", "addr", fault_addr, "err", error_code, "n", g_pf_count);
    while (1) asm volatile ("hlt");
}

int paging_autotest(void) {
    if (!g_paging_on) paging_init();

    volatile uint32_t* p = (volatile uint32_t*)0x00100000;
    uint32_t v = *p;
    *p = v;

    uint32_t before = g_pf_count;
    volatile uint8_t* fault = (volatile uint8_t*)(FAULT_PDE * 0x400000u + 0x100);
    uint8_t tmp = *fault;
    *fault = tmp;

    if (g_pf_count <= before) {
        log_fmt3(LOG_ERR, "mm", "pf missing", "n", g_pf_count, "ok", 0u, "x", 0u);
        return -1;
    }
    return 0;
}

extern "C" void ring3_user_stub(void);
extern "C" void ring3_enter(uint32_t user_eip, uint32_t user_esp,
                            uint32_t* cont_esp_out, uint32_t* cont_eip_out);

extern "C" void paging_ring3_finish(void) {
    g_ring3_flag = 1;
    uint32_t esp = g_ring3_cont_esp;
    uint32_t eip = g_ring3_cont_eip;
    asm volatile (
        "cli\n"
        "mov %0, %%esp\n"
        "jmp *%1\n"
        : : "r"(esp), "r"(eip) : "memory"
    );
}

int paging_ring3_autotest(void) {
    if (!g_paging_on) paging_init();
    gdt_install();

    g_ring3_flag = 0;
    g_tss.esp0 = (uint32_t)(g_kernel_irq_stack + sizeof(g_kernel_irq_stack));

    uint32_t user_esp = (uint32_t)(g_user_stack + sizeof(g_user_stack));
    uint32_t user_eip = (uint32_t)&ring3_user_stub;

    ring3_enter(user_eip, user_esp, &g_ring3_cont_esp, &g_ring3_cont_eip);

    if (!g_ring3_flag) {
        log_msg(LOG_ERR, "mm", "ring3 no flag");
        return -1;
    }
    asm volatile ("sti");
    return 0;
}

/* Isolation: dir A unmaps PDE; access from A PF-maps only A; B stays unmapped. */
int paging_aspace_autotest(void) {
    if (!g_paging_on) paging_init();

    const uint32_t pde = 20; /* 80MB */
    uint32_t a = paging_create_identity_dir();
    uint32_t b = paging_create_identity_dir();
    if (!a || !b || a == b) return -1;

    paging_unmap_pde(a, pde);
    paging_unmap_pde(b, pde);
    if (paging_pde_present(a, pde) || paging_pde_present(b, pde)) return -2;

    uint32_t prev;
    asm volatile ("mov %%cr3, %0" : "=r"(prev));
    paging_load_cr3(a);

    uint32_t before = g_pf_count;
    volatile uint8_t* va = (volatile uint8_t*)(pde * 0x400000u + 0x200);
    uint8_t tmp = *va;
    *va = tmp;

    if (g_pf_count <= before) {
        paging_load_cr3(prev);
        paging_free_dir(a);
        paging_free_dir(b);
        return -3;
    }
    if (!paging_pde_present(a, pde)) {
        paging_load_cr3(prev);
        paging_free_dir(a);
        paging_free_dir(b);
        return -4;
    }
    if (paging_pde_present(b, pde)) {
        paging_load_cr3(prev);
        paging_free_dir(a);
        paging_free_dir(b);
        return -5;
    }

    paging_load_cr3(prev);
    paging_free_dir(a);
    paging_free_dir(b);
    return 0;
}

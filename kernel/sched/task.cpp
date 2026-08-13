#include "sched/task.h"
#include "heap.h"
#include "serial_log.h"
#include "drivers/timer/pit.h"
#include "drivers/network/socket.h"
#include "drivers/network/core/net_ports.h"
#include "mm/paging.h"
#include <stddef.h>

extern "C" void sched_switch(uint32_t** old_esp, uint32_t* new_esp);

static struct task g_tasks[TASK_MAX];
static struct task* g_current = 0;
static bool g_sched_ready = false;
static int g_next_id = 1;
static uint32_t g_yield_count = 0;
static volatile uint32_t g_probe_flag = 0;
static volatile int g_need_resched = 0;
static volatile uint32_t g_slice_left = TASK_SLICE_TICKS;
static uint32_t g_wake_count = 0;
static uint32_t g_net_wake_count = 0;
static uint16_t g_kill_net_port = 39201;

static void task_copy_name(char* dst, const char* src) {
    size_t i = 0;
    if (!src) src = "?";
    while (src[i] && i + 1 < TASK_NAME_MAX) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void task_copy_str(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    if (!src) src = "";
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void task_fds_clear(struct task* t) {
    for (int i = 0; i < TASK_FD_MAX; i++) {
        t->fds[i].type = TASK_FD_NONE;
        t->fds[i].handle = -1;
        t->fds[i].path[0] = 0;
    }
}

static void task_fds_copy(struct task* dst, const struct task* src) {
    for (int i = 0; i < TASK_FD_MAX; i++) {
        dst->fds[i] = src->fds[i];
    }
}

const char* task_state_str(enum task_state st) {
    switch (st) {
        case TASK_READY: return "READY";
        case TASK_RUNNING: return "RUNNING";
        case TASK_BLOCKED: return "BLOCKED";
        case TASK_ZOMBIE: return "ZOMBIE";
        default: return "UNUSED";
    }
}

bool sched_ready(void) {
    return g_sched_ready;
}

struct task* sched_current(void) {
    return g_current;
}

int sched_current_id(void) {
    if (!g_sched_ready || !g_current) return -1;
    return g_current->id;
}

int sched_task_count(void) {
    int n = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state != TASK_UNUSED) n++;
    }
    return n;
}

void sched_foreach(task_iter_fn fn, void* userdata) {
    if (!fn) return;
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_UNUSED) continue;
        fn(&g_tasks[i], userdata);
    }
}

void task_resources_cleanup(int pid) {
    if (pid < 0) return;
    socket_close_by_pid(pid);
    struct task* t = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_UNUSED) continue;
        if (g_tasks[i].id == pid) {
            t = &g_tasks[i];
            break;
        }
    }
    if (t) {
        task_fd_close_all(t);
        if (t->cr3) {
            paging_free_dir(t->cr3);
            t->cr3 = 0;
        }
    }
}

int task_enable_aspace(int id) {
    if (!g_sched_ready || id < 0) return -1;
    struct task* t = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_UNUSED) continue;
        if (g_tasks[i].id == id) { t = &g_tasks[i]; break; }
    }
    if (!t || t->cr3) return -1;
    uint32_t dir = paging_create_identity_dir();
    if (!dir) return -1;
    t->cr3 = dir;
    if (t == g_current) paging_load_cr3(dir);
    return 0;
}

static void task_slot_clear(struct task* t) {
    if (t->state != TASK_UNUSED && t->id != TASK_PID_SYSTEMD) {
        task_resources_cleanup(t->id);
    }
    if (t->stack) {
        free(t->stack);
        t->stack = 0;
    }
    t->state = TASK_UNUSED;
    t->id = 0;
    t->esp = 0;
    t->entry = 0;
    t->arg = 0;
    t->name[0] = 0;
    t->runs = 0;
    t->wake_ms = 0;
    t->wait_reason = WAIT_NONE;
    t->is_idle = false;
    t->parent_pid = -1;
    t->cwd[0] = '/';
    t->cwd[1] = 0;
    t->cr3 = 0;
    t->is_user = false;
    task_fds_clear(t);
}

static struct task* task_alloc_slot(void) {
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_ZOMBIE) {
            task_slot_clear(&g_tasks[i]);
        }
        if (g_tasks[i].state == TASK_UNUSED) return &g_tasks[i];
    }
    return 0;
}

static void task_trampoline(void) {
    struct task* t = g_current;
    if (t && t->entry) {
        t->entry(t->arg);
    }
    task_exit();
}

static void sched_wake_sleepers(void) {
    uint32_t now = timer_ms();
    for (int i = 0; i < TASK_MAX; i++) {
        struct task* t = &g_tasks[i];
        if (t->state != TASK_BLOCKED) continue;
        if (t->wake_ms != 0 && (int32_t)(now - t->wake_ms) >= 0) {
            t->wake_ms = 0;
            t->wait_reason = WAIT_NONE;
            t->state = TASK_READY;
            g_wake_count++;
        }
    }
}

void sched_init(void) {
    for (int i = 0; i < TASK_MAX; i++) {
        g_tasks[i].state = TASK_UNUSED;
        g_tasks[i].stack = 0;
        task_fds_clear(&g_tasks[i]);
        g_tasks[i].cwd[0] = '/';
        g_tasks[i].cwd[1] = 0;
        g_tasks[i].parent_pid = -1;
        g_tasks[i].cr3 = 0;
        g_tasks[i].is_user = false;
        g_tasks[i].is_idle = false;
    }

    struct task* boot = &g_tasks[0];
    boot->id = 0;
    boot->state = TASK_RUNNING;
    boot->esp = 0;
    boot->stack = 0;
    boot->entry = 0;
    boot->arg = 0;
    task_copy_name(boot->name, "systemd");
    boot->runs = 0;
    boot->wake_ms = 0;
    boot->wait_reason = WAIT_NONE;
    boot->is_idle = false;
    boot->parent_pid = -1;
    boot->cwd[0] = '/';
    boot->cwd[1] = 0;
    boot->cr3 = 0;
    boot->is_user = false;
    task_fds_clear(boot);

    g_current = boot;
    g_next_id = 1;
    g_yield_count = 0;
    g_wake_count = 0;
    g_net_wake_count = 0;
    g_need_resched = 0;
    g_slice_left = TASK_SLICE_TICKS;
    g_sched_ready = true;
    log_msg(LOG_INFO, "sched", "init systemd+idle");
}

static void task_setup_stack(struct task* t) {
    uint32_t* sp = (uint32_t*)(t->stack + TASK_STACK_SIZE);
    *(--sp) = (uint32_t)task_trampoline;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    t->esp = sp;
}

int task_create(task_entry_fn entry, void* arg, const char* name) {
    if (!g_sched_ready || !entry) return -1;

    struct task* t = task_alloc_slot();
    if (!t) return -1;

    uint8_t* stack = (uint8_t*)malloc(TASK_STACK_SIZE);
    if (!stack) return -1;

    t->id = g_next_id++;
    t->state = TASK_READY;
    t->stack = stack;
    t->entry = entry;
    t->arg = arg;
    t->runs = 0;
    t->wake_ms = 0;
    t->wait_reason = WAIT_NONE;
    t->is_idle = false;
    t->parent_pid = g_current ? g_current->id : -1;
    t->cr3 = 0;
    t->is_user = false;
    task_copy_name(t->name, name ? name : "kthread");
    if (g_current) task_copy_str(t->cwd, TASK_CWD_MAX, g_current->cwd);
    else {
        t->cwd[0] = '/';
        t->cwd[1] = 0;
    }
    task_fds_clear(t);
    task_setup_stack(t);

    log_fmt3(LOG_INFO, "sched", "create", "id", (uint32_t)t->id, "ok", 1u, "stack", TASK_STACK_SIZE);
    return t->id;
}

int task_fork(task_entry_fn entry, void* arg, const char* name) {
    if (!g_sched_ready || !entry || !g_current) return -1;
    struct task* t = task_alloc_slot();
    if (!t) return -1;
    uint8_t* stack = (uint8_t*)malloc(TASK_STACK_SIZE);
    if (!stack) return -1;

    t->id = g_next_id++;
    t->state = TASK_READY;
    t->stack = stack;
    t->entry = entry;
    t->arg = arg;
    t->runs = 0;
    t->wake_ms = 0;
    t->wait_reason = WAIT_NONE;
    t->is_idle = false;
    t->parent_pid = g_current->id;
    if (g_current->cr3)
        t->cr3 = paging_clone_dir(g_current->cr3);
    else
        t->cr3 = 0;
    t->is_user = g_current->is_user;
    task_copy_name(t->name, name ? name : "child");
    task_copy_str(t->cwd, TASK_CWD_MAX, g_current->cwd);
    task_fds_copy(t, g_current);
    task_setup_stack(t);

    log_fmt3(LOG_INFO, "sched", "fork", "id", (uint32_t)t->id, "parent", (uint32_t)g_current->id, "ok", 1u);
    return t->id;
}

int task_exec(task_entry_fn entry, void* arg, const char* name) {
    if (!g_sched_ready || !g_current || !entry) return -1;
    if (g_current->id == TASK_PID_SYSTEMD || g_current->is_idle) return -2;
    g_current->entry = entry;
    g_current->arg = arg;
    if (name) task_copy_name(g_current->name, name);
    /* Do not rebuild stack while running on it — jump into new image directly. */
    log_fmt3(LOG_INFO, "sched", "exec", "id", (uint32_t)g_current->id, "ok", 1u, "x", 0u);
    entry(arg);
    task_exit();
    return 0;
}

int task_set_idle(int id) {
    if (!g_sched_ready || id < 0) return -1;
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_UNUSED) continue;
        if (g_tasks[i].id == id) {
            g_tasks[i].is_idle = true;
            return 0;
        }
    }
    return -1;
}

const char* task_getcwd(void) {
    if (g_current) return g_current->cwd;
    return "/";
}

int task_chdir(const char* path) {
    if (!g_current || !path || !path[0]) return -1;
    if (path[0] == '/') {
        task_copy_str(g_current->cwd, TASK_CWD_MAX, path);
        return 0;
    }
    char tmp[TASK_CWD_MAX];
    size_t p = 0;
    const char* cwd = g_current->cwd;
    while (cwd[p] && p + 1 < TASK_CWD_MAX) {
        tmp[p] = cwd[p];
        p++;
    }
    if (p > 0 && tmp[p - 1] != '/' && p + 1 < TASK_CWD_MAX) tmp[p++] = '/';
    size_t i = 0;
    while (path[i] && p + 1 < TASK_CWD_MAX) tmp[p++] = path[i++];
    tmp[p] = 0;
    task_copy_str(g_current->cwd, TASK_CWD_MAX, tmp);
    return 0;
}

int task_getcwd_pid(int pid, char* out, size_t out_cap) {
    if (!out || out_cap < 2) return -1;
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_UNUSED) continue;
        if (g_tasks[i].id != pid) continue;
        task_copy_str(out, out_cap, g_tasks[i].cwd);
        return 0;
    }
    return -1;
}

int task_fd_alloc(uint8_t type, int handle, const char* path) {
    if (!g_current) return -1;
    for (int i = 0; i < TASK_FD_MAX; i++) {
        if (g_current->fds[i].type != TASK_FD_NONE) continue;
        g_current->fds[i].type = type;
        g_current->fds[i].handle = handle;
        g_current->fds[i].path[0] = 0;
        if (path) task_copy_str(g_current->fds[i].path, sizeof(g_current->fds[i].path), path);
        return i;
    }
    return -1;
}

int task_fd_close(int fd) {
    if (!g_current || fd < 0 || fd >= TASK_FD_MAX) return -1;
    struct task_fd* e = &g_current->fds[fd];
    if (e->type == TASK_FD_NONE) return -1;
    if (e->type == TASK_FD_SOCK && e->handle >= 0) {
        socket_close(e->handle);
    }
    e->type = TASK_FD_NONE;
    e->handle = -1;
    e->path[0] = 0;
    return 0;
}

void task_fd_close_all(struct task* t) {
    if (!t) return;
    for (int i = 0; i < TASK_FD_MAX; i++) {
        if (t->fds[i].type == TASK_FD_SOCK && t->fds[i].handle >= 0) {
            /* sockets also closed by socket_close_by_pid; avoid double-close races */
            t->fds[i].handle = -1;
        }
        t->fds[i].type = TASK_FD_NONE;
        t->fds[i].path[0] = 0;
    }
}

int task_fd_get(int fd, uint8_t* type_out, int* handle_out) {
    if (!g_current || fd < 0 || fd >= TASK_FD_MAX) return -1;
    if (g_current->fds[fd].type == TASK_FD_NONE) return -1;
    if (type_out) *type_out = g_current->fds[fd].type;
    if (handle_out) *handle_out = g_current->fds[fd].handle;
    return 0;
}

static struct task* sched_pick_next(void) {
    if (!g_current) return 0;
    int start = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        if (&g_tasks[i] == g_current) {
            start = i;
            break;
        }
    }
    struct task* idle_ready = 0;
    for (int n = 1; n <= TASK_MAX; n++) {
        int i = (start + n) % TASK_MAX;
        if (g_tasks[i].state != TASK_READY) continue;
        if (g_tasks[i].is_idle) {
            if (!idle_ready) idle_ready = &g_tasks[i];
            continue;
        }
        return &g_tasks[i];
    }
    return idle_ready;
}

static void sched_switch_to(struct task* next) {
    struct task* prev = g_current;
    if (!next || !prev || next == prev) return;

    uint32_t ncr3 = next->cr3 ? next->cr3 : paging_kernel_cr3();
    uint32_t pcr3 = prev->cr3 ? prev->cr3 : paging_kernel_cr3();
    if (ncr3 != pcr3) paging_load_cr3(ncr3);

    next->state = TASK_RUNNING;
    next->runs++;
    g_current = next;
    g_yield_count++;
    g_slice_left = TASK_SLICE_TICKS;
    g_need_resched = 0;

    sched_switch(&prev->esp, next->esp);
}

void sched_yield(void) {
    if (!g_sched_ready || !g_current) return;
    sched_wake_sleepers();
    struct task* prev = g_current;
    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
    struct task* next = sched_pick_next();
    if (!next || next == prev) {
        if (prev->state == TASK_READY) prev->state = TASK_RUNNING;
        g_need_resched = 0;
        return;
    }
    sched_switch_to(next);
}

void sched_on_tick(void) {
    if (!g_sched_ready) return;
    sched_wake_sleepers();
    if (g_slice_left > 0) g_slice_left--;
    if (g_slice_left == 0) {
        g_need_resched = 1;
        g_slice_left = TASK_SLICE_TICKS;
    }
}

void sched_maybe_preempt(void) {
    if (!g_sched_ready || !g_need_resched) return;
    if (!g_current || g_current->state != TASK_RUNNING) return;
    sched_yield();
}

void sched_wake_net(void) {
    if (!g_sched_ready) return;
    for (int i = 0; i < TASK_MAX; i++) {
        struct task* t = &g_tasks[i];
        if (t->state != TASK_BLOCKED) continue;
        if (t->wait_reason != WAIT_NET) continue;
        t->wake_ms = 0;
        t->wait_reason = WAIT_NONE;
        t->state = TASK_READY;
        g_net_wake_count++;
        g_wake_count++;
    }
}

void task_block_timeout(enum task_wait_reason reason, uint32_t deadline_ms) {
    if (!g_sched_ready || !g_current) {
        while ((int32_t)(timer_ms() - deadline_ms) < 0) asm volatile ("hlt");
        return;
    }
    struct task* self = g_current;
    if (reason == WAIT_NONE) reason = WAIT_SLEEP;
    if (deadline_ms != 0 && (int32_t)(timer_ms() - deadline_ms) >= 0) return;
    self->wait_reason = reason;
    self->wake_ms = deadline_ms ? deadline_ms : 1;
    self->state = TASK_BLOCKED;
    while (self->state == TASK_BLOCKED) {
        struct task* next = sched_pick_next();
        if (next) sched_switch_to(next);
        else {
            asm volatile ("sti; hlt");
            sched_wake_sleepers();
        }
    }
    if (self->state == TASK_READY) self->state = TASK_RUNNING;
    self->wait_reason = WAIT_NONE;
    g_slice_left = TASK_SLICE_TICKS;
    g_need_resched = 0;
}

void task_sleep_ms(uint32_t ms) {
    if (!g_sched_ready || !g_current) {
        uint32_t start = timer_ms();
        while (timer_ms_since(start) < ms) asm volatile ("hlt");
        return;
    }
    if (ms == 0) {
        sched_yield();
        return;
    }
    task_block_timeout(WAIT_SLEEP, timer_ms() + ms);
}

static struct task* task_find_by_id(int id) {
    if (id < 0) return 0;
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_UNUSED) continue;
        if (g_tasks[i].id == id) return &g_tasks[i];
    }
    return 0;
}

enum task_state task_get_state(int id) {
    struct task* t = task_find_by_id(id);
    if (!t) return TASK_UNUSED;
    return t->state;
}

int task_kill(int id) {
    if (!g_sched_ready) return -1;
    struct task* t = task_find_by_id(id);
    if (!t) return -1;
    if (t->id == TASK_PID_SYSTEMD || t->is_idle) return -2;
    if (t->state == TASK_ZOMBIE) return 0;

    task_resources_cleanup(id);

    if (t == g_current) {
        log_fmt3(LOG_INFO, "sched", "kill self", "id", (uint32_t)id, "ok", 1u, "x", 0u);
        task_exit();
        return 0;
    }

    t->state = TASK_ZOMBIE;
    t->entry = 0;
    t->wake_ms = 0;
    t->wait_reason = WAIT_NONE;
    log_fmt3(LOG_INFO, "sched", "kill", "id", (uint32_t)id, "ok", 1u, "x", 0u);
    return 0;
}

void task_exit(void) {
    if (!g_current) {
        while (1) asm volatile ("hlt");
    }
    struct task* t = g_current;
    if (t->id != TASK_PID_SYSTEMD) {
        task_resources_cleanup(t->id);
    }
    t->state = TASK_ZOMBIE;
    t->entry = 0;
    t->wake_ms = 0;
    t->wait_reason = WAIT_NONE;

    struct task* next = sched_pick_next();
    if (!next) {
        next = &g_tasks[0];
        if (next->state == TASK_ZOMBIE || next->state == TASK_UNUSED) next->state = TASK_READY;
    }

    uint32_t* discarded = 0;
    next->state = TASK_RUNNING;
    next->runs++;
    g_current = next;
    g_slice_left = TASK_SLICE_TICKS;
    {
        uint32_t ncr3 = next->cr3 ? next->cr3 : paging_kernel_cr3();
        paging_load_cr3(ncr3);
    }
    sched_switch(&discarded, next->esp);
    while (1) asm volatile ("hlt");
}

static void sched_probe_task(void* arg) {
    (void)arg;
    g_probe_flag = 1;
    sched_yield();
    g_probe_flag = 2;
}

static void sched_sleep_probe(void* arg) {
    (void)arg;
    task_sleep_ms(30);
    g_probe_flag = 3;
}

static void sched_netwake_helper(void* arg) {
    (void)arg;
    task_sleep_ms(20);
    sched_wake_net();
    g_probe_flag = 4;
}

static void sched_kill_victim(void* arg) {
    (void)arg;
    g_probe_flag = 5;
    while (1) task_sleep_ms(1000);
}

static void sched_kill_net_victim(void* arg) {
    (void)arg;
    int fd = socket_create(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        g_probe_flag = 90;
        task_exit();
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = g_kill_net_port;
    addr.sin_addr = 0;
    if (socket_bind(fd, &addr) != 0) {
        socket_close(fd);
        g_probe_flag = 91;
        task_exit();
    }
    socket_set_owner(fd, "killnet", -1);
    task_fd_alloc(TASK_FD_SOCK, fd, 0);
    g_probe_flag = 6;
    while (1) task_sleep_ms(1000);
}

static void sched_cwd_child(void* arg) {
    (void)arg;
    task_chdir("/www");
    g_probe_flag = 7;
    while (1) task_sleep_ms(1000);
}

static void sched_exec_payload(void* arg) {
    (void)arg;
    g_probe_flag = 8;
    task_exit();
}

static void sched_exec_wrapper(void* arg) {
    (void)arg;
    task_exec(sched_exec_payload, 0, "execed");
}

static void sched_fork_child(void* arg) {
    (void)arg;
    g_probe_flag = 9;
    while (1) task_sleep_ms(1000);
}

static void sched_user_shell(void* arg) {
    (void)arg;
    g_probe_flag = 10;
    while (1) task_sleep_ms(1000);
}

int sched_autotest(void) {
    if (!g_sched_ready || !g_current) return -1;

    g_probe_flag = 0;
    int id = task_create(sched_probe_task, 0, "probe");
    if (id < 0) return -1;
    for (int i = 0; i < 64 && g_probe_flag < 2; i++) sched_yield();
    if (g_probe_flag < 2) return -2;
    log_fmt3(LOG_INFO, "autotest", "sched_ok", "probe", (uint32_t)id, "yields", g_yield_count, "ok", 1u);

    g_probe_flag = 0;
    uint32_t wakes_before = g_wake_count;
    if (task_create(sched_sleep_probe, 0, "sleepq") < 0) return -3;
    uint32_t t0 = timer_ms();
    while (g_probe_flag < 3 && timer_ms_since(t0) < 300) {
        sched_yield();
        asm volatile ("hlt");
    }
    uint32_t elapsed = timer_ms_since(t0);
    if (g_probe_flag < 3 || elapsed < 20) return -4;
    log_fmt3(LOG_INFO, "autotest", "sleep_ok", "ms", elapsed, "wakes", g_wake_count - wakes_before, "ok", 1u);

    g_probe_flag = 0;
    uint32_t net_wakes_before = g_net_wake_count;
    if (task_create(sched_netwake_helper, 0, "netwq") < 0) return -6;
    uint32_t n0 = timer_ms();
    task_block_timeout(WAIT_NET, n0 + 200);
    uint32_t net_elapsed = timer_ms_since(n0);
    for (int i = 0; i < 64 && g_probe_flag < 4; i++) sched_yield();
    if (g_net_wake_count <= net_wakes_before || net_elapsed > 150) return -7;
    log_fmt3(LOG_INFO, "autotest", "netwait_ok", "ms", net_elapsed,
             "wakes", g_net_wake_count - net_wakes_before, "ok", 1u);

    g_probe_flag = 0;
    int kid = task_create(sched_kill_victim, 0, "killme");
    if (kid < 0) return -9;
    for (int i = 0; i < 64 && g_probe_flag < 5; i++) sched_yield();
    if (g_probe_flag < 5) return -10;
    if (task_kill(TASK_PID_SYSTEMD) != -2) return -11;
    {
        struct task* sys = sched_current();
        const char* n = sys ? sys->name : "";
        if (!sys || sys->id != TASK_PID_SYSTEMD ||
            !(n[0]=='s'&&n[1]=='y'&&n[2]=='s'&&n[3]=='t'&&n[4]=='e'&&n[5]=='m'&&n[6]=='d'&&n[7]==0))
            return -15;
    }
    if (task_kill(kid) != 0 || task_get_state(kid) != TASK_ZOMBIE) return -13;
    log_fmt3(LOG_INFO, "autotest", "kill_ok", "id", (uint32_t)kid, "ok", 1u, "x", 0u);
    log_msg(LOG_INFO, "autotest", "systemd_ok");

    /* Phase 1: kill closes sockets */
    g_probe_flag = 0;
    int knid = task_create(sched_kill_net_victim, 0, "killnet");
    if (knid < 0) return -20;
    for (int i = 0; i < 128 && g_probe_flag < 6 && g_probe_flag < 90; i++) sched_yield();
    if (g_probe_flag != 6) {
        log_fmt3(LOG_ERR, "autotest", "kill_net_failed", "rc", 1u, "flag", g_probe_flag, "ok", 0u);
        return -21;
    }
    if (!net_ports_busy(NET_PROTO_UDP, g_kill_net_port)) {
        log_fmt3(LOG_ERR, "autotest", "kill_net_failed", "rc", 2u, "ok", 0u, "x", 0u);
        return -22;
    }
    if (task_kill(knid) != 0) return -23;
    if (net_ports_busy(NET_PROTO_UDP, g_kill_net_port)) {
        log_fmt3(LOG_ERR, "autotest", "kill_net_failed", "rc", 3u, "ok", 0u, "x", 0u);
        return -24;
    }
    log_msg(LOG_INFO, "autotest", "kill_net_ok");

    /* Phase 2: cwd isolation */
    char parent_cwd[TASK_CWD_MAX];
    task_copy_str(parent_cwd, sizeof(parent_cwd), task_getcwd());
    g_probe_flag = 0;
    int cid = task_create(sched_cwd_child, 0, "cwdchild");
    if (cid < 0) return -30;
    for (int i = 0; i < 64 && g_probe_flag < 7; i++) sched_yield();
    if (g_probe_flag < 7) return -31;
    char child_cwd[TASK_CWD_MAX];
    if (task_getcwd_pid(cid, child_cwd, sizeof(child_cwd)) != 0) return -32;
    if (!(child_cwd[0]=='/'&&child_cwd[1]=='w'&&child_cwd[2]=='w'&&child_cwd[3]=='w')) {
        log_fmt3(LOG_ERR, "autotest", "cwd_failed", "rc", 1u, "ok", 0u, "x", 0u);
        return -33;
    }
    if (task_getcwd()[0] != parent_cwd[0]) return -34;
    task_kill(cid);
    log_msg(LOG_INFO, "autotest", "cwd_ok");

    /* fd table install */
    {
        int sfd = socket_create(AF_INET, SOCK_DGRAM, 0);
        if (sfd < 0) return -35;
        int tfd = task_fd_alloc(TASK_FD_SOCK, sfd, 0);
        if (tfd < 0) {
            socket_close(sfd);
            return -36;
        }
        uint8_t ty = 0;
        int h = -1;
        if (task_fd_get(tfd, &ty, &h) != 0 || ty != TASK_FD_SOCK || h != sfd) return -37;
        task_fd_close(tfd);
        log_msg(LOG_INFO, "autotest", "fd_ok");
    }

    /* Phase 3: paging + PF */
    if (paging_autotest() != 0) {
        log_msg(LOG_ERR, "autotest", "paging_failed");
        return -40;
    }
    log_msg(LOG_INFO, "autotest", "paging_ok");
    if (paging_ring3_autotest() != 0) {
        log_msg(LOG_ERR, "autotest", "ring3_failed");
        return -41;
    }
    log_msg(LOG_INFO, "autotest", "ring3_ok");

    if (paging_aspace_autotest() != 0) {
        log_msg(LOG_ERR, "autotest", "aspace_failed");
        return -42;
    }
    log_msg(LOG_INFO, "autotest", "aspace_ok");

    /* Phase 4: fork + exec */
    g_probe_flag = 0;
    int fid = task_fork(sched_fork_child, 0, "forked");
    if (fid < 0) return -50;
    for (int i = 0; i < 64 && g_probe_flag < 9; i++) sched_yield();
    if (g_probe_flag < 9) return -51;
    {
        struct task* ft = task_find_by_id(fid);
        if (!ft || ft->parent_pid != TASK_PID_SYSTEMD) return -52;
    }
    task_kill(fid);
    log_msg(LOG_INFO, "autotest", "fork_ok");

    g_probe_flag = 0;
    int eid = task_create(sched_exec_wrapper, 0, "preexec");
    if (eid < 0) return -53;
    for (int i = 0; i < 64 && g_probe_flag < 8; i++) sched_yield();
    if (g_probe_flag < 8) return -54;
    log_msg(LOG_INFO, "autotest", "exec_ok");

    /* systemd launches a user-flagged shell kthread (Phase 4 smoke) */
    g_probe_flag = 0;
    int shid = task_fork(sched_user_shell, 0, "shell");
    if (shid < 0) return -55;
    {
        struct task* sh = task_find_by_id(shid);
        if (!sh) return -56;
        sh->is_user = true;
    }
    for (int i = 0; i < 64 && g_probe_flag < 10; i++) sched_yield();
    if (g_probe_flag < 10) return -57;
    if (task_kill(shid) != 0 || task_get_state(shid) != TASK_ZOMBIE) return -58;
    log_msg(LOG_INFO, "autotest", "user_shell_ok");

    return 0;
}

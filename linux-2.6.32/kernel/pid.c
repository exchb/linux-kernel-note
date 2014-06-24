/*
 * Generic pidhash and scalable, time-bounded PID allocator
 *
 * (C) 2002-2003 William Irwin, IBM
 * (C) 2004 William Irwin, Oracle
 * (C) 2002-2004 Ingo Molnar, Red Hat
 *
 * pid-structures are backing objects for tasks sharing a given ID to chain
 * against. There is very little to them aside from hashing them and
 * parking tasks using given ID's on a list.
 *
 * The hash is always changed with the tasklist_lock write-acquired,
 * and the hash is only accessed with the tasklist_lock at least
 * read-acquired, so there's no additional SMP locking needed here.
 *
 * We have a list of bitmap pages, which bitmaps represent the PID space.
 * Allocating and freeing PIDs is completely lockless. The worst-case
 * allocation scenario when all but one out of 1 million PIDs possible are
 * allocated already: the scanning of 32 list entries and at most PAGE_SIZE
 * bytes. The typical fastpath is a single successful setbit. Freeing is O(1).
 *
 * Pid namespaces:
 *    (C) 2007 Pavel Emelyanov <xemul@openvz.org>, OpenVZ, SWsoft Inc.
 *    (C) 2007 Sukadev Bhattiprolu <sukadev@us.ibm.com>, IBM
 *     Many thanks to Oleg Nesterov for comments and help
 *
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/rculist.h>
#include <linux/bootmem.h>
#include <linux/hash.h>
#include <linux/pid_namespace.h>
#include <linux/init_task.h>
#include <linux/syscalls.h>

#define pid_hashfn(nr, ns)    \
    hash_long((unsigned long)nr + (unsigned long)ns, pidhash_shift)
static struct hlist_head *pid_hash;
static unsigned int pidhash_shift = 4;
struct pid init_struct_pid = INIT_STRUCT_PID;

int pid_max = PID_MAX_DEFAULT;

#define RESERVED_PIDS        300

int pid_max_min = RESERVED_PIDS + 1;
int pid_max_max = PID_MAX_LIMIT;

#define BITS_PER_PAGE        (PAGE_SIZE*8)   // for 32 , PAGE_SIZE 1UL<<12 == 4096 , <<3 刚好是pid_max
#define BITS_PER_PAGE_MASK    (BITS_PER_PAGE-1) // for 32 0x7fff

static inline int mk_pid(struct pid_namespace *pid_ns,
        struct pidmap *map, int off)
{
    return (map - pid_ns->pidmap)*BITS_PER_PAGE + off;   // 将offset转化为pid(加上map偏移)
}

#define find_next_offset(map, off)                    \
        find_next_zero_bit((map)->page, BITS_PER_PAGE, off)

/*
 * PID-map pages start out as NULL, they get allocated upon
 * first use and are never deallocated. This way a low pid_max
 * value does not cause lots of bitmaps to be allocated, but
 * the scheme scales to up to 4 million PIDs, runtime.
 */
struct pid_namespace init_pid_ns = {
    .kref = {
        .refcount       = ATOMIC_INIT(2),
    },
    .pidmap = {
        [ 0 ... PIDMAP_ENTRIES-1] = { ATOMIC_INIT(BITS_PER_PAGE), NULL }
    },
    .last_pid = 0,
    .level = 0,
    .child_reaper = &init_task,
};
EXPORT_SYMBOL_GPL(init_pid_ns);

// 是否是该ns中的init角色
int is_container_init(struct task_struct *tsk)
{
    int ret = 0;
    struct pid *pid;

    rcu_read_lock();
    pid = task_pid(tsk);
    if (pid != NULL && pid->numbers[pid->level].nr == 1)
        ret = 1;
    rcu_read_unlock();

    return ret;
}
EXPORT_SYMBOL(is_container_init);

/*
 * Note: disable interrupts while the pidmap_lock is held as an
 * interrupt might come in and do read_lock(&tasklist_lock).
 *
 * If we don't disable interrupts there is a nasty deadlock between
 * detach_pid()->free_pid() and another cpu that does
 * spin_lock(&pidmap_lock) followed by an interrupt routine that does
 * read_lock(&tasklist_lock);
 *
 * After we clean up the tasklist_lock and know there are no
 * irq handlers that take it we can leave the interrupts enabled.
 * For now it is easier to be safe than to prove it can't happen.
 */

static  __cacheline_aligned_in_smp DEFINE_SPINLOCK(pidmap_lock);

// 释放一个进程的pid号码
static void free_pidmap(struct upid *upid)
{
    int nr = upid->nr;
    // 在pidmap数组中寻找相应的页面
    struct pidmap *map = upid->ns->pidmap + nr / BITS_PER_PAGE;
    int offset = nr & BITS_PER_PAGE_MASK;

    clear_bit(offset, map->page);
    atomic_inc(&map->nr_free);
}

static int alloc_pidmap(struct pid_namespace *pid_ns)
{
    int i, offset, max_scan, pid, last = pid_ns->last_pid;
    struct pidmap *map;

    pid = last + 1;             // 可能会分配到的pid
    if (pid >= pid_max)         // pid_max = (CONFIG_BASE_SMALL?0x1000:0x8000) 0x8000 = 4096
        pid = RESERVED_PIDS;    // 回绕
    offset = pid & BITS_PER_PAGE_MASK;       // pid & 0x7fff 最高位置0,这样pid就在0 - 0x0fff(4095)
    map = &pid_ns->pidmap[pid/BITS_PER_PAGE];

    // 待扫描的页面数目
    max_scan = (pid_max + BITS_PER_PAGE - 1)/BITS_PER_PAGE - !offset;   // 如果offset是0,则不用再扫描该页面
    for (i = 0; i <= max_scan; ++i) {
        if (unlikely(!map->page)) {
            void *page = kzalloc(PAGE_SIZE, GFP_KERNEL);
            /*
             * Free the page if someone raced with us
             * installing it:
             */
            spin_lock_irq(&pidmap_lock);
            if (map->page)
                kfree(page);
            else
                map->page = page;
            spin_unlock_irq(&pidmap_lock);
            if (unlikely(!map->page))
                break;
        }
        // 如果nr_free不为0,说明还有可以分配的pid位
        if (likely(atomic_read(&map->nr_free))) {
            do {
                // 如果找到
                if (!test_and_set_bit(offset, map->page)) {
                    atomic_dec(&map->nr_free);               // 空闲计数-1
                    pid_ns->last_pid = pid;
                    return pid;
                }
                offset = find_next_offset(map, offset);     // find_next_offset == find_next_zero_bit
                pid = mk_pid(pid_ns, map, offset);
            /*
             * find_next_offset() found a bit, the pid from it
             * is in-bounds, and if we fell back to the last
             * bitmap block and the final block was the same
             * as the starting point, pid is before last_pid.
             */
                // 如果到这map完了,或者大于最大值，走外面的if
            } while (offset < BITS_PER_PAGE && pid < pid_max &&
                    (i != max_scan || pid < last ||
                        !((last+1) & BITS_PER_PAGE_MASK)));
        }
        if (map < &pid_ns->pidmap[(pid_max-1)/BITS_PER_PAGE]) {
            ++map;
            offset = 0;
        } else {
            // 绕回，从RESERVED_PIDS开始
            map = &pid_ns->pidmap[0];
            offset = RESERVED_PIDS;
            if (unlikely(last == offset))    // 最初就是从last开始找的
                                             // 如果又回到这儿，意味找不到
                break;
        }
        pid = mk_pid(pid_ns, map, offset);
    }  // end of for
    return -1;
}

int next_pidmap(struct pid_namespace *pid_ns, unsigned int last)
{
    int offset;
    struct pidmap *map, *end;

    if (last >= PID_MAX_LIMIT)
        return -1;

    // 假设每页是4096BITS，那么BITS_PER_PAGE_MASK就是
    // 1111 1111 1111
    // 所以offset就是在最后一页上的offset(从0开始算起哦)
    offset = (last + 1) & BITS_PER_PAGE_MASK;
    map = &pid_ns->pidmap[(last + 1)/BITS_PER_PAGE];
    end = &pid_ns->pidmap[PIDMAP_ENTRIES];
    for (; map < end; map++, offset = 0) {
        if (unlikely(!map->page))
            continue;
        offset = find_next_bit((map)->page, BITS_PER_PAGE, offset);
        if (offset < BITS_PER_PAGE)
            return mk_pid(pid_ns, map, offset);
        // 如果offset > BITS_PER_PAGE，offset进入下次循环前会被clear为0
    }
    return -1;
}

void put_pid(struct pid *pid)
{
    struct pid_namespace *ns;

    if (!pid)
        return;

    ns = pid->numbers[pid->level].ns;
    if ((atomic_read(&pid->count) == 1) ||
         atomic_dec_and_test(&pid->count)) {
        kmem_cache_free(ns->pid_cachep, pid);
        put_pid_ns(ns);
    }
}
EXPORT_SYMBOL_GPL(put_pid);

static void delayed_put_pid(struct rcu_head *rhp)
{
    struct pid *pid = container_of(rhp, struct pid, rcu);
    put_pid(pid);
}

void free_pid(struct pid *pid)
{
    /* We can be called with write_lock_irq(&tasklist_lock) held */
    int i;
    unsigned long flags;

    spin_lock_irqsave(&pidmap_lock, flags);
    for (i = 0; i <= pid->level; i++)
        hlist_del_rcu(&pid->numbers[i].pid_chain);
    spin_unlock_irqrestore(&pidmap_lock, flags);

    for (i = 0; i <= pid->level; i++)
        free_pidmap(pid->numbers + i);

    call_rcu(&pid->rcu, delayed_put_pid);
}

struct pid *alloc_pid(struct pid_namespace *ns)
{
    struct pid *pid;
    enum pid_type type;
    int i, nr;
    struct pid_namespace *tmp;
    struct upid *upid;

    // 从slab中分配一个pid结构
    pid = kmem_cache_alloc(ns->pid_cachep, GFP_KERNEL);
    if (!pid)
        goto out;

    tmp = ns;
    for (i = ns->level; i >= 0; i--) {
        nr = alloc_pidmap(tmp);
        if (nr < 0)
            goto out_free;

        pid->numbers[i].nr = nr;
        pid->numbers[i].ns = tmp;
        tmp = tmp->parent;
    }

    get_pid_ns(ns);
    pid->level = ns->level;
    atomic_set(&pid->count, 1);
    for (type = 0; type < PIDTYPE_MAX; ++type)
        INIT_HLIST_HEAD(&pid->tasks[type]);

    spin_lock_irq(&pidmap_lock);
    for (i = ns->level; i >= 0; i--) {
        upid = &pid->numbers[i];
        hlist_add_head_rcu(&upid->pid_chain,
                &pid_hash[pid_hashfn(upid->nr, upid->ns)]);
    }
    spin_unlock_irq(&pidmap_lock);

out:
    return pid;

out_free:
    while (++i <= ns->level)
        free_pidmap(pid->numbers + i);

    kmem_cache_free(ns->pid_cachep, pid);
    pid = NULL;
    goto out;
}

/* --------------------------------------------------------------------------*/
/**
 * @brief  用局部id和命名空间确定pid实例
 *
 * @param nr    局部id
 * @param ns    namespace
 *
 * @returns   NULL or pid
 */
/* ----------------------------------------------------------------------------*/
struct pid *find_pid_ns(int nr, struct pid_namespace *ns)
{
    struct hlist_node *elem;
    struct upid *pnr;

    hlist_for_each_entry_rcu(pnr, elem,
            &pid_hash[pid_hashfn(nr, ns)], pid_chain)
        if (pnr->nr == nr && pnr->ns == ns)
            return container_of(pnr, struct pid,
                    numbers[ns->level]);

    return NULL;
}
EXPORT_SYMBOL_GPL(find_pid_ns);

struct pid *find_vpid(int nr)
{
    return find_pid_ns(nr, current->nsproxy->pid_ns);
}
EXPORT_SYMBOL_GPL(find_vpid);

/*
 * attach_pid() must be called with the tasklist_lock write-held.
 */
/* 建立双向连接，task_struct 可以 task_struct->pids[type]->pid 访问其pid
 * pid可以遍历pid->tasks[type] 找task_struct
 */
void attach_pid(struct task_struct *task, enum pid_type type,
        struct pid *pid)
{
    struct pid_link *link;

    link = &task->pids[type];
    link->pid = pid;
    hlist_add_head_rcu(&link->node, &pid->tasks[type]);
}

static void __change_pid(struct task_struct *task, enum pid_type type,
            struct pid *new)
{
    struct pid_link *link;
    struct pid *pid;
    int tmp;

    link = &task->pids[type];
    pid = link->pid;

    hlist_del_rcu(&link->node);
    link->pid = new;

    for (tmp = PIDTYPE_MAX; --tmp >= 0; )
        if (!hlist_empty(&pid->tasks[tmp]))
            return;

    free_pid(pid);
}

void detach_pid(struct task_struct *task, enum pid_type type)
{
    __change_pid(task, type, NULL);
}

void change_pid(struct task_struct *task, enum pid_type type,
        struct pid *pid)
{
    __change_pid(task, type, pid);
    attach_pid(task, type, pid);
}

/* transfer_pid is an optimization of attach_pid(new), detach_pid(old) */
void transfer_pid(struct task_struct *old, struct task_struct *new,
               enum pid_type type)
{
    new->pids[type].pid = old->pids[type].pid;
    hlist_replace_rcu(&old->pids[type].node, &new->pids[type].node);
}

struct task_struct *pid_task(struct pid *pid, enum pid_type type)
{
    struct task_struct *result = NULL;
    if (pid) {
        struct hlist_node *first;
        first = rcu_dereference(pid->tasks[type].first);
        if (first)
            result = hlist_entry(first, struct task_struct, pids[(type)].node);
    }
    return result;
}
EXPORT_SYMBOL(pid_task);

/*
 * Must be called under rcu_read_lock() or with tasklist_lock read-held.
 */
// 找进程
struct task_struct *find_task_by_pid_ns(pid_t nr, struct pid_namespace *ns)
{
    return pid_task(find_pid_ns(nr, ns), PIDTYPE_PID);
}

struct task_struct *find_task_by_vpid(pid_t vnr)
{
    return find_task_by_pid_ns(vnr, current->nsproxy->pid_ns);
}

// .. 合并了..
struct pid *get_task_pid(struct task_struct *task, enum pid_type type)
{
    struct pid *pid;
    rcu_read_lock();
    if (type != PIDTYPE_PID)
        task = task->group_leader;
    pid = get_pid(task->pids[type].pid);
    rcu_read_unlock();
    return pid;
}

struct task_struct *get_pid_task(struct pid *pid, enum pid_type type)
{
    struct task_struct *result;
    rcu_read_lock();
    result = pid_task(pid, type);
    if (result)
        get_task_struct(result);
    rcu_read_unlock();
    return result;
}

struct pid *find_get_pid(pid_t nr)
{
    struct pid *pid;

    rcu_read_lock();
    pid = get_pid(find_vpid(nr));
    rcu_read_unlock();

    return pid;
}
EXPORT_SYMBOL_GPL(find_get_pid);

/* --------------------------------------------------------------------------*/
/**
 * @brief  查找命名空间该pid结构对应的数字id
 *
 * @param pid   pid结构，从get_task_pid获取
 * @param ns    当前命名空间
 *
 * @returns   数字id..pit_t 就是个int
 */
/* ----------------------------------------------------------------------------*/
pid_t pid_nr_ns(struct pid *pid, struct pid_namespace *ns)
{
    struct upid *upid;
    pid_t nr = 0;
    
    // 当前命名空间层级小于父空间层级 ?
    if (pid && ns->level <= pid->level) {
        upid = &pid->numbers[ns->level];
        if (upid->ns == ns)
            nr = upid->nr;
    }
    return nr;
}

pid_t pid_vnr(struct pid *pid)
{
    return pid_nr_ns(pid, current->nsproxy->pid_ns);
}
EXPORT_SYMBOL_GPL(pid_vnr);

pid_t __task_pid_nr_ns(struct task_struct *task, enum pid_type type,
            struct pid_namespace *ns)
{
    pid_t nr = 0;

    rcu_read_lock();
    if (!ns)
        ns = current->nsproxy->pid_ns;
    if (likely(pid_alive(task))) {
        if (type != PIDTYPE_PID)
            task = task->group_leader;
        nr = pid_nr_ns(task->pids[type].pid, ns);
    }
    rcu_read_unlock();

    return nr;
}
EXPORT_SYMBOL(__task_pid_nr_ns);

pid_t task_tgid_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
    return pid_nr_ns(task_tgid(tsk), ns);
}
EXPORT_SYMBOL(task_tgid_nr_ns);

struct pid_namespace *task_active_pid_ns(struct task_struct *tsk)
{
    return ns_of_pid(task_pid(tsk));
}
EXPORT_SYMBOL_GPL(task_active_pid_ns);

/*
 * Used by proc to find the first pid that is greater than or equal to nr.
 *
 * If there is a pid at nr this function is exactly the same as find_pid_ns.
 */
struct pid *find_ge_pid(int nr, struct pid_namespace *ns)
{
    struct pid *pid;

    do {
        pid = find_pid_ns(nr, ns);
        if (pid)
            break;
        nr = next_pidmap(ns, nr);
    } while (nr > 0);
    // 因为参数中nr>=0，所以ge_pid一定>0

    return pid;
}

/*
 * The pid hash table is scaled according to the amount of memory in the
 * machine.  From a minimum of 16 slots up to 4096 slots at one gigabyte or
 * more.
 * minimum : 2^4(pidhash_shift)
 */
void __init pidhash_init(void)
{
    int i, pidhash_size;

    pid_hash = alloc_large_system_hash("PID", sizeof(*pid_hash), 0, 18,
                       HASH_EARLY | HASH_SMALL,
                       &pidhash_shift, NULL, 4096);
    pidhash_size = 1 << pidhash_shift;

    for (i = 0; i < pidhash_size; i++)
        INIT_HLIST_HEAD(&pid_hash[i]);   // 初始化(给struct hlist_head ->first空指针)
}

void __init pidmap_init(void)
{
    init_pid_ns.pidmap[0].page = kzalloc(PAGE_SIZE, GFP_KERNEL);
    /* Reserve PID 0. We never call free_pidmap(0) */
    set_bit(0, init_pid_ns.pidmap[0].page);
    atomic_dec(&init_pid_ns.pidmap[0].nr_free);

    init_pid_ns.pid_cachep = KMEM_CACHE(pid,
            SLAB_HWCACHE_ALIGN | SLAB_PANIC);
}

// SPDX-License-Identifier: GPL-2.0
/*
 * DEPT(DEPendency Tracker) - Runtime dependency tracker
 *
 * Started by Byungchul Park <max.byungchul.park@gmail.com>:
 *
 *  Copyright (c) 2020 LG Electronics, Inc., Byungchul Park
 *
 * DEPT provides a general way to detect deadlock possibility in runtime
 * and the interest is not limited to typical lock but to every
 * syncronization primitives.
 *
 * The following ideas were borrowed from LOCKDEP:
 *
 *    1) Use a graph to track relationship between classes.
 *    2) Prevent performance regression using hash.
 *
 * The following items were enhanced from LOCKDEP:
 *
 *    1) Cover more deadlock cases.
 *    2) Allow muliple reports.
 *
 * TODO: Both LOCKDEP and DEPT should co-exist until DEPT is considered
 * stable. Then the dependency check routine should be replaced with
 * DEPT after. It should finally look like:
 *
 *
 *
 * As is:
 *
 *    LOCKDEP
 *    +-----------------------------------------+
 *    | Lock usage correctness check            | <-> locks
 *    |                                         |
 *    |                                         |
 *    | +-------------------------------------+ |
 *    | | Dependency check                    | |
 *    | | (by tracking lock acquisition order)| |
 *    | +-------------------------------------+ |
 *    |                                         |
 *    +-----------------------------------------+
 *
 *    DEPT
 *    +-----------------------------------------+
 *    | Dependency check                        | <-> waits/events
 *    | (by tracking wait and event context)    |
 *    +-----------------------------------------+
 *
 *
 *
 * To be:
 *
 *    LOCKDEP
 *    +-----------------------------------------+
 *    | Lock usage correctness check            | <-> locks
 *    |                                         |
 *    |                                         |
 *    |       (Request dependency check)        |
 *    |                    T                    |
 *    +--------------------|--------------------+
 *                         |
 *    DEPT                 V
 *    +-----------------------------------------+
 *    | Dependency check                        | <-> waits/events
 *    | (by tracking wait and event context)    |
 *    +-----------------------------------------+
 */

#include <linux/sched.h>
#include <linux/stacktrace.h>
#include <linux/spinlock.h>
#include <linux/kallsyms.h>
#include <linux/hash.h>
#include <linux/dept.h>
#include <linux/utsname.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/irq_work.h>
#include <linux/vmalloc.h>
#include "dept_internal.h"

static int dept_stop;
static int dept_per_cpu_ready;

#define DEPT_READY_WARN (!oops_in_progress)

/*
 * Make all operations using DEPT_WARN_ON() fail on oops_in_progress and
 * prevent warning message.
 */
#define DEPT_WARN_ON_ONCE(c)						\
	({								\
		int __ret = 0;						\
									\
		if (likely(DEPT_READY_WARN))				\
			__ret = WARN_ONCE(c, "DEPT_WARN_ON_ONCE: " #c);	\
		__ret;							\
	})

#define DEPT_WARN_ONCE(s...)						\
	({								\
		if (likely(DEPT_READY_WARN))				\
			WARN_ONCE(1, "DEPT_WARN_ONCE: " s);		\
	})

#define DEPT_WARN_ON(c)							\
	({								\
		int __ret = 0;						\
									\
		if (likely(DEPT_READY_WARN))				\
			__ret = WARN(c, "DEPT_WARN_ON: " #c);		\
		__ret;							\
	})

#define DEPT_WARN(s...)							\
	({								\
		if (likely(DEPT_READY_WARN))				\
			WARN(1, "DEPT_WARN: " s);			\
	})

#define DEPT_STOP(s...)							\
	({								\
		WRITE_ONCE(dept_stop, 1);				\
		if (likely(DEPT_READY_WARN))				\
			WARN(1, "DEPT_STOP: " s);			\
	})

#define DEPT_INFO_ONCE(s...)	pr_warn_once("DEPT_INFO_ONCE: " s)
#define DEPT_INFO(s...)		pr_warn("DEPT_INFO: " s)

static arch_spinlock_t dept_spin = (arch_spinlock_t)__ARCH_SPIN_LOCK_UNLOCKED;
static arch_spinlock_t dept_pool_spin = (arch_spinlock_t)__ARCH_SPIN_LOCK_UNLOCKED;

/*
 * DEPT internal engine should be careful in using outside functions
 * e.g. printk at reporting since that kind of usage might cause
 * untrackable deadlock.
 */
static atomic_t dept_outworld = ATOMIC_INIT(0);

static void dept_outworld_enter(void)
{
	atomic_inc(&dept_outworld);
}

static void dept_outworld_exit(void)
{
	atomic_dec(&dept_outworld);
}

static bool dept_outworld_entered(void)
{
	return atomic_read(&dept_outworld);
}

static bool dept_lock(void)
{
	while (!arch_spin_trylock(&dept_spin))
		if (unlikely(dept_outworld_entered()))
			return false;
	return true;
}

static void dept_unlock(void)
{
	arch_spin_unlock(&dept_spin);
}

enum bfs_ret {
	BFS_CONTINUE,
	BFS_CONTINUE_REV,
	BFS_DONE,
	BFS_SKIP,
};

static bool after(unsigned int a, unsigned int b)
{
	return (int)(b - a) < 0;
}

static bool before(unsigned int a, unsigned int b)
{
	return (int)(a - b) < 0;
}

static bool valid_stack(struct dept_stack *s)
{
	return s && s->nr > 0;
}

static bool valid_class(struct dept_class *c)
{
	return c->key;
}

static void invalidate_class(struct dept_class *c)
{
	c->key = 0UL;
}

static struct dept_ecxt *dep_e(struct dept_dep *d)
{
	return d->ecxt;
}

static struct dept_wait *dep_w(struct dept_dep *d)
{
	return d->wait;
}

static struct dept_class *dep_fc(struct dept_dep *d)
{
	return dep_e(d)->class;
}

static struct dept_class *dep_tc(struct dept_dep *d)
{
	return dep_w(d)->class;
}

static const char *irq_str(int irq)
{
	if (irq == DEPT_CXT_SIRQ)
		return "softirq";
	if (irq == DEPT_CXT_HIRQ)
		return "hardirq";
	return "(unknown)";
}

static inline struct dept_task *dept_task(void)
{
	return &current->dept_task;
}

/*
 * Dept doesn't work either when it's stopped by DEPT_STOP() or in a nmi
 * context.
 */
static bool dept_working(void)
{
	return !READ_ONCE(dept_stop) && !in_nmi();
}

/*
 * Even k == NULL is considered as a valid key because it would use
 * &->map_key as the key in that case.
 */
extern struct lock_class_key __lockdep_no_validate__;
static bool valid_key(struct dept_key *k)
{
	return &__lockdep_no_validate__.dkey != k;
}

/*
 * Pool
 * =====================================================================
 * DEPT maintains pools to provide objects in a safe way.
 *
 *    1) Static pool is used at the beginning of booting time.
 *    2) Local pool is tried first before the static pool. Objects that
 *       have been freed will be placed.
 */

#define OBJECT(id, nr)							\
static struct dept_##id spool_##id[nr];					\
static struct dept_##id rpool_##id[nr];					\
static DEFINE_PER_CPU(struct llist_head, lpool_##id);
	#include "dept_object.h"
#undef  OBJECT

struct dept_pool dept_pool[OBJECT_NR] = {
#define OBJECT(id, nr) {						\
	.name = #id,							\
	.obj_sz = sizeof(struct dept_##id),				\
	.obj_nr = nr,							\
	.tot_nr = nr,							\
	.acc_sz = ATOMIC_INIT(sizeof(spool_##id) + sizeof(rpool_##id)), \
	.node_off = offsetof(struct dept_##id, pool_node),		\
	.spool = spool_##id,						\
	.rpool = rpool_##id,						\
	.lpool = &lpool_##id, },
	#include "dept_object.h"
#undef  OBJECT
};

static void dept_wq_work_fn(struct work_struct *work)
{
	int i;

	for (i = 0; i < OBJECT_NR; i++) {
		struct dept_pool *p = dept_pool + i;
		int sz = p->tot_nr * p->obj_sz;
		void *rpool;
		bool need;

		local_irq_disable();
		arch_spin_lock(&dept_pool_spin);
		need = !p->rpool;
		arch_spin_unlock(&dept_pool_spin);
		local_irq_enable();

		if (!need)
			continue;

		rpool = vmalloc(sz);

		if (!rpool) {
			DEPT_STOP("Failed to extend internal resources.\n");
			break;
		}

		local_irq_disable();
		arch_spin_lock(&dept_pool_spin);
		if (!p->rpool) {
			p->rpool = rpool;
			rpool = NULL;
			atomic_add(sz, &p->acc_sz);
		}
		arch_spin_unlock(&dept_pool_spin);
		local_irq_enable();

		if (rpool)
			vfree(rpool);
		else
			DEPT_INFO("Dept object(%s) just got refilled successfully.\n", p->name);
	}
}

static DECLARE_WORK(dept_wq_work, dept_wq_work_fn);

static void dept_irq_work_fn(struct irq_work *w)
{
	schedule_work(&dept_wq_work);
}

static DEFINE_IRQ_WORK(dept_irq_work, dept_irq_work_fn);

static void request_rpool_refill(void)
{
	irq_work_queue(&dept_irq_work);
}

/*
 * Can use llist no matter whether CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG is
 * enabled or not because NMI and other contexts in the same CPU never
 * run inside of DEPT concurrently by preventing reentrance.
 */
static void *from_pool(enum object_t t)
{
	struct dept_pool *p;
	struct llist_head *h;
	struct llist_node *n;

	/*
	 * llist_del_first() doesn't allow concurrent access e.g.
	 * between process and IRQ context.
	 */
	if (DEPT_WARN_ON(!irqs_disabled()))
		return NULL;

	p = &dept_pool[t];

	/*
	 * Try local pool first.
	 */
	if (likely(dept_per_cpu_ready))
		h = this_cpu_ptr(p->lpool);
	else
		h = &p->boot_pool;

	n = llist_del_first(h);
	if (n)
		return (void *)n - p->node_off;

	/*
	 * Try static pool.
	 */
	arch_spin_lock(&dept_pool_spin);

	if (!p->obj_nr) {
		p->spool = p->rpool;
		p->obj_nr = p->rpool ? p->tot_nr : 0;
		p->rpool = NULL;
		request_rpool_refill();
	}

	if (p->obj_nr) {
		void *ret;

		p->obj_nr--;
		ret = p->spool + (p->obj_nr * p->obj_sz);
		arch_spin_unlock(&dept_pool_spin);

		return ret;
	}
	arch_spin_unlock(&dept_pool_spin);

	DEPT_INFO("------------------------------------------\n"
		"  Dept object(%s) is run out.\n"
		"  Dept is trying to refill the object.\n"
		"  Nevertheless, if it fails, Dept will stop.\n",
		p->name);
	return NULL;
}

static void to_pool(void *o, enum object_t t)
{
	struct dept_pool *p = &dept_pool[t];
	struct llist_head *h;

	preempt_disable();
	if (likely(dept_per_cpu_ready))
		h = this_cpu_ptr(p->lpool);
	else
		h = &p->boot_pool;

	llist_add(o + p->node_off, h);
	preempt_enable();
}

#define OBJECT(id, nr)							\
static void (*ctor_##id)(struct dept_##id *a);				\
static void (*dtor_##id)(struct dept_##id *a);				\
static struct dept_##id *new_##id(void)					\
{									\
	struct dept_##id *a;						\
									\
	a = (struct dept_##id *)from_pool(OBJECT_##id);			\
	if (unlikely(!a))						\
		return NULL;						\
									\
	atomic_set(&a->ref, 1);						\
									\
	if (ctor_##id)							\
		ctor_##id(a);						\
									\
	return a;							\
}									\
									\
static struct dept_##id *get_##id(struct dept_##id *a)			\
{									\
	atomic_inc(&a->ref);						\
	return a;							\
}									\
									\
static void put_##id(struct dept_##id *a)				\
{									\
	if (!atomic_dec_return(&a->ref)) {				\
		if (dtor_##id)						\
			dtor_##id(a);					\
		to_pool(a, OBJECT_##id);				\
	}								\
}									\
									\
static void del_##id(struct dept_##id *a)				\
{									\
	put_##id(a);							\
}									\
									\
static bool __maybe_unused id##_consumed(struct dept_##id *a)		\
{									\
	return a && atomic_read(&a->ref) > 1;				\
}
#include "dept_object.h"
#undef  OBJECT

#define SET_CONSTRUCTOR(id, f) \
static void (*ctor_##id)(struct dept_##id *a) = f

static void initialize_dep(struct dept_dep *d)
{
	INIT_LIST_HEAD(&d->bfs_node);
	INIT_LIST_HEAD(&d->dep_node);
	INIT_LIST_HEAD(&d->dep_rev_node);
}
SET_CONSTRUCTOR(dep, initialize_dep);

static void initialize_class(struct dept_class *c)
{
	int i;

	for (i = 0; i < DEPT_CXT_IRQS_NR; i++) {
		struct dept_iecxt *ie = &c->iecxt[i];
		struct dept_iwait *iw = &c->iwait[i];

		ie->ecxt = NULL;
		ie->enirq = i;
		ie->staled = false;

		iw->wait = NULL;
		iw->irq = i;
		iw->staled = false;
		iw->touched = false;
	}
	c->bfs_gen = 0U;

	INIT_LIST_HEAD(&c->all_node);
	INIT_LIST_HEAD(&c->dep_head);
	INIT_LIST_HEAD(&c->dep_rev_head);
}
SET_CONSTRUCTOR(class, initialize_class);

static void initialize_ecxt(struct dept_ecxt *e)
{
	int i;

	for (i = 0; i < DEPT_CXT_IRQS_NR; i++) {
		e->enirq_stack[i] = NULL;
		e->enirq_ip[i] = 0UL;
	}
	e->ecxt_ip = 0UL;
	e->ecxt_stack = NULL;
	e->enirqf = 0UL;
	e->event_ip = 0UL;
	e->event_stack = NULL;
}
SET_CONSTRUCTOR(ecxt, initialize_ecxt);

static void initialize_wait(struct dept_wait *w)
{
	int i;

	for (i = 0; i < DEPT_CXT_IRQS_NR; i++) {
		w->irq_stack[i] = NULL;
		w->irq_ip[i] = 0UL;
	}
	w->wait_ip = 0UL;
	w->wait_stack = NULL;
	w->irqf = 0UL;
}
SET_CONSTRUCTOR(wait, initialize_wait);

static void initialize_stack(struct dept_stack *s)
{
	s->nr = 0;
}
SET_CONSTRUCTOR(stack, initialize_stack);

#define OBJECT(id, nr) \
static void (*ctor_##id)(struct dept_##id *a);
	#include "dept_object.h"
#undef  OBJECT

#undef  SET_CONSTRUCTOR

#define SET_DESTRUCTOR(id, f) \
static void (*dtor_##id)(struct dept_##id *a) = f

static void destroy_dep(struct dept_dep *d)
{
	if (dep_e(d))
		put_ecxt(dep_e(d));
	if (dep_w(d))
		put_wait(dep_w(d));
}
SET_DESTRUCTOR(dep, destroy_dep);

static void destroy_ecxt(struct dept_ecxt *e)
{
	int i;

	for (i = 0; i < DEPT_CXT_IRQS_NR; i++)
		if (e->enirq_stack[i])
			put_stack(e->enirq_stack[i]);
	if (e->class)
		put_class(e->class);
	if (e->ecxt_stack)
		put_stack(e->ecxt_stack);
	if (e->event_stack)
		put_stack(e->event_stack);
}
SET_DESTRUCTOR(ecxt, destroy_ecxt);

static void destroy_wait(struct dept_wait *w)
{
	int i;

	for (i = 0; i < DEPT_CXT_IRQS_NR; i++)
		if (w->irq_stack[i])
			put_stack(w->irq_stack[i]);
	if (w->class)
		put_class(w->class);
	if (w->wait_stack)
		put_stack(w->wait_stack);
}
SET_DESTRUCTOR(wait, destroy_wait);

#define OBJECT(id, nr) \
static void (*dtor_##id)(struct dept_##id *a);
	#include "dept_object.h"
#undef  OBJECT

#undef  SET_DESTRUCTOR

/*
 * Caching and hashing
 * =====================================================================
 * DEPT makes use of caching and hashing to improve performance. Each
 * object can be obtained in O(1) with its key.
 *
 * NOTE: Currently we assume all the objects in the hashs will never be
 * removed. Implement it when needed.
 */

/*
 * Some information might be lost but it's only for hashing key.
 */
static unsigned long mix(unsigned long a, unsigned long b)
{
	int halfbits = sizeof(unsigned long) * 8 / 2;
	unsigned long halfmask = (1UL << halfbits) - 1UL;

	return (a << halfbits) | (b & halfmask);
}

static bool cmp_dep(struct dept_dep *d1, struct dept_dep *d2)
{
	return dep_fc(d1)->key == dep_fc(d2)->key &&
	       dep_tc(d1)->key == dep_tc(d2)->key;
}

static unsigned long key_dep(struct dept_dep *d)
{
	return mix(dep_fc(d)->key, dep_tc(d)->key);
}

static bool cmp_class(struct dept_class *c1, struct dept_class *c2)
{
	return c1->key == c2->key;
}

static unsigned long key_class(struct dept_class *c)
{
	return c->key;
}

#define HASH(id, bits)							\
static struct hlist_head table_##id[1 << (bits)];			\
									\
static struct hlist_head *head_##id(struct dept_##id *a)		\
{									\
	return table_##id + hash_long(key_##id(a), bits);		\
}									\
									\
static struct dept_##id *hash_lookup_##id(struct dept_##id *a)		\
{									\
	struct dept_##id *b;						\
									\
	hlist_for_each_entry_rcu(b, head_##id(a), hash_node)		\
		if (cmp_##id(a, b))					\
			return b;					\
	return NULL;							\
}									\
									\
static void hash_add_##id(struct dept_##id *a)				\
{									\
	get_##id(a);							\
	hlist_add_head_rcu(&a->hash_node, head_##id(a));		\
}									\
									\
static void hash_del_##id(struct dept_##id *a)				\
{									\
	hlist_del_rcu(&a->hash_node);					\
	put_##id(a);							\
}
#include "dept_hash.h"
#undef  HASH

static struct dept_dep *lookup_dep(struct dept_class *fc,
				   struct dept_class *tc)
{
	struct dept_ecxt onetime_e = { .class = fc };
	struct dept_wait onetime_w = { .class = tc };
	struct dept_dep  onetime_d = { .ecxt = &onetime_e,
				       .wait = &onetime_w };
	return hash_lookup_dep(&onetime_d);
}

static struct dept_class *lookup_class(unsigned long key)
{
	struct dept_class onetime_c = { .key = key };

	return hash_lookup_class(&onetime_c);
}

/*
 * Report
 * =====================================================================
 * DEPT prints useful information to help debuging on detection of
 * problematic dependency.
 */

static void print_ip_stack(unsigned long ip, struct dept_stack *s)
{
	if (ip)
		print_ip_sym(KERN_WARNING, ip);

	if (valid_stack(s)) {
		pr_warn("stacktrace:\n");
		stack_trace_print(s->raw, s->nr, 5);
	}

	if (!ip && !valid_stack(s))
		pr_warn("(N/A)\n");
}

#define print_spc(spc, fmt, ...) \
	pr_warn("%*c" fmt, (spc) * 4, ' ', ##__VA_ARGS__)

static void print_diagram(struct dept_dep *d)
{
	struct dept_ecxt *e = dep_e(d);
	struct dept_wait *w = dep_w(d);
	struct dept_class *fc = dep_fc(d);
	struct dept_class *tc = dep_tc(d);
	unsigned long irqf;
	int irq;
	bool firstline = true;
	int spc = 1;
	const char *w_fn = w->wait_fn ?: "(unknown)";
	const char *e_fn = e->event_fn ?: "(unknown)";
	const char *c_fn = e->ecxt_fn ?: "(unknown)";
	const char *fc_n = fc->sched_map ? "<sched>" : (fc->name ?: "(unknown)");
	const char *tc_n = tc->sched_map ? "<sched>" : (tc->name ?: "(unknown)");

	irqf = e->enirqf & w->irqf;
	for_each_set_bit(irq, &irqf, DEPT_CXT_IRQS_NR) {
		if (!firstline)
			pr_warn("\nor\n\n");
		firstline = false;

		print_spc(spc, "[S] %s(%s:%d)\n", c_fn, fc_n, fc->sub_id);
		print_spc(spc, "    <%s interrupt>\n", irq_str(irq));
		print_spc(spc + 1, "[W] %s(%s:%d)\n", w_fn, tc_n, tc->sub_id);
		print_spc(spc, "[E] %s(%s:%d)\n", e_fn, fc_n, fc->sub_id);
	}

	if (!irqf) {
		print_spc(spc, "[S] %s(%s:%d)\n", c_fn, fc_n, fc->sub_id);
		print_spc(spc, "[W] %s(%s:%d)\n", w_fn, tc_n, tc->sub_id);
		if (w->timeout)
			print_spc(spc, "--------------- >8 timeout ---------------\n");
		print_spc(spc, "[E] %s(%s:%d)\n", e_fn, fc_n, fc->sub_id);
	}
}

static void print_dep(struct dept_dep *d)
{
	struct dept_ecxt *e = dep_e(d);
	struct dept_wait *w = dep_w(d);
	struct dept_class *fc = dep_fc(d);
	struct dept_class *tc = dep_tc(d);
	unsigned long irqf;
	int irq;
	const char *w_fn = w->wait_fn ?: "(unknown)";
	const char *e_fn = e->event_fn ?: "(unknown)";
	const char *c_fn = e->ecxt_fn ?: "(unknown)";
	const char *fc_n = fc->sched_map ? "<sched>" : (fc->name ?: "(unknown)");
	const char *tc_n = tc->sched_map ? "<sched>" : (tc->name ?: "(unknown)");

	irqf = e->enirqf & w->irqf;
	for_each_set_bit(irq, &irqf, DEPT_CXT_IRQS_NR) {
		pr_warn("%s has been enabled:\n", irq_str(irq));
		print_ip_stack(e->enirq_ip[irq], e->enirq_stack[irq]);
		pr_warn("\n");

		pr_warn("[S] %s(%s:%d):\n", c_fn, fc_n, fc->sub_id);
		print_ip_stack(e->ecxt_ip, e->ecxt_stack);
		pr_warn("\n");

		pr_warn("[W] %s(%s:%d) in %s context:\n",
		       w_fn, tc_n, tc->sub_id, irq_str(irq));
		print_ip_stack(w->irq_ip[irq], w->irq_stack[irq]);
		pr_warn("\n");

		pr_warn("[E] %s(%s:%d):\n", e_fn, fc_n, fc->sub_id);
		print_ip_stack(e->event_ip, e->event_stack);
	}

	if (!irqf) {
		pr_warn("[S] %s(%s:%d):\n", c_fn, fc_n, fc->sub_id);
		print_ip_stack(e->ecxt_ip, e->ecxt_stack);
		pr_warn("\n");

		pr_warn("[W] %s(%s:%d):\n", w_fn, tc_n, tc->sub_id);
		print_ip_stack(w->wait_ip, w->wait_stack);
		pr_warn("\n");

		pr_warn("[E] %s(%s:%d):\n", e_fn, fc_n, fc->sub_id);
		print_ip_stack(e->event_ip, e->event_stack);
	}
}

static void save_current_stack(int skip);

static bool is_timeout_wait_circle(struct dept_class *c)
{
	struct dept_class *fc = c->bfs_parent;
	struct dept_class *tc = c;

	do {
		struct dept_dep *d = lookup_dep(fc, tc);

		if (d->wait->timeout)
			return true;

		tc = fc;
		fc = fc->bfs_parent;
	} while (tc != c);

	return false;
}

/*
 * Print all classes in a circle.
 */
static void print_circle(struct dept_class *c)
{
	struct dept_class *fc = c->bfs_parent;
	struct dept_class *tc = c;
	int i;

	dept_outworld_enter();
	save_current_stack(6);

	pr_warn("===================================================\n");
	pr_warn("DEPT: Circular dependency has been detected.\n");
	pr_warn("%s %.*s %s\n", init_utsname()->release,
		(int)strcspn(init_utsname()->version, " "),
		init_utsname()->version,
		print_tainted());
	pr_warn("---------------------------------------------------\n");
	pr_warn("summary\n");
	pr_warn("---------------------------------------------------\n");

	if (is_timeout_wait_circle(c)) {
		pr_warn("NOT A DEADLOCK BUT A CIRCULAR DEPENDENCY\n");
		pr_warn("CHECK IF THE TIMEOUT IS INTENDED\n\n");
	} else if (fc == tc) {
		pr_warn("*** AA DEADLOCK ***\n\n");
	} else {
		pr_warn("*** DEADLOCK ***\n\n");
	}

	i = 0;
	do {
		struct dept_dep *d = lookup_dep(fc, tc);

		pr_warn("context %c\n", 'A' + (i++));
		print_diagram(d);
		if (fc != c)
			pr_warn("\n");

		tc = fc;
		fc = fc->bfs_parent;
	} while (tc != c);

	pr_warn("\n");
	pr_warn("[S]: start of the event context\n");
	pr_warn("[W]: the wait blocked\n");
	pr_warn("[E]: the event not reachable\n");

	i = 0;
	do {
		struct dept_dep *d = lookup_dep(fc, tc);

		pr_warn("---------------------------------------------------\n");
		pr_warn("context %c's detail\n", 'A' + i);
		pr_warn("---------------------------------------------------\n");
		pr_warn("context %c\n", 'A' + (i++));
		print_diagram(d);
		pr_warn("\n");
		print_dep(d);

		tc = fc;
		fc = fc->bfs_parent;
	} while (tc != c);

	pr_warn("---------------------------------------------------\n");
	pr_warn("information that might be helpful\n");
	pr_warn("---------------------------------------------------\n");
	dump_stack();

	dept_outworld_exit();
}

/*
 * BFS(Breadth First Search)
 * =====================================================================
 * Whenever a new dependency is added into the graph, search the graph
 * for a new circular dependency.
 */

static void enqueue(struct list_head *h, struct dept_dep *d)
{
	list_add_tail(&d->bfs_node, h);
}

static struct dept_dep *dequeue(struct list_head *h)
{
	struct dept_dep *d;

	d = list_first_entry(h, struct dept_dep, bfs_node);
	list_del(&d->bfs_node);
	return d;
}

static bool empty(struct list_head *h)
{
	return list_empty(h);
}

static void extend_queue(struct list_head *h, struct dept_class *cur)
{
	struct dept_dep *d;

	list_for_each_entry(d, &cur->dep_head, dep_node) {
		struct dept_class *next = dep_tc(d);

		if (cur->bfs_gen == next->bfs_gen)
			continue;
		next->bfs_gen = cur->bfs_gen;
		next->bfs_dist = cur->bfs_dist + 1;
		next->bfs_parent = cur;
		enqueue(h, d);
	}
}

static void extend_queue_rev(struct list_head *h, struct dept_class *cur)
{
	struct dept_dep *d;

	list_for_each_entry(d, &cur->dep_rev_head, dep_rev_node) {
		struct dept_class *next = dep_fc(d);

		if (cur->bfs_gen == next->bfs_gen)
			continue;
		next->bfs_gen = cur->bfs_gen;
		next->bfs_dist = cur->bfs_dist + 1;
		next->bfs_parent = cur;
		enqueue(h, d);
	}
}

typedef enum bfs_ret bfs_f(struct dept_dep *d, void *in, void **out);
static unsigned int bfs_gen;

/*
 * NOTE: Must be called with dept_lock held.
 */
static void bfs(struct dept_class *c, bfs_f *cb, void *in, void **out)
{
	LIST_HEAD(q);
	enum bfs_ret ret;

	if (DEPT_WARN_ON(!cb))
		return;

	/*
	 * Avoid zero bfs_gen.
	 */
	bfs_gen = bfs_gen + 1 ?: 1;

	c->bfs_gen = bfs_gen;
	c->bfs_dist = 0;
	c->bfs_parent = c;

	ret = cb(NULL, in, out);
	if (ret == BFS_DONE)
		return;
	if (ret == BFS_SKIP)
		return;
	if (ret == BFS_CONTINUE)
		extend_queue(&q, c);
	if (ret == BFS_CONTINUE_REV)
		extend_queue_rev(&q, c);

	while (!empty(&q)) {
		struct dept_dep *d = dequeue(&q);

		ret = cb(d, in, out);
		if (ret == BFS_DONE)
			break;
		if (ret == BFS_SKIP)
			continue;
		if (ret == BFS_CONTINUE)
			extend_queue(&q, dep_tc(d));
		if (ret == BFS_CONTINUE_REV)
			extend_queue_rev(&q, dep_fc(d));
	}

	while (!empty(&q))
		dequeue(&q);
}

/*
 * Main operations
 * =====================================================================
 * Add dependencies - Each new dependency is added into the graph and
 * checked if it forms a circular dependency.
 *
 * Track waits - Waits are queued into the ring buffer for later use to
 * generate appropriate dependencies with cross-event.
 *
 * Track event contexts(ecxt) - Event contexts are pushed into local
 * stack for later use to generate appropriate dependencies with waits.
 */

static unsigned long cur_enirqf(void);
static int cur_cxt(void);
static unsigned int cur_ctxt_id(void);

static struct dept_iecxt *iecxt(struct dept_class *c, int irq)
{
	return &c->iecxt[irq];
}

static struct dept_iwait *iwait(struct dept_class *c, int irq)
{
	return &c->iwait[irq];
}

static void stale_iecxt(struct dept_iecxt *ie)
{
	if (ie->ecxt)
		put_ecxt(ie->ecxt);

	WRITE_ONCE(ie->ecxt, NULL);
	WRITE_ONCE(ie->staled, true);
}

static void set_iecxt(struct dept_iecxt *ie, struct dept_ecxt *e)
{
	/*
	 * ->ecxt will never be updated once getting set until the class
	 * gets removed.
	 */
	if (ie->ecxt)
		DEPT_WARN_ON(1);
	else
		WRITE_ONCE(ie->ecxt, get_ecxt(e));
}

static void stale_iwait(struct dept_iwait *iw)
{
	if (iw->wait)
		put_wait(iw->wait);

	WRITE_ONCE(iw->wait, NULL);
	WRITE_ONCE(iw->staled, true);
}

static void set_iwait(struct dept_iwait *iw, struct dept_wait *w)
{
	/*
	 * ->wait will never be updated once getting set until the class
	 * gets removed.
	 */
	if (iw->wait)
		DEPT_WARN_ON(1);
	else
		WRITE_ONCE(iw->wait, get_wait(w));

	iw->touched = true;
}

static void touch_iwait(struct dept_iwait *iw)
{
	iw->touched = true;
}

static void untouch_iwait(struct dept_iwait *iw)
{
	iw->touched = false;
}

static struct dept_stack *get_current_stack(void)
{
	struct dept_stack *s = dept_task()->stack;

	return s ? get_stack(s) : NULL;
}

static void prepare_current_stack(void)
{
	DEPT_WARN_ON(dept_task()->stack);

	dept_task()->stack = new_stack();
}

static void save_current_stack(int skip)
{
	struct dept_stack *s = dept_task()->stack;

	if (!s)
		return;

	if (valid_stack(s))
		return;

	s->nr = stack_trace_save(s->raw, DEPT_MAX_STACK_ENTRY, skip);
}

static void finish_current_stack(void)
{
	struct dept_stack *s = dept_task()->stack;

	dept_task()->stack = NULL;

	/*
	 * Fill the struct dept_stack with a valid stracktrace if it has
	 * been referred at least once.
	 */
	if (stack_consumed(s))
		save_current_stack(2);

	/*
	 * Actual deletion will happen at put_stack() if the stack has
	 * been referred.
	 */
	if (s)
		del_stack(s);
}

/*
 * FIXME: For now, disable LOCKDEP while DEPT is working.
 *
 * Both LOCKDEP and DEPT report it on a deadlock detection using
 * printk taking the risk of another deadlock that might be caused by
 * locks of console or printk between inside and outside of them.
 *
 * For DEPT, it's no problem since multiple reports are allowed. But it
 * would be a bad idea for LOCKDEP since it will stop even on a singe
 * report. So we need to prevent LOCKDEP from its reporting the risk
 * DEPT would take when reporting something.
 */
#include <linux/lockdep.h>

void noinstr dept_off(void)
{
	dept_task()->recursive++;
	lockdep_off();
}

void noinstr dept_on(void)
{
	dept_task()->recursive--;
	lockdep_on();
}

static unsigned long dept_enter(void)
{
	unsigned long flags;

	flags = arch_local_irq_save();
	dept_off();
	prepare_current_stack();
	return flags;
}

static void dept_exit(unsigned long flags)
{
	finish_current_stack();
	dept_on();
	arch_local_irq_restore(flags);
}

static unsigned long dept_enter_recursive(void)
{
	unsigned long flags;

	flags = arch_local_irq_save();
	return flags;
}

static void dept_exit_recursive(unsigned long flags)
{
	arch_local_irq_restore(flags);
}

/*
 * NOTE: Must be called with dept_lock held.
 */
static struct dept_dep *__add_dep(struct dept_ecxt *e,
				  struct dept_wait *w)
{
	struct dept_dep *d;

	if (DEPT_WARN_ON(!valid_class(e->class)))
		return NULL;

	if (DEPT_WARN_ON(!valid_class(w->class)))
		return NULL;

	if (lookup_dep(e->class, w->class))
		return NULL;

	d = new_dep();
	if (unlikely(!d))
		return NULL;

	d->ecxt = get_ecxt(e);
	d->wait = get_wait(w);

	/*
	 * Add the dependency into hash and graph.
	 */
	hash_add_dep(d);
	list_add(&d->dep_node, &dep_fc(d)->dep_head);
	list_add(&d->dep_rev_node, &dep_tc(d)->dep_rev_head);
	return d;
}

static enum bfs_ret cb_check_dl(struct dept_dep *d,
				void *in, void **out)
{
	struct dept_dep *new = (struct dept_dep *)in;

	/*
	 * initial condition for this BFS search
	 */
	if (!d) {
		dep_tc(new)->bfs_parent = dep_fc(new);

		if (dep_tc(new) != dep_fc(new))
			return BFS_CONTINUE;

		/*
		 * AA circle does not make additional deadlock. We don't
		 * have to continue this BFS search.
		 */
		print_circle(dep_tc(new));
		return BFS_DONE;
	}

	/*
	 * Allow multiple reports.
	 */
	if (dep_tc(d) == dep_fc(new))
		print_circle(dep_tc(new));

	return BFS_CONTINUE;
}

/*
 * This function is actually in charge of reporting.
 */
static void check_dl_bfs(struct dept_dep *d)
{
	bfs(dep_tc(d), cb_check_dl, (void *)d, NULL);
}

static enum bfs_ret cb_find_iw(struct dept_dep *d, void *in, void **out)
{
	int irq = *(int *)in;
	struct dept_class *fc;
	struct dept_iwait *iw;

	if (DEPT_WARN_ON(!out))
		return BFS_DONE;

	/*
	 * initial condition for this BFS search
	 */
	if (!d)
		return BFS_CONTINUE_REV;

	fc = dep_fc(d);
	iw = iwait(fc, irq);

	/*
	 * If any parent's ->wait was set, then the children would've
	 * been touched.
	 */
	if (!iw->touched)
		return BFS_SKIP;

	if (!iw->wait)
		return BFS_CONTINUE_REV;

	*out = iw;
	return BFS_DONE;
}

static struct dept_iwait *find_iw_bfs(struct dept_class *c, int irq)
{
	struct dept_iwait *iw = iwait(c, irq);
	struct dept_iwait *found = NULL;

	if (iw->wait)
		return iw;

	/*
	 * '->touched == false' guarantees there's no parent that has
	 * been set ->wait.
	 */
	if (!iw->touched)
		return NULL;

	bfs(c, cb_find_iw, (void *)&irq, (void **)&found);

	if (found)
		return found;

	untouch_iwait(iw);
	return NULL;
}

static enum bfs_ret cb_touch_iw_find_ie(struct dept_dep *d, void *in,
					void **out)
{
	int irq = *(int *)in;
	struct dept_class *tc;
	struct dept_iecxt *ie;
	struct dept_iwait *iw;

	if (DEPT_WARN_ON(!out))
		return BFS_DONE;

	/*
	 * initial condition for this BFS search
	 */
	if (!d)
		return BFS_CONTINUE;

	tc = dep_tc(d);
	ie = iecxt(tc, irq);
	iw = iwait(tc, irq);

	touch_iwait(iw);

	if (!ie->ecxt)
		return BFS_CONTINUE;

	if (!*out)
		*out = ie;

	return BFS_CONTINUE;
}

static struct dept_iecxt *touch_iw_find_ie_bfs(struct dept_class *c,
					       int irq)
{
	struct dept_iecxt *ie = iecxt(c, irq);
	struct dept_iwait *iw = iwait(c, irq);
	struct dept_iecxt *found = ie->ecxt ? ie : NULL;

	touch_iwait(iw);
	bfs(c, cb_touch_iw_find_ie, (void *)&irq, (void **)&found);
	return found;
}

/*
 * Should be called with dept_lock held.
 */
static void __add_idep(struct dept_iecxt *ie, struct dept_iwait *iw)
{
	struct dept_dep *new;

	/*
	 * There's nothing to do.
	 */
	if (!ie || !iw || !ie->ecxt || !iw->wait)
		return;

	new = __add_dep(ie->ecxt, iw->wait);

	/*
	 * Deadlock detected. Let check_dl_bfs() report it.
	 */
	if (new) {
		check_dl_bfs(new);
		stale_iecxt(ie);
		stale_iwait(iw);
	}

	/*
	 * If !new, it would be the case of lack of object resource.
	 * Just let it go and get checked by other chances. Retrying is
	 * meaningless in that case.
	 */
}

static void set_check_iecxt(struct dept_class *c, int irq,
			    struct dept_ecxt *e)
{
	struct dept_iecxt *ie = iecxt(c, irq);

	set_iecxt(ie, e);
	__add_idep(ie, find_iw_bfs(c, irq));
}

static void set_check_iwait(struct dept_class *c, int irq,
			    struct dept_wait *w)
{
	struct dept_iwait *iw = iwait(c, irq);

	set_iwait(iw, w);
	__add_idep(touch_iw_find_ie_bfs(c, irq), iw);
}

static void add_iecxt(struct dept_class *c, int irq, struct dept_ecxt *e,
		      bool stack)
{
	/*
	 * This access is safe since we ensure e->class has set locally.
	 */
	struct dept_task *dt = dept_task();
	struct dept_iecxt *ie = iecxt(c, irq);

	if (DEPT_WARN_ON(!valid_class(c)))
		return;

	if (unlikely(READ_ONCE(ie->staled)))
		return;

	/*
	 * Skip add_iecxt() if ie->ecxt has ever been set at least once.
	 * Which means it has a valid ->ecxt or been staled.
	 */
	if (READ_ONCE(ie->ecxt))
		return;

	if (unlikely(!dept_lock()))
		return;

	if (unlikely(ie->staled))
		goto unlock;
	if (ie->ecxt)
		goto unlock;

	e->enirqf |= (1UL << irq);

	/*
	 * Should be NULL since it's the first time that these
	 * enirq_{ip,stack}[irq] have ever set.
	 */
	DEPT_WARN_ON(e->enirq_ip[irq]);
	DEPT_WARN_ON(e->enirq_stack[irq]);

	e->enirq_ip[irq] = dt->enirq_ip[irq];
	e->enirq_stack[irq] = stack ? get_current_stack() : NULL;

	set_check_iecxt(c, irq, e);
unlock:
	dept_unlock();
}

static void add_iwait(struct dept_class *c, int irq, struct dept_wait *w)
{
	struct dept_iwait *iw = iwait(c, irq);

	if (DEPT_WARN_ON(!valid_class(c)))
		return;

	if (unlikely(READ_ONCE(iw->staled)))
		return;

	/*
	 * Skip add_iwait() if iw->wait has ever been set at least once.
	 * Which means it has a valid ->wait or been staled.
	 */
	if (READ_ONCE(iw->wait))
		return;

	if (unlikely(!dept_lock()))
		return;

	if (unlikely(iw->staled))
		goto unlock;
	if (iw->wait)
		goto unlock;

	w->irqf |= (1UL << irq);

	/*
	 * Should be NULL since it's the first time that these
	 * irq_{ip,stack}[irq] have ever set.
	 */
	DEPT_WARN_ON(w->irq_ip[irq]);
	DEPT_WARN_ON(w->irq_stack[irq]);

	w->irq_ip[irq] = w->wait_ip;
	w->irq_stack[irq] = get_current_stack();

	set_check_iwait(c, irq, w);
unlock:
	dept_unlock();
}

static struct dept_wait_hist *hist(int pos)
{
	struct dept_task *dt = dept_task();

	return dt->wait_hist + (pos % DEPT_MAX_WAIT_HIST);
}

static int hist_pos_next(void)
{
	struct dept_task *dt = dept_task();

	return dt->wait_hist_pos % DEPT_MAX_WAIT_HIST;
}

static void hist_advance(void)
{
	struct dept_task *dt = dept_task();

	dt->wait_hist_pos++;
	dt->wait_hist_pos %= DEPT_MAX_WAIT_HIST;
}

static struct dept_wait_hist *new_hist(void)
{
	struct dept_wait_hist *wh = hist(hist_pos_next());

	hist_advance();
	return wh;
}

static struct dept_wait_hist *last_hist(void)
{
	int pos_n = hist_pos_next();
	struct dept_wait_hist *wh_n = hist(pos_n);

	/*
	 * This is the first try.
	 */
	if (!pos_n && !wh_n->wait)
		return NULL;

	return hist(pos_n + DEPT_MAX_WAIT_HIST - 1);
}

static void add_hist(struct dept_wait *w, unsigned int wg, unsigned int ctxt_id)
{
	struct dept_wait_hist *wh;

	wh = last_hist();

	if (!wh || wh->wait->class != w->class || wh->ctxt_id != ctxt_id)
		wh = new_hist();

	if (likely(wh->wait))
		put_wait(wh->wait);

	wh->wait = get_wait(w);
	wh->wgen = wg;
	wh->ctxt_id = ctxt_id;
}

/*
 * Should be called after setting up e's iecxt and w's iwait.
 */
static void add_dep(struct dept_ecxt *e, struct dept_wait *w)
{
	struct dept_class *fc = e->class;
	struct dept_class *tc = w->class;
	struct dept_dep *d;
	int i;

	if (lookup_dep(fc, tc))
		return;

	if (unlikely(!dept_lock()))
		return;

	/*
	 * __add_dep() will lookup_dep() again with lock held.
	 */
	d = __add_dep(e, w);
	if (d) {
		check_dl_bfs(d);

		for (i = 0; i < DEPT_CXT_IRQS_NR; i++) {
			struct dept_iwait *fiw = iwait(fc, i);
			struct dept_iecxt *found_ie;
			struct dept_iwait *found_iw;

			/*
			 * '->touched == false' guarantees there's no
			 * parent that has been set ->wait.
			 */
			if (!fiw->touched)
				continue;

			/*
			 * find_iw_bfs() will untouch the iwait if
			 * not found.
			 */
			found_iw = find_iw_bfs(fc, i);

			if (!found_iw)
				continue;

			found_ie = touch_iw_find_ie_bfs(tc, i);
			__add_idep(found_ie, found_iw);
		}
	}
	dept_unlock();
}

static atomic_t wgen = ATOMIC_INIT(1);

static int next_wgen(void)
{
	/*
	 * Avoid zero wgen.
	 */
	return atomic_inc_return(&wgen) ?: atomic_inc_return(&wgen);
}

static void add_wait(struct dept_class *c, unsigned long ip,
		     const char *w_fn, int sub_l, bool sched_sleep,
		     bool timeout)
{
	struct dept_task *dt = dept_task();
	struct dept_wait *w;
	unsigned int wg = 0U;
	int cxt;
	int i;

	if (DEPT_WARN_ON(!valid_class(c)))
		return;

	w = new_wait();
	if (unlikely(!w))
		return;

	WRITE_ONCE(w->class, get_class(c));
	w->wait_ip = ip;
	w->wait_fn = w_fn;
	w->wait_stack = get_current_stack();
	w->sched_sleep = sched_sleep;
	w->timeout = timeout;

	cxt = cur_cxt();
	if (cxt == DEPT_CXT_HIRQ || cxt == DEPT_CXT_SIRQ)
		add_iwait(c, cxt, w);

	/*
	 * Avoid adding dependency between user aware nested ecxt and
	 * wait.
	 */
	for (i = dt->ecxt_held_pos - 1; i >= 0; i--) {
		struct dept_ecxt_held *eh;

		eh = dt->ecxt_held + i;

		/*
		 * the case of invalid key'ed one
		 */
		if (!eh->ecxt)
			continue;

		if (eh->ecxt->class != c || eh->sub_l == sub_l)
			add_dep(eh->ecxt, w);
	}

	wg = next_wgen();
	add_hist(w, wg, cur_ctxt_id());

	del_wait(w);
}

static bool add_ecxt(struct dept_map *m, struct dept_class *c,
		     unsigned long ip, const char *c_fn,
		     const char *e_fn, int sub_l)
{
	struct dept_task *dt = dept_task();
	struct dept_ecxt_held *eh;
	struct dept_ecxt *e;
	unsigned long irqf;
	int irq;

	if (DEPT_WARN_ON(!valid_class(c)))
		return false;

	if (DEPT_WARN_ON_ONCE(dt->ecxt_held_pos >= DEPT_MAX_ECXT_HELD))
		return false;

	if (m->nocheck) {
		eh = dt->ecxt_held + (dt->ecxt_held_pos++);
		eh->ecxt = NULL;
		eh->map = m;
		eh->class = get_class(c);
		eh->wgen = atomic_read(&wgen);
		eh->sub_l = sub_l;

		return true;
	}

	e = new_ecxt();
	if (unlikely(!e))
		return false;

	e->class = get_class(c);
	e->ecxt_ip = ip;
	e->ecxt_stack = ip ? get_current_stack() : NULL;
	e->event_fn = e_fn;
	e->ecxt_fn = c_fn;

	eh = dt->ecxt_held + (dt->ecxt_held_pos++);
	eh->ecxt = get_ecxt(e);
	eh->map = m;
	eh->class = get_class(c);
	eh->wgen = atomic_read(&wgen);
	eh->sub_l = sub_l;

	irqf = cur_enirqf();
	for_each_set_bit(irq, &irqf, DEPT_CXT_IRQS_NR)
		add_iecxt(c, irq, e, false);

	del_ecxt(e);
	return true;
}

static int find_ecxt_pos(struct dept_map *m, struct dept_class *c,
			 bool newfirst)
{
	struct dept_task *dt = dept_task();
	int i;

	if (newfirst) {
		for (i = dt->ecxt_held_pos - 1; i >= 0; i--) {
			struct dept_ecxt_held *eh;

			eh = dt->ecxt_held + i;
			if (eh->map == m && eh->class == c)
				return i;
		}
	} else {
		for (i = 0; i < dt->ecxt_held_pos; i++) {
			struct dept_ecxt_held *eh;

			eh = dt->ecxt_held + i;
			if (eh->map == m && eh->class == c)
				return i;
		}
	}
	return -1;
}

static bool pop_ecxt(struct dept_map *m, struct dept_class *c)
{
	struct dept_task *dt = dept_task();
	int pos;
	int i;

	pos = find_ecxt_pos(m, c, true);
	if (pos == -1)
		return false;

	if (dt->ecxt_held[pos].class)
		put_class(dt->ecxt_held[pos].class);

	if (dt->ecxt_held[pos].ecxt)
		put_ecxt(dt->ecxt_held[pos].ecxt);

	dt->ecxt_held_pos--;

	for (i = pos; i < dt->ecxt_held_pos; i++)
		dt->ecxt_held[i] = dt->ecxt_held[i + 1];
	return true;
}

static bool good_hist(struct dept_wait_hist *wh, unsigned int wg)
{
	return wh->wait != NULL && before(wg, wh->wgen);
}

/*
 * Binary-search the ring buffer for the earliest valid wait.
 */
static int find_hist_pos(unsigned int wg)
{
	int oldest;
	int l;
	int r;
	int pos;

	oldest = hist_pos_next();
	if (unlikely(good_hist(hist(oldest), wg))) {
		DEPT_INFO_ONCE("Need to expand the ring buffer.\n");
		return oldest;
	}

	l = oldest + 1;
	r = oldest + DEPT_MAX_WAIT_HIST - 1;
	for (pos = (l + r) / 2; l <= r; pos = (l + r) / 2) {
		struct dept_wait_hist *p = hist(pos - 1);
		struct dept_wait_hist *wh = hist(pos);

		if (!good_hist(p, wg) && good_hist(wh, wg))
			return pos % DEPT_MAX_WAIT_HIST;
		if (good_hist(wh, wg))
			r = pos - 1;
		else
			l = pos + 1;
	}
	return -1;
}

static void do_event(struct dept_map *m, struct dept_class *c,
		     unsigned int wg, unsigned long ip)
{
	struct dept_task *dt = dept_task();
	struct dept_wait_hist *wh;
	struct dept_ecxt_held *eh;
	unsigned int ctxt_id;
	int end;
	int pos;
	int i;

	if (DEPT_WARN_ON(!valid_class(c)))
		return;

	if (m->nocheck)
		return;

	/*
	 * The event was triggered before wait.
	 */
	if (!wg)
		return;

	pos = find_ecxt_pos(m, c, false);
	if (pos == -1)
		return;

	eh = dt->ecxt_held + pos;

	if (DEPT_WARN_ON(!eh->ecxt))
		return;

	eh->ecxt->event_ip = ip;
	eh->ecxt->event_stack = get_current_stack();

	/*
	 * The ecxt already has done what it needs.
	 */
	if (!before(wg, eh->wgen))
		return;

	pos = find_hist_pos(wg);
	if (pos == -1)
		return;

	ctxt_id = cur_ctxt_id();
	end = hist_pos_next();
	end = end > pos ? end : end + DEPT_MAX_WAIT_HIST;
	for (wh = hist(pos); pos < end; wh = hist(++pos)) {
		if (after(wh->wgen, eh->wgen))
			break;

		if (dt->in_sched && wh->wait->sched_sleep)
			continue;

		if (wh->ctxt_id == ctxt_id)
			add_dep(eh->ecxt, wh->wait);
	}

	for (i = 0; i < DEPT_CXT_IRQS_NR; i++) {
		struct dept_ecxt *e;

		if (before(dt->wgen_enirq[i], wg))
			continue;

		e = eh->ecxt;
		add_iecxt(e->class, i, e, false);
	}
}

static void del_dep_rcu(struct rcu_head *rh)
{
	struct dept_dep *d = container_of(rh, struct dept_dep, rh);

	preempt_disable();
	del_dep(d);
	preempt_enable();
}

/*
 * NOTE: Must be called with dept_lock held.
 */
static void disconnect_class(struct dept_class *c)
{
	struct dept_dep *d, *n;
	int i;

	list_for_each_entry_safe(d, n, &c->dep_head, dep_node) {
		list_del_rcu(&d->dep_node);
		list_del_rcu(&d->dep_rev_node);
		hash_del_dep(d);
		call_rcu(&d->rh, del_dep_rcu);
	}

	list_for_each_entry_safe(d, n, &c->dep_rev_head, dep_rev_node) {
		list_del_rcu(&d->dep_node);
		list_del_rcu(&d->dep_rev_node);
		hash_del_dep(d);
		call_rcu(&d->rh, del_dep_rcu);
	}

	for (i = 0; i < DEPT_CXT_IRQS_NR; i++) {
		stale_iecxt(iecxt(c, i));
		stale_iwait(iwait(c, i));
	}
}

/*
 * Context control
 * =====================================================================
 * Whether a wait is in {hard,soft}-IRQ context or whether
 * {hard,soft}-IRQ has been enabled on the way to an event is very
 * important to check dependency. All those things should be tracked.
 */

static unsigned long cur_enirqf(void)
{
	struct dept_task *dt = dept_task();
	int he = dt->hardirqs_enabled;
	int se = dt->softirqs_enabled;

	if (he)
		return DEPT_HIRQF | (se ? DEPT_SIRQF : 0UL);
	return 0UL;
}

static int cur_cxt(void)
{
	if (lockdep_softirq_context(current))
		return DEPT_CXT_SIRQ;
	if (lockdep_hardirq_context())
		return DEPT_CXT_HIRQ;
	return DEPT_CXT_PROCESS;
}

static unsigned int cur_ctxt_id(void)
{
	struct dept_task *dt = dept_task();
	int cxt = cur_cxt();

	return dt->cxt_id[cxt] | (1UL << cxt);
}

static void enirq_transition(int irq)
{
	struct dept_task *dt = dept_task();
	int i;

	/*
	 * IRQ can cut in on the way to the event. Used for cross-event
	 * detection.
	 *
	 *    wait context	event context(ecxt)
	 *    ------------	-------------------
	 *    wait event
	 *       UPDATE wgen
	 *			observe IRQ enabled
	 *			   UPDATE wgen
	 *			   keep the wgen locally
	 *
	 *			on the event
	 *			   check the wgen kept
	 */

	dt->wgen_enirq[irq] = next_wgen();

	for (i = dt->ecxt_held_pos - 1; i >= 0; i--) {
		struct dept_ecxt_held *eh;
		struct dept_ecxt *e;

		eh = dt->ecxt_held + i;
		e = eh->ecxt;
		if (e)
			add_iecxt(e->class, irq, e, true);
	}
}

static void dept_enirq(unsigned long ip)
{
	struct dept_task *dt = dept_task();
	unsigned long irqf = cur_enirqf();
	int irq;
	unsigned long flags;

	if (unlikely(!dept_working()))
		return;

	/*
	 * IRQ ON/OFF transition might happen while Dept is working.
	 * We cannot handle recursive entrance. Just ingnore it.
	 * Only transitions outside of Dept will be considered.
	 */
	if (dt->recursive)
		return;

	flags = dept_enter();

	for_each_set_bit(irq, &irqf, DEPT_CXT_IRQS_NR) {
		dt->enirq_ip[irq] = ip;
		enirq_transition(irq);
	}

	dept_exit(flags);
}

void dept_softirqs_on_ip(unsigned long ip)
{
	/*
	 * Assumes that it's called with IRQ disabled so that accessing
	 * current's fields is not racy.
	 */
	dept_task()->softirqs_enabled = true;
	dept_enirq(ip);
}

void dept_hardirqs_on(void)
{
	/*
	 * Assumes that it's called with IRQ disabled so that accessing
	 * current's fields is not racy.
	 */
	dept_task()->hardirqs_enabled = true;
	dept_enirq(_RET_IP_);
}

void dept_softirqs_off(void)
{
	/*
	 * Assumes that it's called with IRQ disabled so that accessing
	 * current's fields is not racy.
	 */
	dept_task()->softirqs_enabled = false;
}

void noinstr dept_hardirqs_off(void)
{
	/*
	 * Assumes that it's called with IRQ disabled so that accessing
	 * current's fields is not racy.
	 */
	dept_task()->hardirqs_enabled = false;
}

void noinstr dept_update_cxt(void)
{
	struct dept_task *dt = dept_task();

	dt->cxt_id[DEPT_CXT_PROCESS] += 1UL << DEPT_CXTS_NR;
}

/*
 * Ensure it's the outmost softirq context.
 */
void dept_softirq_enter(void)
{
	struct dept_task *dt = dept_task();

	dt->cxt_id[DEPT_CXT_SIRQ] += 1UL << DEPT_CXTS_NR;
}

/*
 * Ensure it's the outmost hardirq context.
 */
void noinstr dept_hardirq_enter(void)
{
	struct dept_task *dt = dept_task();

	dt->cxt_id[DEPT_CXT_HIRQ] += 1UL << DEPT_CXTS_NR;
}

void dept_sched_enter(void)
{
	dept_task()->in_sched = true;
}

void dept_sched_exit(void)
{
	dept_task()->in_sched = false;
}

/*
 * Exposed APIs
 * =====================================================================
 */

static void clean_classes_cache(struct dept_key *k)
{
	int i;

	for (i = 0; i < DEPT_MAX_SUBCLASSES_CACHE; i++) {
		if (!READ_ONCE(k->classes[i]))
			continue;

		WRITE_ONCE(k->classes[i], NULL);
	}
}

/*
 * Assume we don't have to consider race with the map when
 * dept_map_init() is called.
 */
void dept_map_init(struct dept_map *m, struct dept_key *k, int sub_u,
		   const char *n)
{
	unsigned long flags;

	if (unlikely(!dept_working())) {
		m->nocheck = true;
		return;
	}

	if (DEPT_WARN_ON(sub_u < 0)) {
		m->nocheck = true;
		return;
	}

	if (DEPT_WARN_ON(sub_u >= DEPT_MAX_SUBCLASSES_USR)) {
		m->nocheck = true;
		return;
	}

	/*
	 * Allow recursive entrance.
	 */
	flags = dept_enter_recursive();

	clean_classes_cache(&m->map_key);

	m->keys = k;
	m->sub_u = sub_u;
	m->name = n;
	m->wgen = 0U;
	m->nocheck = !valid_key(k);

	dept_exit_recursive(flags);
}
EXPORT_SYMBOL_GPL(dept_map_init);

/*
 * Assume we don't have to consider race with the map when
 * dept_map_reinit() is called.
 */
void dept_map_reinit(struct dept_map *m, struct dept_key *k, int sub_u,
		     const char *n)
{
	unsigned long flags;

	if (unlikely(!dept_working())) {
		m->nocheck = true;
		return;
	}

	/*
	 * Allow recursive entrance.
	 */
	flags = dept_enter_recursive();

	if (k) {
		clean_classes_cache(&m->map_key);
		m->keys = k;
		m->nocheck = !valid_key(k);
	}

	if (sub_u >= 0 && sub_u < DEPT_MAX_SUBCLASSES_USR)
		m->sub_u = sub_u;

	if (n)
		m->name = n;

	m->wgen = 0U;

	dept_exit_recursive(flags);
}
EXPORT_SYMBOL_GPL(dept_map_reinit);

void dept_map_copy(struct dept_map *to, struct dept_map *from)
{
	if (unlikely(!dept_working())) {
		to->nocheck = true;
		return;
	}

	*to = *from;

	/*
	 * XXX: 'to' might be in a stack or something. Using the address
	 * in a stack segment as a key is meaningless. Just ignore the
	 * case for now.
	 */
	if (!to->keys) {
		to->nocheck = true;
		return;
	}

	/*
	 * Since the class cache can be modified concurrently we could
	 * observe half pointers (64bit arch using 32bit copy insns).
	 * Therefore clear the caches and take the performance hit.
	 *
	 * XXX: Doesn't work well with lockdep_set_class_and_subclass()
	 *      since that relies on cache abuse.
	 */
	clean_classes_cache(&to->map_key);
}

LIST_HEAD(dept_classes);

static bool within(const void *addr, void *start, unsigned long size)
{
	return addr >= start && addr < start + size;
}

void dept_free_range(void *start, unsigned int sz)
{
	struct dept_task *dt = dept_task();
	struct dept_class *c, *n;
	unsigned long flags;

	if (unlikely(!dept_working()))
		return;

	if (dt->recursive) {
		DEPT_STOP("Failed to successfully free Dept objects.\n");
		return;
	}

	flags = dept_enter();

	/*
	 * dept_free_range() should not fail.
	 *
	 * FIXME: Should be fixed if dept_free_range() causes deadlock
	 * with dept_lock().
	 */
	while (unlikely(!dept_lock()))
		cpu_relax();

	list_for_each_entry_safe(c, n, &dept_classes, all_node) {
		if (!within((void *)c->key, start, sz) &&
		    !within(c->name, start, sz))
			continue;

		hash_del_class(c);
		disconnect_class(c);
		list_del(&c->all_node);
		invalidate_class(c);

		/*
		 * Actual deletion will happen on the rcu callback
		 * that has been added in disconnect_class().
		 */
		del_class(c);
	}
	dept_unlock();
	dept_exit(flags);

	/*
	 * Wait until even lockless hash_lookup_class() for the class
	 * returns NULL.
	 */
	might_sleep();
	synchronize_rcu();
}

static int sub_id(struct dept_map *m, int e)
{
	return (m ? m->sub_u : 0) + e * DEPT_MAX_SUBCLASSES_USR;
}

static struct dept_class *check_new_class(struct dept_key *local,
					  struct dept_key *k, int sub_id,
					  const char *n, bool sched_map)
{
	struct dept_class *c = NULL;

	if (DEPT_WARN_ON(sub_id >= DEPT_MAX_SUBCLASSES))
		return NULL;

	if (DEPT_WARN_ON(!k))
		return NULL;

	/*
	 * XXX: Assume that users prevent the map from using if any of
	 * the cached keys has been invalidated. If not, the cache,
	 * local->classes should not be used because it would be racy
	 * with class deletion.
	 */
	if (local && sub_id < DEPT_MAX_SUBCLASSES_CACHE)
		c = READ_ONCE(local->classes[sub_id]);

	if (c)
		return c;

	c = lookup_class((unsigned long)k->base + sub_id);
	if (c)
		goto caching;

	if (unlikely(!dept_lock()))
		return NULL;

	c = lookup_class((unsigned long)k->base + sub_id);
	if (unlikely(c))
		goto unlock;

	c = new_class();
	if (unlikely(!c))
		goto unlock;

	c->name = n;
	c->sched_map = sched_map;
	c->sub_id = sub_id;
	c->key = (unsigned long)(k->base + sub_id);
	hash_add_class(c);
	list_add(&c->all_node, &dept_classes);
unlock:
	dept_unlock();
caching:
	if (local && sub_id < DEPT_MAX_SUBCLASSES_CACHE)
		WRITE_ONCE(local->classes[sub_id], c);

	return c;
}

/*
 * Called between dept_enter() and dept_exit().
 */
static void __dept_wait(struct dept_map *m, unsigned long w_f,
			unsigned long ip, const char *w_fn, int sub_l,
			bool sched_sleep, bool sched_map, bool timeout)
{
	int e;

	/*
	 * Be as conservative as possible. In case of mulitple waits for
	 * a single dept_map, we are going to keep only the last wait's
	 * wgen for simplicity - keeping all wgens seems overengineering.
	 *
	 * Of course, it might cause missing some dependencies that
	 * would rarely, probabily never, happen but it helps avoid
	 * false positive report.
	 */
	for_each_set_bit(e, &w_f, DEPT_MAX_SUBCLASSES_EVT) {
		struct dept_class *c;
		struct dept_key *k;

		k = m->keys ?: &m->map_key;
		c = check_new_class(&m->map_key, k,
				    sub_id(m, e), m->name, sched_map);
		if (!c)
			continue;

		add_wait(c, ip, w_fn, sub_l, sched_sleep, timeout);
	}
}

/*
 * Called between dept_enter() and dept_exit().
 */
static void __dept_event(struct dept_map *m, unsigned long e_f,
			 unsigned long ip, const char *e_fn,
			 bool sched_map)
{
	struct dept_class *c;
	struct dept_key *k;
	int e;

	e = find_first_bit(&e_f, DEPT_MAX_SUBCLASSES_EVT);

	if (DEPT_WARN_ON(e >= DEPT_MAX_SUBCLASSES_EVT))
		return;

	/*
	 * An event is an event. If the caller passed more than single
	 * event, then warn it and handle the event corresponding to
	 * the first bit anyway.
	 */
	DEPT_WARN_ON(1UL << e != e_f);

	k = m->keys ?: &m->map_key;
	c = check_new_class(&m->map_key, k, sub_id(m, e), m->name, sched_map);

	if (c && add_ecxt(m, c, 0UL, NULL, e_fn, 0)) {
		do_event(m, c, READ_ONCE(m->wgen), ip);
		pop_ecxt(m, c);
	}
}

void dept_wait(struct dept_map *m, unsigned long w_f,
	       unsigned long ip, const char *w_fn, int sub_l,
	       long timeoutval)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;
	bool timeout;

	if (unlikely(!dept_working()))
		return;

	timeout = timeoutval > 0 && timeoutval < MAX_SCHEDULE_TIMEOUT;

#if !defined(CONFIG_DEPT_AGGRESSIVE_TIMEOUT_WAIT)
	if (timeout)
		return;
#endif

	if (dt->recursive)
		return;

	if (m->nocheck)
		return;

	flags = dept_enter();

	__dept_wait(m, w_f, ip, w_fn, sub_l, false, false, timeout);

	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_wait);

void dept_stage_wait(struct dept_map *m, struct dept_key *k,
		     unsigned long ip, const char *w_fn,
		     long timeoutval)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;
	bool timeout;

	if (unlikely(!dept_working()))
		return;

	timeout = timeoutval > 0 && timeoutval < MAX_SCHEDULE_TIMEOUT;

#if !defined(CONFIG_DEPT_AGGRESSIVE_TIMEOUT_WAIT)
	if (timeout)
		return;
#endif

	if (m && m->nocheck)
		return;

	/*
	 * Either m or k should be passed. Which means Dept relies on
	 * either its own map or the caller's position in the code when
	 * determining its class.
	 */
	if (DEPT_WARN_ON(!m && !k))
		return;

	/*
	 * Allow recursive entrance.
	 */
	flags = dept_enter_recursive();

	/*
	 * Ensure the outmost dept_stage_wait() works.
	 */
	if (dt->stage_m.keys)
		goto exit;

	if (m) {
		dt->stage_m = *m;

		/*
		 * Ensure dt->stage_m.keys != NULL and it works with the
		 * map's map_key, not stage_m's one when ->keys == NULL.
		 */
		if (!m->keys)
			dt->stage_m.keys = &m->map_key;
	} else {
		dt->stage_m.name = w_fn;
		dt->stage_sched_map = true;
	}

	/*
	 * dept_map_reinit() includes WRITE_ONCE(->wgen, 0U) that
	 * effectively disables the map just in case real sleep won't
	 * happen. dept_request_event_wait_commit() will enable it.
	 */
	dept_map_reinit(&dt->stage_m, k, -1, NULL);

	dt->stage_w_fn = w_fn;
	dt->stage_ip = ip;
	dt->stage_timeout = timeout;
exit:
	dept_exit_recursive(flags);
}
EXPORT_SYMBOL_GPL(dept_stage_wait);

static void __dept_clean_stage(struct dept_task *dt)
{
	memset(&dt->stage_m, 0x0, sizeof(struct dept_map));
	dt->stage_sched_map = false;
	dt->stage_w_fn = NULL;
	dt->stage_ip = 0UL;
	dt->stage_timeout = false;
}

void dept_clean_stage(void)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;

	if (unlikely(!dept_working()))
		return;

	/*
	 * Allow recursive entrance.
	 */
	flags = dept_enter_recursive();
	__dept_clean_stage(dt);
	dept_exit_recursive(flags);
}
EXPORT_SYMBOL_GPL(dept_clean_stage);

/*
 * Always called from __schedule().
 */
void dept_request_event_wait_commit(void)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;
	unsigned int wg;
	unsigned long ip;
	const char *w_fn;
	bool sched_map;
	bool timeout;

	if (unlikely(!dept_working()))
		return;

	/*
	 * It's impossible that __schedule() is called while Dept is
	 * working that already disabled IRQ at the entrance.
	 */
	if (DEPT_WARN_ON(dt->recursive))
		return;

	flags = dept_enter();

	/*
	 * Checks if current has staged a wait.
	 */
	if (!dt->stage_m.keys)
		goto exit;

	w_fn = dt->stage_w_fn;
	ip = dt->stage_ip;
	sched_map = dt->stage_sched_map;
	timeout = dt->stage_timeout;

	wg = next_wgen();
	WRITE_ONCE(dt->stage_m.wgen, wg);

	__dept_wait(&dt->stage_m, 1UL, ip, w_fn, 0, true, sched_map, timeout);
exit:
	dept_exit(flags);
}

/*
 * Always called from try_to_wake_up().
 */
void dept_stage_event(struct task_struct *requestor, unsigned long ip)
{
	struct dept_task *dt = dept_task();
	struct dept_task *dt_req = &requestor->dept_task;
	unsigned long flags;
	struct dept_map m;
	bool sched_map;

	if (unlikely(!dept_working()))
		return;

	if (dt->recursive)
		return;

	flags = dept_enter();

	/*
	 * Serializing is unnecessary as long as it always comes from
	 * try_to_wake_up().
	 */
	m = dt_req->stage_m;
	sched_map = dt_req->stage_sched_map;
	__dept_clean_stage(dt_req);

	/*
	 * ->stage_m.keys should not be NULL if it's in use. Should
	 * make sure that it's not NULL when staging a valid map.
	 */
	if (!m.keys)
		goto exit;

	__dept_event(&m, 1UL, ip, "try_to_wake_up", sched_map);
exit:
	dept_exit(flags);
}

/*
 * Modifies the latest ecxt corresponding to m and e_f.
 */
void dept_map_ecxt_modify(struct dept_map *m, unsigned long e_f,
			  struct dept_key *new_k, unsigned long new_e_f,
			  unsigned long new_ip, const char *new_c_fn,
			  const char *new_e_fn, int new_sub_l)
{
	struct dept_task *dt = dept_task();
	struct dept_ecxt_held *eh;
	struct dept_class *c;
	struct dept_key *k;
	unsigned long flags;
	int pos = -1;
	int new_e;
	int e;

	if (unlikely(!dept_working()))
		return;

	/*
	 * XXX: Couldn't handle re-enterance cases. Ingore it for now.
	 */
	if (dt->recursive)
		return;

	/*
	 * Should go ahead no matter whether ->nocheck == true or not
	 * because ->nocheck value can be changed within the ecxt area
	 * delimitated by dept_ecxt_enter() and dept_ecxt_exit().
	 */

	flags = dept_enter();

	for_each_set_bit(e, &e_f, DEPT_MAX_SUBCLASSES_EVT) {
		k = m->keys ?: &m->map_key;
		c = check_new_class(&m->map_key, k,
				    sub_id(m, e), m->name, false);
		if (!c)
			continue;

		/*
		 * When it found an ecxt for any event in e_f, done.
		 */
		pos = find_ecxt_pos(m, c, true);
		if (pos != -1)
			break;
	}

	if (unlikely(pos == -1))
		goto exit;

	eh = dt->ecxt_held + pos;
	new_sub_l = new_sub_l >= 0 ? new_sub_l : eh->sub_l;

	new_e = find_first_bit(&new_e_f, DEPT_MAX_SUBCLASSES_EVT);

	if (new_e < DEPT_MAX_SUBCLASSES_EVT)
		/*
		 * Let it work with the first bit anyway.
		 */
		DEPT_WARN_ON(1UL << new_e != new_e_f);
	else
		new_e = e;

	pop_ecxt(m, c);

	/*
	 * Apply the key to the map.
	 */
	if (new_k)
		dept_map_reinit(m, new_k, -1, NULL);

	k = m->keys ?: &m->map_key;
	c = check_new_class(&m->map_key, k, sub_id(m, new_e), m->name, false);

	if (c && add_ecxt(m, c, new_ip, new_c_fn, new_e_fn, new_sub_l))
		goto exit;

	/*
	 * Successfully pop_ecxt()ed but failed to add_ecxt().
	 */
	dt->missing_ecxt++;
exit:
	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_map_ecxt_modify);

void dept_ecxt_enter(struct dept_map *m, unsigned long e_f, unsigned long ip,
		     const char *c_fn, const char *e_fn, int sub_l)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;
	struct dept_class *c;
	struct dept_key *k;
	int e;

	if (unlikely(!dept_working()))
		return;

	if (dt->recursive) {
		dt->missing_ecxt++;
		return;
	}

	/*
	 * Should go ahead no matter whether ->nocheck == true or not
	 * because ->nocheck value can be changed within the ecxt area
	 * delimitated by dept_ecxt_enter() and dept_ecxt_exit().
	 */

	flags = dept_enter();

	e = find_first_bit(&e_f, DEPT_MAX_SUBCLASSES_EVT);

	if (e >= DEPT_MAX_SUBCLASSES_EVT)
		goto missing_ecxt;

	/*
	 * An event is an event. If the caller passed more than single
	 * event, then warn it and handle the event corresponding to
	 * the first bit anyway.
	 */
	DEPT_WARN_ON(1UL << e != e_f);

	k = m->keys ?: &m->map_key;
	c = check_new_class(&m->map_key, k, sub_id(m, e), m->name, false);

	if (c && add_ecxt(m, c, ip, c_fn, e_fn, sub_l))
		goto exit;
missing_ecxt:
	dt->missing_ecxt++;
exit:
	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_ecxt_enter);

bool dept_ecxt_holding(struct dept_map *m, unsigned long e_f)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;
	bool ret = false;
	int e;

	if (unlikely(!dept_working()))
		return false;

	if (dt->recursive)
		return false;

	flags = dept_enter();

	for_each_set_bit(e, &e_f, DEPT_MAX_SUBCLASSES_EVT) {
		struct dept_class *c;
		struct dept_key *k;

		k = m->keys ?: &m->map_key;
		c = check_new_class(&m->map_key, k,
				    sub_id(m, e), m->name, false);
		if (!c)
			continue;

		if (find_ecxt_pos(m, c, true) != -1) {
			ret = true;
			break;
		}
	}

	dept_exit(flags);

	return ret;
}
EXPORT_SYMBOL_GPL(dept_ecxt_holding);

void dept_request_event(struct dept_map *m)
{
	unsigned long flags;
	unsigned int wg;

	if (unlikely(!dept_working()))
		return;

	if (m->nocheck)
		return;

	/*
	 * Allow recursive entrance.
	 */
	flags = dept_enter_recursive();

	wg = next_wgen();
	WRITE_ONCE(m->wgen, wg);

	dept_exit_recursive(flags);
}
EXPORT_SYMBOL_GPL(dept_request_event);

void dept_event(struct dept_map *m, unsigned long e_f,
		unsigned long ip, const char *e_fn)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;

	if (unlikely(!dept_working()))
		return;

	if (m->nocheck)
		return;

	if (dt->recursive) {
		/*
		 * Dept won't work with this even though an event
		 * context has been asked. Don't make it confused at
		 * handling the event. Disable it until the next.
		 */
		WRITE_ONCE(m->wgen, 0U);
		return;
	}

	flags = dept_enter();

	__dept_event(m, e_f, ip, e_fn, false);

	/*
	 * Keep the map diabled until the next sleep.
	 */
	WRITE_ONCE(m->wgen, 0U);

	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_event);

void dept_ecxt_exit(struct dept_map *m, unsigned long e_f,
		    unsigned long ip)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;
	int e;

	if (unlikely(!dept_working()))
		return;

	if (dt->recursive) {
		dt->missing_ecxt--;
		return;
	}

	/*
	 * Should go ahead no matter whether ->nocheck == true or not
	 * because ->nocheck value can be changed within the ecxt area
	 * delimitated by dept_ecxt_enter() and dept_ecxt_exit().
	 */

	flags = dept_enter();

	for_each_set_bit(e, &e_f, DEPT_MAX_SUBCLASSES_EVT) {
		struct dept_class *c;
		struct dept_key *k;

		k = m->keys ?: &m->map_key;
		c = check_new_class(&m->map_key, k,
				    sub_id(m, e), m->name, false);
		if (!c)
			continue;

		/*
		 * When it found an ecxt for any event in e_f, done.
		 */
		if (pop_ecxt(m, c))
			goto exit;
	}

	dt->missing_ecxt--;
exit:
	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_ecxt_exit);

void dept_task_exit(struct task_struct *t)
{
	struct dept_task *dt = &t->dept_task;
	int i;

	if (unlikely(!dept_working()))
		return;

	raw_local_irq_disable();

	if (dt->stack) {
		put_stack(dt->stack);
		dt->stack = NULL;
	}

	for (i = 0; i < dt->ecxt_held_pos; i++) {
		if (dt->ecxt_held[i].class) {
			put_class(dt->ecxt_held[i].class);
			dt->ecxt_held[i].class = NULL;
		}
		if (dt->ecxt_held[i].ecxt) {
			put_ecxt(dt->ecxt_held[i].ecxt);
			dt->ecxt_held[i].ecxt = NULL;
		}
	}

	for (i = 0; i < DEPT_MAX_WAIT_HIST; i++) {
		if (dt->wait_hist[i].wait) {
			put_wait(dt->wait_hist[i].wait);
			dt->wait_hist[i].wait = NULL;
		}
	}

	dt->task_exit = true;
	dept_off();

	raw_local_irq_enable();
}

void dept_task_init(struct task_struct *t)
{
	memset(&t->dept_task, 0x0, sizeof(struct dept_task));
}

void dept_key_init(struct dept_key *k)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;
	int sub_id;

	if (unlikely(!dept_working()))
		return;

	if (dt->recursive) {
		DEPT_STOP("Key initialization fails.\n");
		return;
	}

	flags = dept_enter();

	clean_classes_cache(k);

	/*
	 * dept_key_init() should not fail.
	 *
	 * FIXME: Should be fixed if dept_key_init() causes deadlock
	 * with dept_lock().
	 */
	while (unlikely(!dept_lock()))
		cpu_relax();

	for (sub_id = 0; sub_id < DEPT_MAX_SUBCLASSES; sub_id++) {
		struct dept_class *c;

		c = lookup_class((unsigned long)k->base + sub_id);
		if (!c)
			continue;

		DEPT_STOP("The class(%s/%d) has not been removed.\n",
			  c->name, sub_id);
		break;
	}

	dept_unlock();
	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_key_init);

void dept_key_destroy(struct dept_key *k)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;
	int sub_id;

	if (unlikely(!dept_working()))
		return;

	if (dt->recursive == 1 && dt->task_exit) {
		/*
		 * Need to allow to go ahead in this case where
		 * ->recursive has been set to 1 by dept_off() in
		 * dept_task_exit() and ->task_exit has been set to
		 * true in dept_task_exit().
		 */
	} else if (dt->recursive) {
		DEPT_STOP("Key destroying fails.\n");
		return;
	}

	flags = dept_enter();

	/*
	 * dept_key_destroy() should not fail.
	 *
	 * FIXME: Should be fixed if dept_key_destroy() causes deadlock
	 * with dept_lock().
	 */
	while (unlikely(!dept_lock()))
		cpu_relax();

	for (sub_id = 0; sub_id < DEPT_MAX_SUBCLASSES; sub_id++) {
		struct dept_class *c;

		c = lookup_class((unsigned long)k->base + sub_id);
		if (!c)
			continue;

		hash_del_class(c);
		disconnect_class(c);
		list_del(&c->all_node);
		invalidate_class(c);

		/*
		 * Actual deletion will happen on the rcu callback
		 * that has been added in disconnect_class().
		 */
		del_class(c);
	}

	dept_unlock();
	dept_exit(flags);

	/*
	 * Wait until even lockless hash_lookup_class() for the class
	 * returns NULL.
	 */
	might_sleep();
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(dept_key_destroy);

static void move_llist(struct llist_head *to, struct llist_head *from)
{
	struct llist_node *first = llist_del_all(from);
	struct llist_node *last;

	if (!first)
		return;

	for (last = first; last->next; last = last->next);
	llist_add_batch(first, last, to);
}

static void migrate_per_cpu_pool(void)
{
	const int boot_cpu = 0;
	int i;

	/*
	 * The boot CPU has been using the temperal local pool so far.
	 * From now on that per_cpu areas have been ready, use the
	 * per_cpu local pool instead.
	 */
	DEPT_WARN_ON(smp_processor_id() != boot_cpu);
	for (i = 0; i < OBJECT_NR; i++) {
		struct llist_head *from;
		struct llist_head *to;

		from = &dept_pool[i].boot_pool;
		to = per_cpu_ptr(dept_pool[i].lpool, boot_cpu);
		move_llist(to, from);
	}
}

#define B2KB(B) ((B) / 1024)

/*
 * Should be called after setup_per_cpu_areas() and before no non-boot
 * CPUs have been on.
 */
void __init dept_init(void)
{
	size_t mem_total = 0;

	local_irq_disable();
	dept_per_cpu_ready = 1;
	migrate_per_cpu_pool();
	local_irq_enable();

#define HASH(id, bits) BUILD_BUG_ON(1 << (bits) <= 0);
	#include "dept_hash.h"
#undef  HASH
#define OBJECT(id, nr) mem_total += sizeof(struct dept_##id) * nr;
	#include "dept_object.h"
#undef  OBJECT
#define HASH(id, bits) mem_total += sizeof(struct hlist_head) * (1 << (bits));
	#include "dept_hash.h"
#undef  HASH

	pr_info("DEPendency Tracker: Copyright (c) 2020 LG Electronics, Inc., Byungchul Park\n");
	pr_info("... DEPT_MAX_STACK_ENTRY: %d\n", DEPT_MAX_STACK_ENTRY);
	pr_info("... DEPT_MAX_WAIT_HIST  : %d\n", DEPT_MAX_WAIT_HIST);
	pr_info("... DEPT_MAX_ECXT_HELD  : %d\n", DEPT_MAX_ECXT_HELD);
	pr_info("... DEPT_MAX_SUBCLASSES : %d\n", DEPT_MAX_SUBCLASSES);
#define OBJECT(id, nr)							\
	pr_info("... memory initially used by %s: %zu KB\n",		\
	       #id, B2KB(sizeof(spool_##id) + sizeof(rpool_##id)));
	#include "dept_object.h"
#undef  OBJECT
#define HASH(id, bits)							\
	pr_info("... hash list head used by %s: %zu KB\n",		\
	       #id, B2KB(sizeof(struct hlist_head) * (1 << (bits))));
	#include "dept_hash.h"
#undef  HASH
	pr_info("... total memory initially used by objects and hashs: %zu KB\n", B2KB(mem_total));
	pr_info("... per task memory footprint: %zu bytes\n", sizeof(struct dept_task));
}

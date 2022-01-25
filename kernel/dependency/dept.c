/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Dept(DEPendency Tracker) - Runtime dependency tracker
 *
 * Started by Byungchul Park <max.byungchul.park@gmail.com>:
 *
 *  Copyright (c) 2020 LG Electronics, Inc., Byungchul Park
 *
 * Dept provides a general way to detect deadlock possibility in runtime
 * and the interest is not limited to typical lock but to every
 * syncronization primitives.
 *
 * The following ideas were borrowed from Lockdep:
 *
 *    1) Use a graph to track relationship between classes.
 *    2) Prevent performance regression using hash.
 *
 * The following items were enhanced from Lockdep:
 *
 *    1) Cover more deadlock cases.
 *    2) Allow muliple reports.
 *
 * TODO: Both Lockdep and Dept should co-exist until Dept is considered
 * stable. Then the dependency check routine shoud be replaced with Dept
 * after. It should finally look like:
 *
 *
 *
 * As is:
 *
 *    Lockdep
 *    +-----------------------------------------+
 *    | Lock usage correctness check            | <-> locks
 *    |                                         |
 *    |                                         |
 *    | +-------------------------------------+ |
 *    | | Dependency check                    | |
 *    | | (by tracking lock aquisition order) | |
 *    | +-------------------------------------+ |
 *    |                                         |
 *    +-----------------------------------------+
 *
 *    Dept
 *    +-----------------------------------------+
 *    | Dependency check                        | <-> waits/events
 *    | (by tracking wait and event context)    |
 *    +-----------------------------------------+
 *
 *
 *
 * To be:
 *
 *    Lockdep
 *    +-----------------------------------------+
 *    | Lock usage correctness check            | <-> locks
 *    |                                         |
 *    |                                         |
 *    |       (Request dependency check)        |
 *    |                    T                    |
 *    +--------------------|--------------------+
 *                         |
 *    Dept                 V
 *    +-----------------------------------------+
 *    | Dependency check                        | <-> waits/events
 *    | (by tracking wait and event context)    |
 *    +-----------------------------------------+
 *
 *
 *
 * ---
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your ootion) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 */

#include <linux/sched.h>
#include <linux/stacktrace.h>
#include <linux/spinlock.h>
#include <linux/kallsyms.h>
#include <linux/hash.h>
#include <linux/dept.h>
#include <linux/utsname.h>
#include "dept_internal.h"

static int dept_stop;
static int dept_per_cpu_ready;

/*
 * Make all operations using DEPT_WARN_ON() fail on oops_in_progress and
 * prevent warning message.
 */
#define DEPT_WARN_ON_ONCE(c)						\
	({								\
		int __ret = 1;						\
		if (likely(!oops_in_progress))				\
			__ret = WARN_ONCE(c, "DEPT_WARN_ON_ONCE: " #c);	\
		__ret;							\
	})

#define DEPT_WARN_ONCE(s...)						\
	({								\
		if (likely(!oops_in_progress))				\
			WARN_ONCE(1, "DEPT_WARN_ONCE: " s);		\
	})

#define DEPT_WARN_ON(c)							\
	({								\
		int __ret = 1;						\
		if (likely(!oops_in_progress))				\
			__ret = WARN(c, "DEPT_WARN_ON: " #c);		\
		__ret;							\
	})

#define DEPT_WARN(s...)							\
	({								\
		if (likely(!oops_in_progress))				\
			WARN(1, "DEPT_WARN: " s);			\
	})

#define DEPT_STOP(s...)							\
	({								\
		WRITE_ONCE(dept_stop, 1);				\
		if (likely(!oops_in_progress))				\
			WARN(1, "DEPT_STOP: " s);			\
	})

static arch_spinlock_t dept_spin = (arch_spinlock_t)__ARCH_SPIN_LOCK_UNLOCKED;

/*
 * Dept internal engine should be careful in using outside functions
 * e.g. printk at reporting since that kind of usage might cause
 * untrackable deadlock.
 */
static atomic_t dept_outworld = ATOMIC_INIT(0);

static inline void dept_outworld_enter(void)
{
	atomic_inc(&dept_outworld);
}

static inline void dept_outworld_exit(void)
{
	atomic_dec(&dept_outworld);
}

static inline bool dept_outworld_entered(void)
{
	return atomic_read(&dept_outworld);
}

static inline bool dept_lock(void)
{
	while (!arch_spin_trylock(&dept_spin))
		if (unlikely(dept_outworld_entered()))
			return false;
	return true;
}

static inline void dept_unlock(void)
{
	arch_spin_unlock(&dept_spin);
}

/*
 * whether to stack-trace on every wait or every ecxt
 */
static bool rich_stack = true;

enum bfs_ret {
	DEPT_BFS_CONTINUE,
	DEPT_BFS_CONTINUE_REV,
	DEPT_BFS_DONE,
	DEPT_BFS_SKIP,
};

/*
 * The irq wait is in unknown state. Should identify the state.
 */
#define DEPT_IWAIT_UNKNOWN ((void *)1)

static inline bool before(unsigned int a, unsigned int b)
{
	return (int)(a - b) < 0;
}

static inline bool valid_stack(struct dept_stack *s)
{
	return s && s->nr > 0;
}

static inline bool valid_class(struct dept_class *c)
{
	return c->key;
}

static inline void invalidate_class(struct dept_class *c)
{
	c->key = 0UL;
}

static inline struct dept_class *dep_fc(struct dept_dep *d)
{
	return d->ecxt->class;
}

static inline struct dept_class *dep_tc(struct dept_dep *d)
{
	return d->wait->class;
}

static inline const char *irq_str(int irq)
{
	if (irq == DEPT_SIRQ)
		return "softirq";
	if (irq == DEPT_HIRQ)
		return "hardirq";
	return "(unknown)";
}

static inline struct dept_task *dept_task(void)
{
	return &current->dept_task;
}

/*
 * Pool
 * =====================================================================
 * Dept maintains pools to provide objects in a safe way.
 *
 *    1) Static pool is used at the beginning of booting time.
 *    2) Local pool is tried first before the static pool. Objects that
 *       have been freed will be placed.
 */

#define OBJECT(id, nr)							\
static struct dept_##id spool_##id[nr];					\
static DEFINE_PER_CPU(struct llist_head, lpool_##id);
	#include "dept_object.h"
#undef  OBJECT

struct dept_pool dept_pool[OBJECT_NR] = {
#define OBJECT(id, nr) {						\
	.name = #id,							\
	.obj_sz = sizeof(struct dept_##id),				\
	.obj_nr = ATOMIC_INIT(nr),					\
	.node_off = offsetof(struct dept_##id, pool_node),		\
	.spool = spool_##id,						\
	.lpool = &lpool_##id, },
	#include "dept_object.h"
#undef  OBJECT
};

/*
 * Can use llist no matter whether CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG is
 * enabled because Dept never race with NMI by nesting control.
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
	if (atomic_read(&p->obj_nr) > 0) {
		int idx = atomic_dec_return(&p->obj_nr);
		if (idx >= 0)
			return p->spool + (idx * p->obj_sz);
	}

	DEPT_WARN_ONCE("Pool(%s) is empty.\n", p->name);
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
static inline struct dept_##id *new_##id(void)				\
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
static inline struct dept_##id *get_##id(struct dept_##id *a)		\
{									\
	atomic_inc(&a->ref);						\
	return a;							\
}									\
									\
static inline void put_##id(struct dept_##id *a)			\
{									\
	if (!atomic_dec_return(&a->ref)) {				\
		if (dtor_##id)						\
			dtor_##id(a);					\
		to_pool(a, OBJECT_##id);				\
	}								\
}									\
									\
static inline void del_##id(struct dept_##id *a)			\
{									\
	put_##id(a);							\
}									\
									\
static inline bool id##_refered(struct dept_##id *a, int expect)	\
{									\
	return a && atomic_read(&a->ref) > expect;			\
}
#include "dept_object.h"
#undef  OBJECT

#define SET_CONSTRUCTOR(id, f) \
static void (*ctor_##id)(struct dept_##id *a) = f;

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

	for (i = 0; i < DEPT_IRQS_NR; i++) {
		c->iwait_dist[i] = INT_MAX;
		WRITE_ONCE(c->iecxt[i], NULL);
		WRITE_ONCE(c->iwait[i], NULL);
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

	for (i = 0; i < DEPT_IRQS_NR; i++) {
		e->enirq_stack[i] = NULL;
		e->enirq_ip[i] = 0UL;
	}
	e->ecxt_ip = 0UL;
	e->ecxt_stack = NULL;
	e->enirqf = 0UL;
	e->event_stack = NULL;
}
SET_CONSTRUCTOR(ecxt, initialize_ecxt);

static void initialize_wait(struct dept_wait *w)
{
	int i;

	for (i = 0; i < DEPT_IRQS_NR; i++) {
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
static void (*dtor_##id)(struct dept_##id *a) = f;

static void destroy_dep(struct dept_dep *d)
{
	if (d->ecxt)
		put_ecxt(d->ecxt);
	if (d->wait)
		put_wait(d->wait);
}
SET_DESTRUCTOR(dep, destroy_dep);

static void destroy_ecxt(struct dept_ecxt *e)
{
	int i;

	for (i = 0; i < DEPT_IRQS_NR; i++)
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

	for (i = 0; i < DEPT_IRQS_NR; i++)
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
 * Dept makes use of caching and hashing to improve performance. Each
 * object can be obtained in O(1) with its key.
 *
 * NOTE: Currently we assume all the objects in the hashs will never be
 * removed. Implement it when needed.
 */

/*
 * Some information might be lost but it's only for hashing key.
 */
static inline unsigned long mix(unsigned long a, unsigned long b)
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

static bool cmp_staleiw(struct dept_staleiw *s1, struct dept_staleiw *s2)
{
	return s1->ip == s2->ip && s1->irq == s2->irq;
}

static unsigned long key_staleiw(struct dept_staleiw *s)
{
	return s->ip ^ (1UL << s->irq);
}

static bool cmp_staleie(struct dept_staleie *s1, struct dept_staleie *s2)
{
	return s1->ip == s2->ip && s1->irq == s2->irq;
}

static unsigned long key_staleie(struct dept_staleie *s)
{
	return s->ip ^ (1UL << s->irq);
}

#define HASH(id, bits)							\
static struct hlist_head table_##id[1UL << bits];			\
									\
static inline struct hlist_head *head_##id(struct dept_##id *a)		\
{									\
	return table_##id + hash_long(key_##id(a), bits);		\
}									\
									\
static inline struct dept_##id *hash_lookup_##id(struct dept_##id *a)	\
{									\
	struct dept_##id *b;						\
	hlist_for_each_entry_rcu(b, head_##id(a), hash_node)		\
		if (cmp_##id(a, b))					\
			return b;					\
	return NULL;							\
}									\
									\
static inline void hash_add_##id(struct dept_##id *a)			\
{									\
	hlist_add_head_rcu(&a->hash_node, head_##id(a));		\
}									\
									\
static inline void hash_del_##id(struct dept_##id *a)			\
{									\
	hlist_del_rcu(&a->hash_node);					\
}
#include "dept_hash.h"
#undef  HASH

static inline struct dept_dep *dep(struct dept_class *fc,
				   struct dept_class *tc)
{
	struct dept_ecxt onetime_e = { .class = fc };
	struct dept_wait onetime_w = { .class = tc };
	struct dept_dep  onetime_d = { .ecxt = &onetime_e,
				       .wait = &onetime_w };
	return hash_lookup_dep(&onetime_d);
}

static inline struct dept_class *class(unsigned long key)
{
	struct dept_class onetime_c = { .key = key };
	return hash_lookup_class(&onetime_c);
}

static inline struct dept_staleiw *staleiw(unsigned long ip, int irq)
{
	struct dept_staleiw onetime_s = { .ip = ip, .irq = irq };
	return hash_lookup_staleiw(&onetime_s);
}

static inline struct dept_staleie *staleie(unsigned long ip, int irq)
{
	struct dept_staleie onetime_s = { .ip = ip, .irq = irq };
	return hash_lookup_staleie(&onetime_s);
}

/*
 * Report
 * =====================================================================
 * Dept prints useful information to help debuging on detection of
 * problematic dependency.
 */

static inline void print_ip_stack(unsigned long ip, struct dept_stack *s)
{
	if (ip)
		print_ip_sym(KERN_WARNING, ip);

	if (valid_stack(s)) {
		printk("stacktrace:\n");
		stack_trace_print(s->raw, s->nr, 5);
	}

	if (!ip && !valid_stack(s))
		printk("(N/A)\n");
}

#define print_spc(spc, fmt, ...)					\
	printk("%*c" fmt, (spc) * 4, ' ', ##__VA_ARGS__)

static void print_diagram(struct dept_dep *d)
{
	struct dept_ecxt *e = d->ecxt;
	struct dept_wait *w = d->wait;
	struct dept_class *fc = dep_fc(d);
	struct dept_class *tc = dep_tc(d);
	unsigned long irqf;
	int irq;
	bool firstline = true;
	int spc = 1;
	const char *w_fn = w->wait_fn  ?: "(unknown)";
	const char *e_fn = e->event_fn ?: "(unknown)";
	const char *c_fn = e->ecxt_fn ?: "(unknown)";

	irqf = e->enirqf & w->irqf;

	for_each_set_bit(irq, &irqf, DEPT_IRQS_NR) {
		if (!firstline)
			printk("\nor\n\n");
		firstline = false;

		print_spc(spc, "[S] %s(%s:%d)\n", c_fn, fc->name, fc->sub);
		print_spc(spc, "    <%s interrupt>\n", irq_str(irq));
		print_spc(spc + 1, "[W] %s(%s:%d)\n", w_fn, tc->name, tc->sub);
		print_spc(spc, "[E] %s(%s:%d)\n", e_fn, fc->name, fc->sub);
	}

	if (!irqf) {
		print_spc(spc, "[S] %s(%s:%d)\n", c_fn, fc->name, fc->sub);
		print_spc(spc, "[W] %s(%s:%d)\n", w_fn, tc->name, tc->sub);
		print_spc(spc, "[E] %s(%s:%d)\n", e_fn, fc->name, fc->sub);
	}
}

static void print_dep(struct dept_dep *d)
{
	struct dept_ecxt *e = d->ecxt;
	struct dept_wait *w = d->wait;
	struct dept_class *fc = dep_fc(d);
	struct dept_class *tc = dep_tc(d);
	unsigned long irqf;
	int irq;
	const char *w_fn = w->wait_fn  ?: "(unknown)";
	const char *e_fn = e->event_fn ?: "(unknown)";
	const char *c_fn = e->ecxt_fn ?: "(unknown)";

	irqf = e->enirqf & w->irqf;
	for_each_set_bit(irq, &irqf, DEPT_IRQS_NR) {
		printk("%s has been enabled:\n", irq_str(irq));
		print_ip_stack(e->enirq_ip[irq], e->enirq_stack[irq]);
		printk("\n");

		printk("[S] %s(%s:%d):\n", c_fn, fc->name, fc->sub);
		print_ip_stack(e->ecxt_ip, e->ecxt_stack);
		printk("\n");

		printk("[W] %s(%s:%d) in %s context:\n",
		       w_fn, tc->name, tc->sub, irq_str(irq));
		print_ip_stack(w->irq_ip[irq], w->irq_stack[irq]);
		printk("\n");

		printk("[E] %s(%s:%d):\n", e_fn, fc->name, fc->sub);
		print_ip_stack(e->event_ip, e->event_stack);
	}

	if (!irqf) {
		printk("[S] %s(%s:%d):\n", c_fn, fc->name, fc->sub);
		print_ip_stack(e->ecxt_ip, e->ecxt_stack);
		printk("\n");

		printk("[W] %s(%s:%d):\n", w_fn, tc->name, tc->sub);
		print_ip_stack(w->wait_ip, w->wait_stack);
		printk("\n");

		printk("[E] %s(%s:%d):\n", e_fn, fc->name, fc->sub);
		print_ip_stack(e->event_ip, e->event_stack);
	}
}

static void save_current_stack(int skip);

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

	printk("===================================================\n");
	printk("Dept: Circular dependency has been detected.\n");
	printk("%s %.*s %s\n", init_utsname()->release,
		(int)strcspn(init_utsname()->version, " "),
		init_utsname()->version,
		print_tainted());
	printk("---------------------------------------------------\n");
	printk("summary\n");
	printk("---------------------------------------------------\n");

	if (fc == tc)
		printk("*** AA DEADLOCK ***\n\n");
	else
		printk("*** DEADLOCK ***\n\n");

	i = 0;
	do {
		struct dept_dep *d = dep(fc, tc);

		printk("context %c\n", 'A' + (i++));
		print_diagram(d);
		if (fc != c)
			printk("\n");

		tc = fc;
		fc = fc->bfs_parent;
	} while (tc != c);

	printk("\n");
	printk("[S]: start of the event context\n");
	printk("[W]: the wait blocked\n");
	printk("[E]: the event not reachable\n");

	i = 0;
	do {
		struct dept_dep *d = dep(fc, tc);

		printk("---------------------------------------------------\n");
		printk("context %c's detail\n", 'A' + i);
		printk("---------------------------------------------------\n");
		printk("context %c\n", 'A' + (i++));
		print_diagram(d);
		printk("\n");
		print_dep(d);

		tc = fc;
		fc = fc->bfs_parent;
	} while (tc != c);

	printk("---------------------------------------------------\n");
	printk("information that might be helpful\n");
	printk("---------------------------------------------------\n");
	dump_stack();

	dept_outworld_exit();
}

/*
 * BFS(Breadth First Search)
 * =====================================================================
 * Whenever a new dependency is added into the graph, search the graph
 * for a new circular dependency.
 */

static inline void enqueue(struct list_head *h, struct dept_dep *d)
{
	list_add_tail(&d->bfs_node, h);
}

static inline struct dept_dep *dequeue(struct list_head *h)
{
	struct dept_dep *d;
	d = list_first_entry(h, struct dept_dep, bfs_node);
	list_del(&d->bfs_node);
	return d;
}

static inline bool empty(struct list_head *h)
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
	if (ret == DEPT_BFS_DONE)
		return;
	if (ret == DEPT_BFS_SKIP)
		return;
	if (ret == DEPT_BFS_CONTINUE)
		extend_queue(&q, c);
	if (ret == DEPT_BFS_CONTINUE_REV)
		extend_queue_rev(&q, c);

	while (!empty(&q)) {
		struct dept_dep *d = dequeue(&q);

		ret = cb(d, in, out);
		if (ret == DEPT_BFS_DONE)
			break;
		if (ret == DEPT_BFS_SKIP)
			continue;
		if (ret == DEPT_BFS_CONTINUE)
			extend_queue(&q, dep_tc(d));
		if (ret == DEPT_BFS_CONTINUE_REV)
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

static inline unsigned long cur_enirqf(void);
static inline int cur_irq(void);
static inline unsigned int cur_ctxt_id(void);

static inline struct dept_stack *get_current_stack(void)
{
	struct dept_stack *s = dept_task()->stack;
	return s ? get_stack(s) : NULL;
}

static inline void prepare_current_stack(void)
{
	struct dept_stack *s = dept_task()->stack;
	/*
	 * The reference counter would be 1 when no one refers to the
	 * dept_stack.
	 */
	const int expect_ref = 1;

	/*
	 * The dept_stack is already ready.
	 */
	if (s && !stack_refered(s, expect_ref)) {
		s->nr = 0;
		return;
	}

	if (s)
		put_stack(s);

	s = dept_task()->stack = new_stack();
	if (!s)
		return;

	get_stack(s);
	del_stack(s);
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
	/*
	 * The reference counter would be 1 when no one refers to the
	 * dept_stack.
	 */
	const int expect_ref = 1;

	if (s && stack_refered(s, expect_ref))
		save_current_stack(2);
}

/*
 * FIXME: For now, disable Lockdep while Dept is working.
 *
 * Both Lockdep and Dept report it on a deadlock detection using
 * printk taking the risk of another deadlock that might be caused by
 * locks of console or printk between inside and outside of them.
 *
 * For Dept, it's no problem since multiple reports are allowed. But it
 * would be a bad idea for Lockdep since it will stop even on a singe
 * report. So we need to prevent Lockdep from its reporting the risk
 * Dept would take when reporting something.
 */
#include <linux/lockdep.h>

void dept_off(void)
{
	dept_task()->recursive++;
	lockdep_off();
}

void dept_on(void)
{
	dept_task()->recursive--;
	lockdep_on();
}

static inline unsigned long dept_enter(void)
{
	unsigned long flags;
	raw_local_irq_save(flags);
	dept_off();
	prepare_current_stack();
	return flags;
}

static inline void dept_exit(unsigned long flags)
{
	finish_current_stack();
	dept_on();
	raw_local_irq_restore(flags);
}

/*
 * NOTE: Must be called with dept_lock held.
 */
static struct dept_dep *__add_dep(struct dept_ecxt *e,
				  struct dept_wait *w)
{
	struct dept_dep *d;

	if (!valid_class(e->class) || !valid_class(w->class))
		return NULL;

	if (dep(e->class, w->class))
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
	list_add(&d->dep_node    , &dep_fc(d)->dep_head    );
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
			return DEPT_BFS_CONTINUE;

		/*
		 * AA circle does not make additional deadlock. We don't
		 * have to continue this BFS search.
		 */
		print_circle(dep_tc(new));
		return DEPT_BFS_DONE;
	}

	/*
	 * Allow multiple reports.
	 */
	if (dep_tc(d) == dep_fc(new))
		print_circle(dep_tc(new));

	return DEPT_BFS_CONTINUE;
}

/*
 * This function is actually in charge of reporting.
 */
static inline void check_dl(struct dept_dep *d)
{
	bfs(dep_tc(d), cb_check_dl, (void *)d, NULL);
}

/*
 * Should keep the search until the end even if it encounters
 * iwait_dist <= bfs_dist on the way because there might be
 * DEPT_IWAIT_UNKNOWN or something here and there which should be filled
 * with a valid one.
 *
 * XXX: The search by the branch can stop when it encounters
 * iwait_dist <= bfs_dist, if it's proved that there are no need to go
 * in the case.
 */
static enum bfs_ret cb_prop_iwait(struct dept_dep *d,
				  void *in, void **out)
{
	int irq = *(int *)in;
	struct dept_class *fc;
	struct dept_class *tc;
	struct dept_dep *new;

	if (DEPT_WARN_ON(!out))
		return DEPT_BFS_DONE;

	/*
	 * initial condition for this BFS search
	 */
	if (!d)
		return DEPT_BFS_CONTINUE;

	fc = dep_fc(d);
	tc = dep_tc(d);

	if (tc->iwait_dist[irq] <= tc->bfs_dist)
		return DEPT_BFS_CONTINUE;

	tc->iwait_dist[irq] = tc->bfs_dist;
	WRITE_ONCE(tc->iwait[irq], fc->iwait[irq]);

	if (!tc->iecxt[irq])
		return DEPT_BFS_CONTINUE;

	/*
	 * Add irq dependency to form a complete circle.
	 */
	new = __add_dep(tc->iecxt[irq], tc->iwait[irq]);
	if (!new)
		return DEPT_BFS_CONTINUE;

	*out = new;
	return DEPT_BFS_DONE;
}

/*
 * Should keep the search until the end even if it encounters another
 * iwait on the way because there might be the iwait here and there by
 * any chance which should be cleaned as well.
 *
 * XXX: The search by the branch can stop when it encounters another
 * iwait, if it's proved that there are no more the iwait in the case.
 */
static enum bfs_ret cb_clean_iwait(struct dept_dep *d, void *in, void **out)
{
	struct dept_class *c = (struct dept_class *)in;
	struct dept_class *tc;
	int i;

	/*
	 * initial condition for this BFS search
	 */
	if (!d)
		return DEPT_BFS_CONTINUE;

	tc = dep_tc(d);

	for (i = 0; i < DEPT_IRQS_NR; i++) {
		if (!tc->iwait[i] || tc->iwait[i] != c->iwait[i])
			continue;
		WRITE_ONCE(tc->iwait[i], DEPT_IWAIT_UNKNOWN);
		tc->iwait_dist[i] = INT_MAX;
	}

	return DEPT_BFS_CONTINUE;
}

/*
 * Should perform cleaning iwait even if c is not a root of the iwait
 * because c might be a bridge for the iwait to propagate.
 */
static void clean_iwait(struct dept_class *root, struct dept_class *c)
{
	bfs(root, cb_clean_iwait, (void *)c, NULL);
}

static LIST_HEAD(staleies);
static LIST_HEAD(staleiws);

static void stale_iecxt_iwait(struct dept_dep *d, int irq)
{
	struct dept_ecxt *ie = d->ecxt;
	struct dept_wait *iw = d->wait;
	struct dept_class *iwroot = iw->class;
	struct dept_staleie *sie;
	struct dept_staleiw *siw;
	struct dept_class onetime_c;
	int i;

	/*
	 * iwroot should be root of the iwait.
	 */
	DEPT_WARN_ON(iwroot->iwait_dist[irq] != 0);

	/*
	 * ie and iw should have one common irq flag at least.
	 */
	DEPT_WARN_ON(!(ie->enirqf & iw->irqf));

	for (i = 0; i < DEPT_IRQS_NR; i++)
		onetime_c.iwait[i] = (i == irq) ? iw : NULL;
	clean_iwait(iwroot, &onetime_c);

	sie = new_staleie();
	if (likely(sie)) {
		sie->ip = ie->enirq_ip[irq];
		sie->irq = irq;
		hash_add_staleie(sie);
		list_add(&sie->all_node, &staleies);
	}
	ie->enirqf &= ~(1UL << irq);
	put_ecxt(ie);
	WRITE_ONCE(dep_fc(d)->iecxt[irq], NULL);

	siw = new_staleiw();
	if (likely(siw)) {
		siw->ip = iw->irq_ip[irq];
		siw->irq = irq;
		hash_add_staleiw(siw);
		list_add(&siw->all_node, &staleiws);
	}
	iw->irqf &= ~(1UL << irq);
	put_wait(iw);
	WRITE_ONCE(dep_tc(d)->iwait[irq], NULL);
}

/*
 * Should start from a class that has 0 of iwait_dist.
 */
static void propagate_iwait(struct dept_class *c, int irq)
{
	struct dept_dep *new;
	do {
		new = NULL;
		bfs(c, cb_prop_iwait, (void *)&irq, (void **)&new);

		/*
		 * Deadlock detected. Let check_dl() report it.
		 */
		if (new) {
			check_dl(new);
			stale_iecxt_iwait(new, irq);
		}
	} while (new);
}

static enum bfs_ret cb_find_iwait(struct dept_dep *d, void *in, void **out)
{
	int irq = *(int *)in;
	struct dept_wait *w;

	if (DEPT_WARN_ON(!out))
		return DEPT_BFS_DONE;

	/*
	 * initial condition for this BFS search
	 */
	if (!d)
		return DEPT_BFS_CONTINUE_REV;

	w = dep_fc(d)->iwait[irq];
	if (!w || w == DEPT_IWAIT_UNKNOWN)
		return DEPT_BFS_CONTINUE_REV;

	*out = w;
	return DEPT_BFS_DONE;
}

static struct dept_wait *find_iwait(struct dept_class *c, int irq)
{
	struct dept_wait *w = NULL;
	bfs(c, cb_find_iwait, (void *)&irq, (void **)&w);
	return w;
}

static void add_iecxt(struct dept_ecxt *e, int irq, bool stack)
{
	/*
	 * This access is safe since we ensure e->class has set locally.
	 */
	struct dept_class *c = e->class;
	struct dept_task *dt = dept_task();

	if (READ_ONCE(c->iecxt[irq]))
		return;

	if (unlikely(staleie(dt->enirq_ip[irq], irq)))
		return;

	if (unlikely(!dept_lock()))
		return;

	if (c->iecxt[irq])
		goto unlock;

	if (unlikely(staleie(dt->enirq_ip[irq], irq)))
		goto unlock;

	e->enirqf |= (1UL << irq);

	WRITE_ONCE(c->iecxt[irq], get_ecxt(e));

	/*
	 * Should be NULL since it's the first time that these
	 * enirq_{ip,stack}[irq] have ever set.
	 */
	DEPT_WARN_ON(e->enirq_ip[irq]);
	DEPT_WARN_ON(e->enirq_stack[irq]);

	e->enirq_ip[irq] = dt->enirq_ip[irq];
	if (stack)
		e->enirq_stack[irq] = get_current_stack();

	if (c->iwait[irq] == DEPT_IWAIT_UNKNOWN) {
		struct dept_wait *w = find_iwait(c, irq);

		/*
		 * Deadlock detected. Let propagate_iwait() report it.
		 */
		if (w)
			propagate_iwait(w->class, irq);
	} else if (c->iwait[irq]) {
		/*
		 * Add irq dependency to form a complete circle.
		 */
		struct dept_dep *d = __add_dep(e, c->iwait[irq]);

		/*
		 * Deadlock detected. Let check_dl() report it.
		 */
		if (d) {
			check_dl(d);
			stale_iecxt_iwait(d, irq);
		}
	}
unlock:
	dept_unlock();
}

static void add_iwait(struct dept_wait *w, int irq)
{
	struct dept_class *c = w->class;
	struct dept_wait *iw;

	iw = READ_ONCE(c->iwait[irq]);
	if (iw && READ_ONCE(iw->class) == c)
		return;

	if (unlikely(staleiw(w->irq_ip[irq], irq)))
		return;

	if (unlikely(!dept_lock()))
		return;

	iw = c->iwait[irq];
	if (iw && iw->class == c)
		goto unlock;

	if (unlikely(staleiw(w->irq_ip[irq], irq)))
		goto unlock;

	w->irqf |= (1UL << irq);

	c->iwait_dist[irq] = 0;
	WRITE_ONCE(c->iwait[irq], get_wait(w));

	/*
	 * Should be NULL since it's the first time that these
	 * irq_{ip,stack}[irq] have ever set.
	 */
	DEPT_WARN_ON(w->irq_ip[irq]);
	DEPT_WARN_ON(w->irq_stack[irq]);

	w->irq_ip[irq] = w->wait_ip;
	w->irq_stack[irq] = get_current_stack();

	if (c->iecxt[irq]) {
		/*
		 * Add irq dependency to form a complete circle.
		 */
		struct dept_dep *d = __add_dep(c->iecxt[irq], w);

		/*
		 * Deadlock detected. Let check_dl() report it.
		 */
		if (d) {
			check_dl(d);
			stale_iecxt_iwait(d, irq);
		}
	}
	propagate_iwait(c, irq);
unlock:
	dept_unlock();
}

static inline struct dept_wait_hist *hist(int pos)
{
	struct dept_task *dt = dept_task();
	return dt->wait_hist + (pos % DEPT_MAX_WAIT_HIST);
}

static inline int hist_pos_next(void)
{
	struct dept_task *dt = dept_task();
	return dt->wait_hist_pos % DEPT_MAX_WAIT_HIST;
}

static inline void hist_advance(void)
{
	struct dept_task *dt = dept_task();
	dt->wait_hist_pos++;
	dt->wait_hist_pos %= DEPT_MAX_WAIT_HIST;
}

static inline struct dept_wait_hist *new_hist(void)
{
	struct dept_wait_hist *wh = hist(hist_pos_next());
	hist_advance();
	return wh;
}

static void add_hist(struct dept_wait *w, unsigned int wg, unsigned int ctxt_id)
{
	struct dept_wait_hist *wh = new_hist();

	if (likely(wh->wait))
		put_wait(wh->wait);

	wh->wait = get_wait(w);
	wh->wgen = wg;
	wh->ctxt_id = ctxt_id;
}

static void add_dep(struct dept_ecxt *e, struct dept_wait *w)
{
	struct dept_dep *d;
	int i;

	if (dep(e->class, w->class))
		return;

	if (unlikely(!dept_lock()))
		return;

	d = __add_dep(e, w);
	if (d) {
		check_dl(d);

		for (i = 0; i < DEPT_IRQS_NR; i++) {
			/*
			 * There's no iwait to propagate in the 'from'.
			 */
			if (!e->class->iwait[i])
				continue;
			/*
			 * This case has been already handled by add_iwait().
			 */
			if (w->class->iwait[i])
				continue;

			propagate_iwait(e->class->iwait[i]->class, i);
		}
	}
	dept_unlock();
}

static atomic_t wgen = ATOMIC_INIT(1);

static void add_wait(struct dept_class *c, unsigned long ip,
		     const char *w_fn, int ne)
{
	struct dept_task *dt = dept_task();
	struct dept_wait *w;
	unsigned int wg = 0U;
	int irq;
	int i;

	w = new_wait();
	if (unlikely(!w))
		return;

	WRITE_ONCE(w->class, get_class(c));
	w->wait_ip = ip;
	w->wait_fn = w_fn;
	w->wait_stack = get_current_stack();

	irq = cur_irq();
	if (irq < DEPT_IRQS_NR)
		add_iwait(w, irq);

	/*
	 * Avoid adding dependency between user aware nested ecxt and
	 * wait.
	 */
	for (i = dt->ecxt_held_pos - 1; i >= 0; i--) {
		struct dept_ecxt_held *eh;
		eh = dt->ecxt_held + i;
		if (eh->ecxt->class != c || eh->nest == ne)
			break;
	}

	for (; i >= 0; i--) {
		struct dept_ecxt_held *eh;
		eh = dt->ecxt_held + i;
		add_dep(eh->ecxt, w);
	}

	if (!wait_refered(w, 1) && !rich_stack) {
		if (w->wait_stack)
			put_stack(w->wait_stack);
		w->wait_stack = NULL;
	}

	/*
	 * Avoid zero wgen.
	 */
	wg = atomic_inc_return(&wgen) ?: atomic_inc_return(&wgen);
	add_hist(w, wg, cur_ctxt_id());

	del_wait(w);
}

static void add_ecxt(void *obj, struct dept_class *c, unsigned long ip,
		     const char *c_fn, const char *e_fn, int ne)
{
	struct dept_task *dt = dept_task();
	struct dept_ecxt_held *eh;
	struct dept_ecxt *e;
	unsigned long irqf;
	int irq;

	if (DEPT_WARN_ON(dt->ecxt_held_pos == DEPT_MAX_ECXT_HELD))
		return;

	e = new_ecxt();
	if (unlikely(!e))
		return;

	e->class = get_class(c);
	e->ecxt_ip = ip;
	e->ecxt_stack = ip && rich_stack ? get_current_stack() : NULL;
	e->event_fn = e_fn;
	e->ecxt_fn = c_fn;

	eh = dt->ecxt_held + (dt->ecxt_held_pos++);
	eh->ecxt = get_ecxt(e);
	eh->key = (unsigned long)obj;
	eh->wgen = atomic_read(&wgen);
	eh->nest = ne;

	irqf = cur_enirqf();
	for_each_set_bit(irq, &irqf, DEPT_IRQS_NR)
		add_iecxt(e, irq, false);

	del_ecxt(e);
}

static int find_ecxt_pos(unsigned long key, bool newfirst)
{
	struct dept_task *dt = dept_task();
	int i;

	if (newfirst) {
		for (i = dt->ecxt_held_pos - 1; i >= 0; i--)
			if (dt->ecxt_held[i].key == key)
				return i;
	} else {
		for (i = 0; i < dt->ecxt_held_pos; i++)
			if (dt->ecxt_held[i].key == key)
				return i;
	}
	return -1;
}

static void pop_ecxt(void *obj)
{
	struct dept_task *dt = dept_task();
	unsigned long key = (unsigned long)obj;
	int pos;
	int i;

	/*
	 * TODO: WARN on pos == -1.
	 */
	pos = find_ecxt_pos(key, true);
	if (pos == -1)
		return;

	put_ecxt(dt->ecxt_held[pos].ecxt);
	dt->ecxt_held_pos--;

	for (i = pos; i < dt->ecxt_held_pos; i++)
		dt->ecxt_held[i] = dt->ecxt_held[i + 1];
}

static inline bool good_hist(struct dept_wait_hist *wh, unsigned int wg)
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
		DEPT_WARN_ONCE("Need to expand the ring buffer.\n");
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

static void do_event(void *obj, struct dept_class *c, unsigned int wg,
		     unsigned long ip)
{
	struct dept_task *dt = dept_task();
	struct dept_wait_hist *wh;
	struct dept_ecxt_held *eh;
	unsigned long key = (unsigned long)obj;
	unsigned int ctxt_id;
	int end;
	int pos;
	int i;

	/*
	 * The event was triggered before wait.
	 */
	if (!wg)
		return;

	pos = find_ecxt_pos(key, false);
	if (pos == -1)
		return;

	eh = dt->ecxt_held + pos;
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
		if (wh->ctxt_id == ctxt_id)
			add_dep(eh->ecxt, wh->wait);
		if (!before(wh->wgen, eh->wgen))
			break;
	}

	for (i = 0; i < DEPT_IRQS_NR; i++) {
		if (before(dt->wgen_enirq[i], wg))
			continue;
		add_iecxt(eh->ecxt, i, false);
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

	clean_iwait(c, c);
	for (i = 0; i < DEPT_IRQS_NR; i++) {
		struct dept_ecxt *e = c->iecxt[i];
		struct dept_wait *w = c->iwait[i];
		if (e)
			put_ecxt(e);
		if (w && w->class == c)
			put_wait(w);
		WRITE_ONCE(c->iecxt[i], NULL);
		WRITE_ONCE(c->iwait[i], NULL);
	}

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
}

/*
 * IRQ context control
 * =====================================================================
 * Whether a wait is in {hard,soft}-IRQ context or whether
 * {hard,soft}-IRQ has been enabled on the way to an event is very
 * important to check dependency. All those things should be tracked.
 */

static inline unsigned long cur_enirqf(void)
{
	struct dept_task *dt = dept_task();
	int he = dt->hardirqs_enabled;
	int se = dt->softirqs_enabled;
	if (he)
		return DEPT_HIRQF | (se ? DEPT_SIRQF : 0UL);
	return 0UL;
}

static inline int cur_irq(void)
{
	if (lockdep_softirq_context(current))
		return DEPT_SIRQ;
	if (lockdep_hardirq_context())
		return DEPT_HIRQ;
	return DEPT_IRQS_NR;
}

static inline unsigned int cur_ctxt_id(void)
{
	struct dept_task *dt = dept_task();
	int irq = cur_irq();

	/*
	 * Normal process context
	 */
	if (irq == DEPT_IRQS_NR)
		return 0U;

	return dt->irq_id[irq] | (1UL << irq);
}

static void enirq_transition(int irq)
{
	struct dept_task *dt = dept_task();
	int i;

	/*
	 * READ wgen >= wgen of an event with IRQ enabled has been
	 * observed on the way to the event means, the IRQ can cut in
	 * within the ecxt. Used for cross-event detection.
	 *
	 *    wait context	event context(ecxt)
	 *    ------------	-------------------
	 *    wait event
	 *       WRITE wgen
	 *			observe IRQ enabled
	 *			   READ wgen
	 *			   keep the wgen locally
	 *
	 *			on the event
	 *			   check the local wgen
	 */
	dt->wgen_enirq[irq] = atomic_read(&wgen);

	for (i = dt->ecxt_held_pos - 1; i >= 0; i--) {
		struct dept_ecxt_held *eh;
		eh = dt->ecxt_held + i;
		add_iecxt(eh->ecxt, irq, true);
	}
}

static void enirq_update(unsigned long ip)
{
	struct dept_task *dt = dept_task();
	unsigned long irqf;
	unsigned long prev;
	int irq;

	prev = dt->eff_enirqf;
	irqf = cur_enirqf();
	dt->eff_enirqf = irqf;

	/*
	 * Do enirq_transition() only on an OFF -> ON transition.
	 */
	for_each_set_bit(irq, &irqf, DEPT_IRQS_NR) {
		unsigned long flags;

		if (prev & (1UL << irq))
			continue;

		flags = dept_enter();
		dt->enirq_ip[irq] = ip;
		enirq_transition(irq);
		dept_exit(flags);
	}
}

/*
 * Ensure it has been called on OFF -> ON transition.
 */
void dept_enable_softirq(unsigned long ip)
{
	struct dept_task *dt = dept_task();

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	if (DEPT_WARN_ON(early_boot_irqs_disabled))
		return;

	if (DEPT_WARN_ON(!irqs_disabled()))
		return;

	dt->softirqs_enabled = true;
	enirq_update(ip);
}

/*
 * Ensure it has been called on OFF -> ON transition.
 */
void dept_enable_hardirq(unsigned long ip)
{
	struct dept_task *dt = dept_task();

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	if (DEPT_WARN_ON(early_boot_irqs_disabled))
		return;

	if (DEPT_WARN_ON(!irqs_disabled()))
		return;

	dt->hardirqs_enabled = true;
	enirq_update(ip);
}

/*
 * Ensure it has been called on ON -> OFF transition.
 */
void dept_disable_softirq(unsigned long ip)
{
	struct dept_task *dt = dept_task();

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	if (DEPT_WARN_ON(!irqs_disabled()))
		return;

	dt->softirqs_enabled = false;
	enirq_update(ip);
}

/*
 * Ensure it has been called on ON -> OFF transition.
 */
void dept_disable_hardirq(unsigned long ip)
{
	struct dept_task *dt = dept_task();

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	if (DEPT_WARN_ON(!irqs_disabled()))
		return;

	dt->hardirqs_enabled = false;
	enirq_update(ip);
}

/*
 * Ensure it's the outmost softirq context.
 */
void dept_softirq_enter(void)
{
	struct dept_task *dt = dept_task();
	dt->irq_id[DEPT_SIRQ] += (1UL << DEPT_IRQS_NR);
}

/*
 * Ensure it's the outmost hardirq context.
 */
void dept_hardirq_enter(void)
{
	struct dept_task *dt = dept_task();
	dt->irq_id[DEPT_HIRQ] += (1UL << DEPT_IRQS_NR);
}

/*
 * Dept API
 * =====================================================================
 * Main Dept APIs.
 */

static inline void clean_classes_cache(struct dept_key *k)
{
	int i;

	for (i = 0; i < DEPT_MAX_SUBCLASSES_CACHE; i++)
		k->classes[i] = NULL;
}

void dept_map_init(struct dept_map *m, struct dept_key *k, int sub,
		   const char *n)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	flags = dept_enter();

	if(DEPT_WARN_ON(sub < 0 || sub >= DEPT_MAX_SUBCLASSES_USR)) {
		m->nocheck = true;
		goto exit;
	}

	if (m->keys != k)
		m->keys = k;
	clean_classes_cache(&m->keys_local);

	m->sub_usr = sub;
	m->name = n;
	m->wgen = 0U;
	m->nocheck = false;
exit:
	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_map_init);

void dept_map_reinit(struct dept_map *m)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	flags = dept_enter();

	clean_classes_cache(&m->keys_local);
	m->wgen = 0U;
	m->nocheck = false;

	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_map_reinit);

void dept_map_nocheck(struct dept_map *m)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	flags = dept_enter();

	m->nocheck = true;

	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_map_nocheck);

LIST_HEAD(dept_classes);

static inline bool within(const void *addr, void *start, unsigned long size)
{
	return addr >= start && addr < start + size;
}

static void free_staleie_staleiw_range(void *start, unsigned int sz)
{
	struct dept_staleie *sie, *sien;
	struct dept_staleiw *siw, *siwn;

	list_for_each_entry_safe(sie, sien, &staleies, all_node)
		if (within((void *)sie->ip, start, sz))
			hash_del_staleie(sie);

	list_for_each_entry_safe(siw, siwn, &staleiws, all_node)
		if (within((void *)siw->ip, start, sz))
			hash_del_staleiw(siw);
}

void dept_free_range(void *start, unsigned int sz)
{
	struct dept_task *dt = dept_task();
	struct dept_class *c, *n;
	unsigned long flags;

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	flags = dept_enter();

	/*
	 * dept_free_range() should not fail.
	 *
	 * FIXME: Should be fixed if dept_free_range() causes deadlock
	 * with dept_lock().
	 */
	while (unlikely(!dept_lock()));

	/*
	 * Free all staleies and staleiws within the range.
	 */
	free_staleie_staleiw_range(start, sz);

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

static inline int map_sub(struct dept_map *m, int e)
{
	return m->sub_usr + e * DEPT_MAX_SUBCLASSES_USR;
}

static struct dept_class *check_new_class(struct dept_key *local,
					  struct dept_key *k, int sub,
					  const char *n)
{
	struct dept_class *c = NULL;

	if (DEPT_WARN_ON(sub >= DEPT_MAX_SUBCLASSES))
		return NULL;

	if (DEPT_WARN_ON(!k))
		return NULL;

	if (sub < DEPT_MAX_SUBCLASSES_CACHE)
		c = READ_ONCE(local->classes[sub]);

	if (c)
		return c;

	c = class((unsigned long)k->subkeys + sub);
	if (c)
		goto caching;

	if (unlikely(!dept_lock()))
		return NULL;

	c = class((unsigned long)k->subkeys + sub);
	if (unlikely(c))
		goto unlock;

	c = new_class();
	if (unlikely(!c))
		goto unlock;

	c->name = n;
	c->sub = sub;
	c->key = (unsigned long)(k->subkeys + sub);
	hash_add_class(c);
	list_add(&c->all_node, &dept_classes);
unlock:
	dept_unlock();
caching:
	if (sub < DEPT_MAX_SUBCLASSES_CACHE && c)
		WRITE_ONCE(local->classes[sub], c);

	return c;
}

void dept_wait(struct dept_map *m, unsigned long w_f, unsigned long ip,
	       const char *w_fn, int ne)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;
	int e;

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	if (m->nocheck)
		return;

	flags = dept_enter();

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

		k = m->keys ?: &m->keys_local;
		c = check_new_class(&m->keys_local, k,
				    map_sub(m, e), m->name);
		if (!c)
			continue;

		add_wait(c, ip, w_fn, ne);
	}

	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_wait);

void dept_ecxt_enter(struct dept_map *m, unsigned long e_f, unsigned long ip,
		     const char *c_fn, const char *e_fn, int ne)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;
	int e;

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	if (m->nocheck)
		return;

	flags = dept_enter();

	for_each_set_bit(e, &e_f, DEPT_MAX_SUBCLASSES_EVT) {
		struct dept_class *c;
		struct dept_key *k;

		k = m->keys ?: &m->keys_local;
		c = check_new_class(&m->keys_local, k,
				    map_sub(m, e), m->name);
		if (!c)
			continue;

		add_ecxt((void *)m, c, ip, c_fn, e_fn, ne);
	}
	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_ecxt_enter);

void dept_asked_event(struct dept_map *m)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;
	unsigned int wg;

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	if (m->nocheck)
		return;

	flags = dept_enter();

	/*
	 * Avoid zero wgen.
	 */
	wg = atomic_inc_return(&wgen) ?: atomic_inc_return(&wgen);
	WRITE_ONCE(m->wgen, wg);

	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_asked_event);

void dept_event(struct dept_map *m, unsigned long e_f, unsigned long ip,
		const char *e_fn)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;
	int e;

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	if (m->nocheck)
		return;

	flags = dept_enter();

	for_each_set_bit(e, &e_f, DEPT_MAX_SUBCLASSES_EVT) {
		struct dept_class *c;
		struct dept_key *k;

		k = m->keys ?: &m->keys_local;
		c = check_new_class(&m->keys_local, k,
				    map_sub(m, e), m->name);
		if (!c)
			continue;

		add_ecxt((void *)m, c, 0UL, NULL, e_fn, 0);
		do_event((void *)m, c, READ_ONCE(m->wgen), ip);
		pop_ecxt((void *)m);
	}
	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_event);

void dept_ecxt_exit(struct dept_map *m, unsigned long ip)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	if (m->nocheck)
		return;

	flags = dept_enter();
	pop_ecxt((void *)m);
	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_ecxt_exit);

struct dept_map *dept_top_map(void)
{
	struct dept_task *dt = dept_task();
	struct dept_map *m;
	unsigned long flags;
	int pos;

	if (READ_ONCE(dept_stop) || dt->recursive)
		return NULL;

	flags = dept_enter();
	pos = dt->ecxt_held_pos;
	m = pos ? (struct dept_map *)dt->ecxt_held[pos - 1].key : NULL;
	dept_exit(flags);

	return m;
}
EXPORT_SYMBOL_GPL(dept_top_map);

void dept_warn_on(bool cond)
{
	struct dept_task *dt = dept_task();
	unsigned long flags;

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	flags = dept_enter();
	DEPT_WARN_ON(cond);
	dept_exit(flags);
}
EXPORT_SYMBOL_GPL(dept_warn_on);

void dept_task_exit(struct task_struct *t)
{
	struct dept_task *dt = &t->dept_task;
	int i;

	raw_local_irq_disable();

	if (dt->stack)
		put_stack(dt->stack);

	for (i = 0; i < dt->ecxt_held_pos; i++)
		put_ecxt(dt->ecxt_held[i].ecxt);

	for (i = 0; i < DEPT_MAX_WAIT_HIST; i++)
		if (dt->wait_hist[i].wait)
			put_wait(dt->wait_hist[i].wait);

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
	int sub;

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	flags = dept_enter();

	/*
	 * dept_key_init() should not fail.
	 *
	 * FIXME: Should be fixed if dept_key_init() causes deadlock
	 * with dept_lock().
	 */
	while (unlikely(!dept_lock()));

	for (sub = 0; sub < DEPT_MAX_SUBCLASSES; sub++) {
		struct dept_class *c;

		c = class((unsigned long)k->subkeys + sub);
		if (!c)
			continue;

		DEPT_STOP("The class(%s/%d) has not been removed.\n",
			  c->name, sub);
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
	int sub;

	if (READ_ONCE(dept_stop) || dt->recursive)
		return;

	flags = dept_enter();

	/*
	 * dept_key_destroy() should not fail.
	 *
	 * FIXME: Should be fixed if dept_key_destroy() causes deadlock
	 * with dept_lock().
	 */
	while (unlikely(!dept_lock()));

	for (sub = 0; sub < DEPT_MAX_SUBCLASSES; sub++) {
		struct dept_class *c;

		c = class((unsigned long)k->subkeys + sub);
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

#define OBJECT(id, nr) mem_total += sizeof(struct dept_##id) * nr;
	#include "dept_object.h"
#undef  OBJECT
#define HASH(id, bits) mem_total += sizeof(struct hlist_head) * (1UL << bits);
	#include "dept_hash.h"
#undef  HASH

	printk("DEPendency Tracker: Copyright (c) 2020 LG Electronics, Inc., Byungchul Park\n");
	printk("... DEPT_MAX_STACK_ENTRY: %d\n", DEPT_MAX_STACK_ENTRY);
	printk("... DEPT_MAX_WAIT_HIST  : %d\n", DEPT_MAX_WAIT_HIST  );
	printk("... DEPT_MAX_ECXT_HELD  : %d\n", DEPT_MAX_ECXT_HELD  );
	printk("... DEPT_MAX_SUBCLASSES : %d\n", DEPT_MAX_SUBCLASSES );
#define OBJECT(id, nr)							\
	printk("... memory used by %s: %zu KB\n",			\
	       #id, B2KB(sizeof(struct dept_##id) * nr));
	#include "dept_object.h"
#undef  OBJECT
#define HASH(id, bits)							\
	printk("... hash list head used by %s: %zu KB\n",		\
	       #id, B2KB(sizeof(struct hlist_head) * (1UL << bits)));
	#include "dept_hash.h"
#undef  HASH
	printk("... total memory used by objects and hashs: %zu KB\n", B2KB(mem_total));
	printk("... per task memory footprint: %zu bytes\n", sizeof(struct dept_task));
}

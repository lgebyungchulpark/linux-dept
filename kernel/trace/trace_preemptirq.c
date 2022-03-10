// SPDX-License-Identifier: GPL-2.0
/*
 * preemptoff and irqoff tracepoints
 *
 * Copyright (C) Joel Fernandes (Google) <joel@joelfernandes.org>
 */

#include <linux/kallsyms.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/ftrace.h>
#include <linux/kprobes.h>
#include "trace.h"

#define CREATE_TRACE_POINTS
#include <trace/events/preemptirq.h>

#ifdef CONFIG_TRACE_IRQFLAGS
/* Per-cpu variable to prevent redundant calls when IRQs already off */
static DEFINE_PER_CPU(int, tracing_irq_cpu);

void trace_softirqs_on(unsigned long ip)
{
	lockdep_softirqs_on(ip);
	dept_enable_softirq(ip);
}

void trace_softirqs_off(unsigned long ip)
{
	lockdep_softirqs_off(ip);
	dept_disable_softirq(ip);
}

void trace_hardirqs_on(void)
{
	if (this_cpu_read(tracing_irq_cpu)) {
		if (!in_nmi())
			trace_irq_enable_rcuidle(CALLER_ADDR0, CALLER_ADDR1);
		tracer_hardirqs_on(CALLER_ADDR0, CALLER_ADDR1);
		this_cpu_write(tracing_irq_cpu, 0);
	}
	dept_enable_hardirq(CALLER_ADDR0);

	lockdep_hardirqs_on(CALLER_ADDR0);
}
EXPORT_SYMBOL(trace_hardirqs_on);
NOKPROBE_SYMBOL(trace_hardirqs_on);

void trace_hardirqs_off(void)
{
	if (!this_cpu_read(tracing_irq_cpu)) {
		this_cpu_write(tracing_irq_cpu, 1);
		tracer_hardirqs_off(CALLER_ADDR0, CALLER_ADDR1);
		if (!in_nmi())
			trace_irq_disable_rcuidle(CALLER_ADDR0, CALLER_ADDR1);
	}
	dept_disable_hardirq(CALLER_ADDR0);

	lockdep_hardirqs_off(CALLER_ADDR0);
}
EXPORT_SYMBOL(trace_hardirqs_off);
NOKPROBE_SYMBOL(trace_hardirqs_off);

__visible void trace_hardirqs_on_caller(unsigned long caller_addr)
{
	if (this_cpu_read(tracing_irq_cpu)) {
		if (!in_nmi())
			trace_irq_enable_rcuidle(CALLER_ADDR0, caller_addr);
		tracer_hardirqs_on(CALLER_ADDR0, caller_addr);
		this_cpu_write(tracing_irq_cpu, 0);
	}
	dept_enable_hardirq(CALLER_ADDR0);

	lockdep_hardirqs_on(CALLER_ADDR0);
}
EXPORT_SYMBOL(trace_hardirqs_on_caller);
NOKPROBE_SYMBOL(trace_hardirqs_on_caller);

__visible void trace_hardirqs_off_caller(unsigned long caller_addr)
{
	if (!this_cpu_read(tracing_irq_cpu)) {
		this_cpu_write(tracing_irq_cpu, 1);
		tracer_hardirqs_off(CALLER_ADDR0, caller_addr);
		if (!in_nmi())
			trace_irq_disable_rcuidle(CALLER_ADDR0, caller_addr);
	}
	dept_disable_hardirq(CALLER_ADDR0);

	lockdep_hardirqs_off(CALLER_ADDR0);
}
EXPORT_SYMBOL(trace_hardirqs_off_caller);
NOKPROBE_SYMBOL(trace_hardirqs_off_caller);
#endif /* CONFIG_TRACE_IRQFLAGS */

#ifdef CONFIG_TRACE_PREEMPT_TOGGLE

void trace_preempt_on(unsigned long a0, unsigned long a1)
{
	if (!in_nmi())
		trace_preempt_enable_rcuidle(a0, a1);
	tracer_preempt_on(a0, a1);
}

void trace_preempt_off(unsigned long a0, unsigned long a1)
{
	if (!in_nmi())
		trace_preempt_disable_rcuidle(a0, a1);
	tracer_preempt_off(a0, a1);
}
#endif

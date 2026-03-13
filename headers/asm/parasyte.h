#ifndef __ASM_PARASYTE_H
#define __ASM_PARASYTE_H

#include <linux/cpumask.h>
#include <linux/wait.h>
#include <linux/of.h>
#include <uapi/asm/parasyte.h>

struct parasyte_msg_client;

void parasyte_handle_irq(void);
bool parasyte_ready(void);

#ifdef CONFIG_PARASYTE_HIVE
int parasyte_reserve_cma(void);
void parasyte_post_spore_shutdown(void);
#endif

#ifdef CONFIG_PARASYTE_SPORE
void parasyte_spore_set_cpu_mapping(unsigned int cpu, struct device_node *dn);
void parasyte_set_notify_hive(void (*notify_hive)(unsigned int irq));
void parasyte_notify_hive(void);
void parasyte_report_shutdown(void);
void parasyte_spore_shutdown(void);
void __parasyte_notify_hive(unsigned int);
#else
#define parasyte_set_notify_hive(notify_hive)	\
	do {					\
		(void)notify_hive;		\
		WARN_ON(1);			\
	} while (0)
#endif

#endif

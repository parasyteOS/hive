#define pr_fmt(fmt)	"parasyte/init: " fmt

#include <linux/module.h>
#include <asm/parasyte.h>

#include "dev.h"
#include "ring.h"
#include "spore.h"

static bool __parasyte_ready = false;

static int __init parasyte_init(void)
{
	int err = 0;
	__parasyte_ready = false;
#ifdef CONFIG_PARASYTE_HIVE
	err = parasyte_dev_init();
	if (err) return err;
#endif
#ifdef CONFIG_PARASYTE_SPORE
	err = parasyte_ring_init();
	if (err) return err;
	err = parasyte_spore_init();
	if (err) return err;
#endif
	__parasyte_ready = true;
	return err;
}
module_init(parasyte_init);

bool parasyte_ready(void)
{
	return __parasyte_ready;
}

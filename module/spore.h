#include <asm/parasyte.h>
#include <linux/interrupt.h>

int parasyte_spore_init(void);
void parasyte_spore_process_msg(struct parasyte_msg *msg);
int parasyte_spore_commit_io(struct parasyte_msg_client *client,
			     __u64 address, __u64 size, __u64 value_or_addr,
			     bool read, bool sync);
int parasyte_spore_request_softirq(__u64 nr, irq_handler_t handler, void *data);
int parasyte_spore_free_softirq(__u64 nr);
int parasyte_spore_synchronize_softirq(__u64 nr);

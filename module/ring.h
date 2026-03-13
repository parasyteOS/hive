#include <asm/parasyte.h>

#define MSG_PENDING(flags) (!!(flags & PARASYTE_MSG_FLAG_PENDING))
#define MSG_CLEAR_PENDING(flags) flags &= ~PARASYTE_MSG_FLAG_PENDING
#define MSG_COMPLETE(flags) (!!(flags & PARASYTE_MSG_FLAG_COMPLETE))
#define MSG_SET_COMPLETE(flags) flags |= PARASYTE_MSG_FLAG_COMPLETE
#define MSG_ERROR(flags) (!!(flags & PARASYTE_MSG_FLAG_ERROR))
#define MSG_SET_ERROR(flags) flags |= PARASYTE_MSG_FLAG_ERROR
#define MSG_SYNC(flags) (!!(flags & PARASYTE_MSG_FLAG_SYNC))
#define MSG_IO_USE_PTR(flags) (!!(flags & PARASYTE_MSG_FLAG_IO_USE_PTR))

int parasyte_ring_init(void);
int parasyte_ring_produce_msg(struct parasyte_msg_client *client, __u64 type, __u64 hive_cpu_idx,
			      bool sync, void *data,
			      int (*fill_msg)(__u64 *msg_flags,
					      union parasyte_msg_payload *msg_payload,
					      void *data),
			      int (*callback)(__u64 *msg_flags,
					      union parasyte_msg_payload *msg_payload,
					      void *data));
struct parasyte_msg_client *parasyte_ring_allocate_msg_client(void);
void parasyte_ring_free_msg_client(struct parasyte_msg_client *client);

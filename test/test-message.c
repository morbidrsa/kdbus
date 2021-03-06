#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <time.h>
#include <stdbool.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "kdbus-api.h"
#include "kdbus-util.h"
#include "kdbus-enum.h"
#include "kdbus-test.h"

/*
 * maximum number of queued messages wich will not be user accounted.
 * after this value is reached each user will have an individual limit.
 */
#define KDBUS_CONN_MAX_MSGS_UNACCOUNTED		16

/*
 * maximum number of queued messages from the same indvidual user after the
 * the un-accounted value has been hit
 */
#define KDBUS_CONN_MAX_MSGS_PER_USER		16

#define MAX_USER_TOTAL_MSGS  (KDBUS_CONN_MAX_MSGS_UNACCOUNTED + \
				KDBUS_CONN_MAX_MSGS_PER_USER)
/* maximum number of queued messages in a connection */
#define KDBUS_CONN_MAX_MSGS			256

/* maximum number of queued requests waiting for a reply */
#define KDBUS_CONN_MAX_REQUESTS_PENDING		128

/* maximum message payload size */
#define KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE		(2 * 1024UL * 1024UL)

int kdbus_test_message_basic(struct kdbus_test_env *env)
{
	struct kdbus_conn *conn;
	struct kdbus_conn *sender;
	struct kdbus_msg *msg;
	uint64_t cookie = 0x1234abcd5678eeff;
	uint64_t offset;
	int ret;

	sender = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(sender != NULL);

	/* create a 2nd connection */
	conn = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(conn != NULL);

	ret = kdbus_add_match_empty(conn);
	ASSERT_RETURN(ret == 0);

	ret = kdbus_add_match_empty(sender);
	ASSERT_RETURN(ret == 0);

	/* send over 1st connection */
	ret = kdbus_msg_send(sender, NULL, cookie, 0, 0, 0,
			     KDBUS_DST_ID_BROADCAST);
	ASSERT_RETURN(ret == 0);

	/* Make sure that we do not get our own broadcasts */
	ret = kdbus_msg_recv(sender, NULL, NULL);
	ASSERT_RETURN(ret == -EAGAIN);

	/* ... and receive on the 2nd */
	ret = kdbus_msg_recv_poll(conn, 100, &msg, &offset);
	ASSERT_RETURN(ret == 0);
	ASSERT_RETURN(msg->cookie == cookie);

	kdbus_msg_free(msg);

	/* Msgs that expect a reply must have timeout and cookie */
	ret = kdbus_msg_send(sender, NULL, 0, KDBUS_MSG_EXPECT_REPLY,
			     0, 0, conn->id);
	ASSERT_RETURN(ret == -EINVAL);

	/* Faked replies with a valid reply cookie are rejected */
	ret = kdbus_msg_send_reply(conn, time(NULL) ^ cookie, sender->id);
	ASSERT_RETURN(ret == -EPERM);

	ret = kdbus_free(conn, offset);
	ASSERT_RETURN(ret == 0);

	kdbus_conn_free(sender);
	kdbus_conn_free(conn);

	return TEST_OK;
}

static int msg_recv_prio(struct kdbus_conn *conn,
			 int64_t requested_prio,
			 int64_t expected_prio)
{
	struct kdbus_cmd_recv recv = {
		.size = sizeof(recv),
		.flags = KDBUS_RECV_USE_PRIORITY,
		.priority = requested_prio,
	};
	struct kdbus_msg *msg;
	int ret;

	ret = kdbus_cmd_recv(conn->fd, &recv);
	if (ret < 0) {
		kdbus_printf("error receiving message: %d (%m)\n", -errno);
		return ret;
	}

	msg = (struct kdbus_msg *)(conn->buf + recv.msg.offset);
	kdbus_msg_dump(conn, msg);

	if (msg->priority != expected_prio) {
		kdbus_printf("expected message prio %lld, got %lld\n",
			     (unsigned long long) expected_prio,
			     (unsigned long long) msg->priority);
		return -EINVAL;
	}

	kdbus_msg_free(msg);
	ret = kdbus_free(conn, recv.msg.offset);
	if (ret < 0)
		return ret;

	return 0;
}

int kdbus_test_message_prio(struct kdbus_test_env *env)
{
	struct kdbus_conn *a, *b;
	uint64_t cookie = 0;

	a = kdbus_hello(env->buspath, 0, NULL, 0);
	b = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(a && b);

	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0,   25, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0, -600, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0,   10, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0,  -35, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0, -100, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0,   20, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0,  -15, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0, -800, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0, -150, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0,   10, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0, -800, a->id) == 0);
	ASSERT_RETURN(kdbus_msg_send(b, NULL, ++cookie, 0, 0,  -10, a->id) == 0);

	ASSERT_RETURN(msg_recv_prio(a, -200, -800) == 0);
	ASSERT_RETURN(msg_recv_prio(a, -100, -800) == 0);
	ASSERT_RETURN(msg_recv_prio(a, -400, -600) == 0);
	ASSERT_RETURN(msg_recv_prio(a, -400, -600) == -EAGAIN);
	ASSERT_RETURN(msg_recv_prio(a, 10, -150) == 0);
	ASSERT_RETURN(msg_recv_prio(a, 10, -100) == 0);

	kdbus_printf("--- get priority (all)\n");
	ASSERT_RETURN(kdbus_msg_recv(a, NULL, NULL) == 0);

	kdbus_conn_free(a);
	kdbus_conn_free(b);

	return TEST_OK;
}

static int kdbus_test_notify_kernel_quota(struct kdbus_test_env *env)
{
	int ret;
	unsigned int i;
	struct kdbus_conn *conn;
	struct kdbus_conn *reader;
	struct kdbus_msg *msg = NULL;

	reader = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(reader);

	conn = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(conn);

	/* Register for ID signals */
	ret = kdbus_add_match_id(reader, 0x1, KDBUS_ITEM_ID_ADD,
				 KDBUS_MATCH_ID_ANY);
	ASSERT_RETURN(ret == 0);

	ret = kdbus_add_match_id(reader, 0x2, KDBUS_ITEM_ID_REMOVE,
				 KDBUS_MATCH_ID_ANY);
	ASSERT_RETURN(ret == 0);

	/* Each iteration two notifications: add and remove ID */
	for (i = 0; i < KDBUS_CONN_MAX_MSGS / 2; i++) {
		struct kdbus_conn *notifier;

		notifier = kdbus_hello(env->buspath, 0, NULL, 0);
		ASSERT_RETURN(notifier);

		kdbus_conn_free(notifier);

	}

	/*
	 * Now the reader queue is full, message will be lost
	 * but it will not be accounted in dropped msgs
	 */
	ret = kdbus_msg_send(conn, NULL, 0xdeadbeef, 0, 0, 0, reader->id);
	ASSERT_RETURN(ret == -ENOBUFS);

	/* More ID kernel notifications that will be lost */
	kdbus_conn_free(conn);

	conn = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(conn);

	kdbus_conn_free(conn);

	kdbus_msg_free(msg);

	/* Read our queue */
	for (i = 0; i < KDBUS_CONN_MAX_MSGS; i++) {
		ret = kdbus_msg_recv_poll(reader, 100, &msg, NULL);
		ASSERT_RETURN(ret == 0);

		kdbus_msg_free(msg);
	}

	ret = kdbus_msg_recv(reader, NULL, NULL);
	ASSERT_RETURN(ret == -EAGAIN);

	kdbus_conn_free(reader);

	return 0;
}

/* Return the number of message successfully sent */
static int kdbus_fill_conn_queue(struct kdbus_conn *conn_src,
				 uint64_t dst_id,
				 unsigned int max_msgs)
{
	unsigned int i;
	uint64_t cookie = 0;
	int ret;

	for (i = 0; i < max_msgs; i++) {
		ret = kdbus_msg_send(conn_src, NULL, ++cookie,
				     0, 0, 0, dst_id);
		if (ret < 0)
			break;
	}

	return i;
}

static int kdbus_test_broadcast_quota(struct kdbus_test_env *env)
{
	int ret;
	unsigned int i;
	struct kdbus_msg *msg;
	struct kdbus_conn *privileged_a;
	struct kdbus_conn *privileged_b;
	struct kdbus_conn *holder;
	struct kdbus_policy_access access = {
		.type = KDBUS_POLICY_ACCESS_WORLD,
		.id = getuid(),
		.access = KDBUS_POLICY_TALK,
	};
	uint64_t expected_cookie = time(NULL) ^ 0xdeadbeef;

	holder = kdbus_hello_registrar(env->buspath, "com.example.a",
				       &access, 1,
				       KDBUS_HELLO_POLICY_HOLDER);
	ASSERT_RETURN(holder);

	privileged_a = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(privileged_a);

	privileged_b = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(privileged_b);

	/* Acquire name with access world so they can talk to us */
	ret = kdbus_name_acquire(privileged_a, "com.example.a", NULL);
	ASSERT_RETURN(ret >= 0);

	/* Broadcast matches for privileged connections */
	ret = kdbus_add_match_empty(privileged_a);
	ASSERT_RETURN(ret == 0);

	ret = kdbus_add_match_empty(privileged_b);
	ASSERT_RETURN(ret == 0);

	/*
	 * We start accouting after KDBUS_CONN_MAX_MSGS_UNACCOUNTED
	 * so the first sender will at least send
	 * KDBUS_CONN_MAX_MSGS_UNACCOUNTED + KDBUS_CONN_MAX_MSGS_PER_USER
	 */
	ret = RUN_UNPRIVILEGED_CONN(unpriv, env->buspath, ({
		unsigned int cnt;

		cnt = kdbus_fill_conn_queue(unpriv, KDBUS_DST_ID_BROADCAST,
					    MAX_USER_TOTAL_MSGS);
		ASSERT_EXIT(cnt == MAX_USER_TOTAL_MSGS);

		/*
		 * Another message that will trigger the lost count
		 *
		 * Broadcasts always succeed
		 */
		ret = kdbus_msg_send(unpriv, NULL, 0xdeadbeef, 0, 0,
				     0, KDBUS_DST_ID_BROADCAST);
		ASSERT_EXIT(ret == 0);
	}));
	ASSERT_RETURN(ret == 0);

	expected_cookie++;
	/* Now try to send a legitimate message from B to A */
	ret = kdbus_msg_send(privileged_b, NULL, expected_cookie, 0,
			     0, 0, privileged_a->id);
	ASSERT_RETURN(ret == 0);

	expected_cookie++;
	ret = kdbus_msg_send(privileged_b, NULL, expected_cookie, 0,
			     0, 0, KDBUS_DST_ID_BROADCAST);
	ASSERT_RETURN(ret == 0);

	/* Read our queue */
	for (i = 0; i < MAX_USER_TOTAL_MSGS; i++) {
		ret = kdbus_msg_recv_poll(privileged_a, 100, &msg, NULL);
		ASSERT_RETURN(ret == 0);

		ASSERT_RETURN(msg->dst_id == KDBUS_DST_ID_BROADCAST);

		kdbus_msg_free(msg);
	}

	ret = kdbus_msg_recv_poll(privileged_a, 100, &msg, NULL);
	ASSERT_RETURN(ret == 0);

	/* Unicast message */
	ASSERT_RETURN(msg->cookie == expected_cookie - 1);
	ASSERT_RETURN(msg->src_id == privileged_b->id &&
		      msg->dst_id == privileged_a->id);

	kdbus_msg_free(msg);

	ret = kdbus_msg_recv_poll(privileged_a, 100, &msg, NULL);
	ASSERT_RETURN(ret == 0);

	/* Broadcast message */
	ASSERT_RETURN(msg->cookie == expected_cookie);
	ASSERT_RETURN(msg->src_id == privileged_b->id &&
		      msg->dst_id == KDBUS_DST_ID_BROADCAST);

	kdbus_msg_free(msg);

	/* Queue empty */
	ret = kdbus_msg_recv(privileged_a, NULL, NULL);
	ASSERT_RETURN(ret == -EAGAIN);

	kdbus_conn_free(holder);
	kdbus_conn_free(privileged_a);
	kdbus_conn_free(privileged_b);

	return 0;
}

static int kdbus_test_expected_reply_quota(struct kdbus_test_env *env)
{
	int ret;
	unsigned int i, n;
	unsigned int count;
	uint64_t cookie = 0x1234abcd5678eeff;
	struct kdbus_conn *conn;
	struct kdbus_conn *connections[9];

	conn = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(conn);

	for (i = 0; i < 9; i++) {
		connections[i] = kdbus_hello(env->buspath, 0, NULL, 0);
		ASSERT_RETURN(connections[i]);
	}

	count = 0;
	/* Send 16 messages to 8 different connections */
	for (i = 0; i < 8; i++) {
		for (n = 0; n < KDBUS_CONN_MAX_MSGS_PER_USER; n++, count++) {
			ret = kdbus_msg_send(conn, NULL, cookie++,
					     KDBUS_MSG_EXPECT_REPLY,
					     100000000ULL, 0,
					     connections[i]->id);
			ASSERT_RETURN(ret == 0);
		}
	}

	ASSERT_RETURN(count == KDBUS_CONN_MAX_REQUESTS_PENDING);

	/*
	 * Now try to send a message to the last connection,
	 * if we have reached KDBUS_CONN_MAX_REQUESTS_PENDING
	 * no further requests are allowed
	 */
	ret = kdbus_msg_send(conn, NULL, cookie++, KDBUS_MSG_EXPECT_REPLY,
			     1000000000ULL, 0, connections[i]->id);
	ASSERT_RETURN(ret == -EMLINK);

	for (i = 0; i < 9; i++)
		kdbus_conn_free(connections[i]);

	kdbus_conn_free(conn);

	return 0;
}

static int kdbus_test_multi_users_quota(struct kdbus_test_env *env)
{
	int ret, efd1, efd2;
	unsigned int cnt, recved_count;
	struct kdbus_conn *conn;
	struct kdbus_conn *privileged;
	struct kdbus_conn *holder;
	eventfd_t child1_count = 0, child2_count = 0;
	struct kdbus_policy_access access = {
		.type = KDBUS_POLICY_ACCESS_WORLD,
		.id = geteuid(),
		.access = KDBUS_POLICY_TALK,
	};

	holder = kdbus_hello_registrar(env->buspath, "com.example.a",
				       &access, 1,
				       KDBUS_HELLO_POLICY_HOLDER);
	ASSERT_RETURN(holder);

	privileged = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(privileged);

	conn = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(conn);

	/* Acquire name with access world so they can talk to us */
	ret = kdbus_name_acquire(conn, "com.example.a", NULL);
	ASSERT_EXIT(ret >= 0);

	/* Use this to tell parent how many messages have bee sent */
	efd1 = eventfd(0, EFD_CLOEXEC);
	ASSERT_RETURN_VAL(efd1 >= 0, efd1);

	efd2 = eventfd(0, EFD_CLOEXEC);
	ASSERT_RETURN_VAL(efd2 >= 0, efd2);

	/*
	 * Queue multiple messages as different users at the
	 * same time.
	 *
	 * When the receiver queue count is below
	 * KDBUS_CONN_MAX_MSGS_UNACCOUNTED messages are not accounted.
	 *
	 * So we start two threads running under different uid, they
	 * race and each one will try to send:
	 * (KDBUS_CONN_MAX_MSGS_UNACCOUNTED + KDBUS_CONN_MAX_MSGS_PER_USER) + 1
	 * msg
	 *
	 * Both threads will return how many message was successfull
	 * queued, later we compute and try to validate the user quota
	 * checks.
	 */
	ret = RUN_UNPRIVILEGED(UNPRIV_UID, UNPRIV_GID, ({
		struct kdbus_conn *unpriv;

		unpriv = kdbus_hello(env->buspath, 0, NULL, 0);
		ASSERT_EXIT(unpriv);

		cnt = kdbus_fill_conn_queue(unpriv, conn->id,
					    MAX_USER_TOTAL_MSGS + 1);
		/* Explicitly check for 0 we can't send it to eventfd */
		ASSERT_EXIT(cnt > 0);

		ret = eventfd_write(efd1, cnt);
		ASSERT_EXIT(ret == 0);
	}),
	({;
		/* Queue other messages as a different user */
		ret = RUN_UNPRIVILEGED(UNPRIV_UID - 1, UNPRIV_GID - 1, ({
			struct kdbus_conn *unpriv;

			unpriv = kdbus_hello(env->buspath, 0, NULL, 0);
			ASSERT_EXIT(unpriv);

			cnt = kdbus_fill_conn_queue(unpriv, conn->id,
						    MAX_USER_TOTAL_MSGS + 1);
			/* Explicitly check for 0 */
			ASSERT_EXIT(cnt > 0);

			ret = eventfd_write(efd2, cnt);
			ASSERT_EXIT(ret == 0);
		}),
		({ 0; }));
		ASSERT_RETURN(ret == 0);

	}));
	ASSERT_RETURN(ret == 0);

	/* Delay reading, so if children die we are not blocked */
	ret = eventfd_read(efd1, &child1_count);
	ASSERT_RETURN(ret >= 0);

	ret = eventfd_read(efd2, &child2_count);
	ASSERT_RETURN(ret >= 0);

	recved_count = child1_count + child2_count;

	/* Validate how many messages have been sent */
	ASSERT_RETURN(recved_count > 0);

	/*
	 * We start accounting after KDBUS_CONN_MAX_MSGS_UNACCOUNTED so now we
	 * have a KDBUS_CONN_MAX_MSGS_UNACCOUNTED not accounted, and given we
	 * have at least sent (KDBUS_CONN_MAX_MSGS_UNACCOUNTED +
	 * KDBUS_CONN_MAX_MSGS_PER_USER) + 1 for the two threads: recved_count
	 * for both treads will for sure exceed that value.
	 *
	 * 1) Both thread1 msgs + threads2 msgs exceed
	 *    KDBUS_CONN_MAX_MSGS_UNACCOUNTED. Accounting is started.
	 * 2) Now both of them will be able to send only his quota
	 *    which is KDBUS_CONN_MAX_MSGS_PER_USER
	 *    (previous sent messages of 1) were not accounted)
	 */
	ASSERT_RETURN(recved_count > MAX_USER_TOTAL_MSGS + 1)

	/*
	 * A process should never receive more than
	 * (KDBUS_CONN_MAX_MSGS_UNACCOUNTED + KDBUS_CONN_MAX_MSGS_PER_USER) + 1)
	 */
	ASSERT_RETURN(child1_count < MAX_USER_TOTAL_MSGS + 1)

	/*
	 * Now both no accounted messages should give us
	 * KDBUS_CONN_MAX_MSGS_UNACCOUNTED when the accounting
	 * started.
	 *
	 * child1 non accounted + child2 non accounted =
	 * KDBUS_CONN_MAX_MSGS_UNACCOUNTED
	 */
	ASSERT_RETURN(KDBUS_CONN_MAX_MSGS_UNACCOUNTED ==
		((child1_count - KDBUS_CONN_MAX_MSGS_PER_USER) +
		 ((recved_count - child1_count) -
		  KDBUS_CONN_MAX_MSGS_PER_USER)));

	/*
	 * A process should never receive more than
	 * (KDBUS_CONN_MAX_MSGS_UNACCOUNTED + KDBUS_CONN_MAX_MSGS_PER_USER) + 1)
	 */
	ASSERT_RETURN(child2_count < MAX_USER_TOTAL_MSGS + 1)

	/*
	 * Now both no accounted messages should give us
	 * KDBUS_CONN_MAX_MSGS_UNACCOUNTED when the accounting
	 * started.
	 *
	 * child1 non accounted + child2 non accounted =
	 * KDBUS_CONN_MAX_MSGS_UNACCOUNTED
	 */
	ASSERT_RETURN(KDBUS_CONN_MAX_MSGS_UNACCOUNTED ==
		((child2_count - KDBUS_CONN_MAX_MSGS_PER_USER) +
		 ((recved_count - child2_count) -
		  KDBUS_CONN_MAX_MSGS_PER_USER)));

	/* Try to queue up more, but we fail no space in the pool */
	cnt = kdbus_fill_conn_queue(privileged, conn->id, KDBUS_CONN_MAX_MSGS);
	ASSERT_RETURN(cnt > 0 && cnt < KDBUS_CONN_MAX_MSGS);

	ret = kdbus_msg_send(privileged, NULL, 0xdeadbeef, 0, 0,
			     0, conn->id);
	ASSERT_RETURN(ret == -ENOBUFS);

	close(efd1);
	close(efd2);

	kdbus_conn_free(privileged);
	kdbus_conn_free(holder);
	kdbus_conn_free(conn);

	return 0;
}

int kdbus_test_pool_quota(struct kdbus_test_env *env)
{
	struct kdbus_conn *a, *b, *c;
	struct kdbus_cmd_send cmd = {};
	struct kdbus_item *item;
	struct kdbus_msg *recv_msg;
	struct kdbus_msg *msg;
	uint64_t cookie = time(NULL);
	uint64_t size;
	unsigned int i;
	char *payload;
	int ret;

	/* just a guard */
	if (POOL_SIZE <= KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE ||
	    POOL_SIZE % KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE != 0)
		return 0;

	payload = calloc(KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE, sizeof(char));
	ASSERT_RETURN_VAL(payload, -ENOMEM);

	a = kdbus_hello(env->buspath, 0, NULL, 0);
	b = kdbus_hello(env->buspath, 0, NULL, 0);
	c = kdbus_hello(env->buspath, 0, NULL, 0);
	ASSERT_RETURN(a && b && c);

	size = sizeof(struct kdbus_msg);
	size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_vec));

	msg = malloc(size);
	ASSERT_RETURN_VAL(msg, -ENOMEM);

	memset(msg, 0, size);
	msg->size = size;
	msg->src_id = a->id;
	msg->dst_id = c->id;
	msg->payload_type = KDBUS_PAYLOAD_DBUS;

	item = msg->items;
	item->type = KDBUS_ITEM_PAYLOAD_VEC;
	item->size = KDBUS_ITEM_HEADER_SIZE + sizeof(struct kdbus_vec);
	item->vec.address = (uintptr_t)payload;
	item->vec.size = KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE;
	item = KDBUS_ITEM_NEXT(item);

	cmd.size = sizeof(cmd);
	cmd.msg_address = (uintptr_t)msg;

	for (i = 0; i < (POOL_SIZE/(size + KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE) - 1); i++) {
		msg->cookie = cookie++;

		ret = kdbus_cmd_send(a->fd, &cmd);
		ASSERT_RETURN_VAL(ret == 0, ret);
	}

	msg->cookie = cookie++;
	ret = kdbus_cmd_send(a->fd, &cmd);
	ASSERT_RETURN(ret == -EXFULL);

	ret = kdbus_msg_send(b, NULL, cookie++, 0, 0, 0, c->id);
	ASSERT_RETURN(ret == 0);

	for (i = 0; i < (POOL_SIZE/(size + KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE) - 1); i++) {
		ret = kdbus_msg_recv(c, &recv_msg, NULL);
		ASSERT_RETURN(ret == 0);
		ASSERT_RETURN(recv_msg->src_id == a->id);

		kdbus_msg_free(recv_msg);
	}

	ret = kdbus_msg_recv(c, &recv_msg, NULL);
	ASSERT_RETURN(ret == 0);
	ASSERT_RETURN(recv_msg->src_id == b->id);

	kdbus_msg_free(recv_msg);

	ret = kdbus_msg_recv(c, NULL, NULL);
	ASSERT_RETURN(ret == -EAGAIN);

	free(msg);
	free(payload);

	kdbus_conn_free(c);
	kdbus_conn_free(b);
	kdbus_conn_free(a);

	return 0;
}

int kdbus_test_message_quota(struct kdbus_test_env *env)
{
	struct kdbus_conn *a, *b;
	uint64_t cookie = 0;
	int ret;
	int i;

	ret = kdbus_test_notify_kernel_quota(env);
	ASSERT_RETURN(ret == 0);

	ret = kdbus_test_expected_reply_quota(env);
	ASSERT_RETURN(ret == 0);

	ret = kdbus_test_pool_quota(env);
	ASSERT_RETURN(ret == 0);

	if (geteuid() == 0 && all_uids_gids_are_mapped()) {
		ret = kdbus_test_multi_users_quota(env);
		ASSERT_RETURN(ret == 0);

		ret = kdbus_test_broadcast_quota(env);
		ASSERT_RETURN(ret == 0);

		/* Drop to 'nobody' and continue test */
		ret = setresuid(UNPRIV_UID, UNPRIV_UID, UNPRIV_UID);
		ASSERT_RETURN(ret == 0);
	}

	a = kdbus_hello(env->buspath, 0, NULL, 0);
	b = kdbus_hello(env->buspath, 0, NULL, 0);

	ret = kdbus_fill_conn_queue(b, a->id, MAX_USER_TOTAL_MSGS);
	ASSERT_RETURN(ret == MAX_USER_TOTAL_MSGS);

	ret = kdbus_msg_send(b, NULL, ++cookie, 0, 0, 0, a->id);
	ASSERT_RETURN(ret == -ENOBUFS);

	for (i = 0; i < MAX_USER_TOTAL_MSGS; ++i) {
		ret = kdbus_msg_recv(a, NULL, NULL);
		ASSERT_RETURN(ret == 0);
	}

	ret = kdbus_fill_conn_queue(b, a->id, MAX_USER_TOTAL_MSGS);
	ASSERT_RETURN(ret == MAX_USER_TOTAL_MSGS);

	ret = kdbus_msg_send(b, NULL, ++cookie, 0, 0, 0, a->id);
	ASSERT_RETURN(ret == -ENOBUFS);

	kdbus_conn_free(a);
	kdbus_conn_free(b);

	return TEST_OK;
}

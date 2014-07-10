/* Copyright (c) 2007-2008 Timo Sirainen */

#include "lib.h"
#include "base64.h"
#include "str.h"
#include "strescape.h"
#include "time-util.h"
#include "istream.h"
#include "istream-crlf.h"
#include "ostream.h"
#include "imap-date.h"
#include "imap-util.h"
#include "imap-arg.h"

#include "settings.h"
#include "mailbox.h"
#include "mailbox-state.h"
#include "mailbox-source.h"
#include "checkpoint.h"
#include "commands.h"
#include "search.h"
#include "client.h"
#include "client-state.h"

#include <stdlib.h>

struct state states[STATE_COUNT] = {
	{ "BANNER",	  "Bann", LSTATE_NONAUTH,  0,   0,  0 },
	{ "AUTHENTICATE", "Auth", LSTATE_NONAUTH,  0,   0,  FLAG_STATECHANGE | FLAG_STATECHANGE_AUTH },
	{ "LOGIN",	  "Logi", LSTATE_NONAUTH,  100, 0,  FLAG_STATECHANGE | FLAG_STATECHANGE_AUTH },
	{ "LIST",	  "List", LSTATE_AUTH,     50,  0,  FLAG_EXPUNGES },
	{ "MCREATE",	  "MCre", LSTATE_AUTH,     0,   0,  FLAG_EXPUNGES },
	{ "MDELETE",	  "MDel", LSTATE_AUTH,     0,   0,  FLAG_EXPUNGES },
	{ "STATUS",	  "Stat", LSTATE_AUTH,     50,  0,  FLAG_EXPUNGES },
	{ "SELECT",	  "Sele", LSTATE_AUTH,     100, 0,  FLAG_STATECHANGE | FLAG_STATECHANGE_SELECTED },
	{ "FETCH",	  "Fetc", LSTATE_SELECTED, 100, 0,  FLAG_MSGSET },
	{ "FETCH2",	  "Fet2", LSTATE_SELECTED, 100, 30, FLAG_MSGSET },
	{ "SEARCH",	  "Sear", LSTATE_SELECTED, 0,   0,  FLAG_MSGSET },
	{ "SORT",	  "Sort", LSTATE_SELECTED, 0,   0,  FLAG_MSGSET },
	{ "THREAD",	  "Thre", LSTATE_SELECTED, 0,   0,  FLAG_MSGSET },
	{ "COPY",	  "Copy", LSTATE_SELECTED, 33,  5,  FLAG_MSGSET | FLAG_EXPUNGES },
	{ "STORE",	  "Stor", LSTATE_SELECTED, 50,  0,  FLAG_MSGSET },
	{ "DELETE",	  "Dele", LSTATE_SELECTED, 100, 0,  FLAG_MSGSET },
	{ "EXPUNGE",	  "Expu", LSTATE_SELECTED, 100, 0,  FLAG_EXPUNGES },
	{ "APPEND",	  "Appe", LSTATE_AUTH,     100, 5,  FLAG_EXPUNGES },
	{ "NOOP",	  "Noop", LSTATE_AUTH,     0,   0,  FLAG_EXPUNGES },
	{ "IDLE",	  "Noop", LSTATE_AUTH,     0,   0,  FLAG_EXPUNGES | FLAG_STATECHANGE },
	{ "CHECK",	  "Chec", LSTATE_AUTH,     0,   0,  FLAG_EXPUNGES },
	{ "LOGOUT",	  "Logo", LSTATE_NONAUTH,  100, 0,  FLAG_STATECHANGE | FLAG_STATECHANGE_NONAUTH },
	{ "DISCONNECT",	  "Disc", LSTATE_NONAUTH,  0,   0,  0 },
	{ "DELAY",	  "Dela", LSTATE_NONAUTH,  0,   0,  0 },
	{ "CHECKPOINT!",  "ChkP", LSTATE_NONAUTH,  0,   0,  0 },
	{ "LMTP",         "LMTP", LSTATE_NONAUTH,  0,   0,  0 }
};

unsigned int counters[STATE_COUNT], total_counters[STATE_COUNT];
unsigned int timers[STATE_COUNT], timer_counts[STATE_COUNT];

bool do_rand(enum client_state state)
{
	return (rand() % 100) < states[state].probability;
}

bool do_rand_again(enum client_state state)
{
	return (rand() % 100) < states[state].probability_again;
}

void client_state_add_to_timer(enum client_state state,
			       const struct timeval *tv_start)
{
	struct timeval tv_end;
	int diff;

	gettimeofday(&tv_end, NULL);
	diff = timeval_diff_msecs(&tv_end, tv_start);
	if (diff < 0)
		diff = 0;
	i_assert((unsigned int)diff < UINT_MAX - timers[state]);
	timers[state] += diff;
	timer_counts[state]++;
}

static void auth_plain_callback(struct client *client, struct command *cmd,
				const struct imap_arg *args,
				enum command_reply reply)
{
	buffer_t *str, *buf;

	if (reply == REPLY_OK) {
		state_callback(client, cmd, args, reply);
		return;
	}
	if (reply != REPLY_CONT) {
		client_state_error(client, "AUTHENTICATE failed");
		client_disconnect(client);
		return;
	}

	counters[cmd->state]++;

	buf = t_str_new(512);
	if (conf.master_user != NULL) {
		str_append(buf, client->user->username);
		str_append_c(buf, '\0');
		str_append(buf, conf.master_user);
	} else {
		str_append_c(buf, '\0');
		str_append(buf, client->user->username);
	}
	str_append_c(buf, '\0');
	str_append(buf, client->user->password);

	str = t_str_new(512);
	base64_encode(buf->data, buf->used, str);
	str_append(str, "\r\n");

	o_stream_send_str(client->output, str_c(str));
}

static enum client_state client_eat_first_plan(struct client *client)
{
	enum client_state state;

	if (disconnect_clients)
		return STATE_LOGOUT;

	i_assert(client->plan_size > 0);
	state = client->plan[0];

	client->plan_size--;
	memmove(client->plan, client->plan + 1,
		sizeof(client->plan[0]) * client->plan_size);
	return state;
}

static enum client_state client_get_next_state(enum client_state state)
{
	i_assert(state < STATE_LOGOUT);

	for (;;) {
		if (!conf.random_states)
			state++;
		else {
			/* if we're not in selected state, we'll randomly do
			   LIST, SELECT, APPEND or LOGOUT */
			state = STATE_LIST +
				(rand() % (STATE_LOGOUT - STATE_LIST + 1));
		}

		if (do_rand(state))
			break;

		if (state == STATE_LOGOUT) {
			/* logout skipped, wrap */
			state = STATE_AUTHENTICATE + 1;
		}
	}
	return state;
}

static enum login_state flags2login_state(enum state_flags flags)
{
	if ((flags & FLAG_STATECHANGE_NONAUTH) != 0)
		return LSTATE_NONAUTH;
	else if ((flags & FLAG_STATECHANGE_AUTH) != 0)
		return LSTATE_AUTH;
	else if ((flags & FLAG_STATECHANGE_SELECTED) != 0)
		return LSTATE_SELECTED;

	i_unreached();
}

static enum state_flags
client_get_pending_cmd_flags(struct client *client,
			     enum login_state *new_lstate_r)
{
	enum state_flags state_flags = 0;
	struct command *const *cmds;
	unsigned int i, count;

	*new_lstate_r = client->login_state;
	cmds = array_get(&client->commands, &count);
	for (i = 0; i < count; i++) {
		enum state_flags flags = states[cmds[i]->state].flags;

		if ((flags & FLAG_STATECHANGE) != 0)
			*new_lstate_r = flags2login_state(flags);
		state_flags |= flags;
	}
	return state_flags;
}

static enum client_state client_update_plan(struct client *client)
{
	enum client_state state;
	enum login_state lstate;

	state = client->plan_size > 0 ?
		client->plan[client->plan_size - 1] : client->plan[0];
	if (client->plan_size > 0 &&
	    (states[state].flags & FLAG_STATECHANGE) != 0) {
		/* wait until the state change is done before making new
		   commands. */
		return client->plan[0];
	}
	if ((client_get_pending_cmd_flags(client, &lstate) &
	     FLAG_STATECHANGE) != 0)
		return state;

	if (state == STATE_LOGOUT)
		return state;

	while (client->plan_size <
	       sizeof(client->plan)/sizeof(client->plan[0])) {
		switch (client->login_state) {
		case LSTATE_NONAUTH:
			/* we begin with LOGIN/AUTHENTICATE commands */
			i_assert(client->plan_size == 0);
			state = do_rand(STATE_AUTHENTICATE) ?
				STATE_AUTHENTICATE : STATE_LOGIN;
			break;
		case LSTATE_AUTH:
		case LSTATE_SELECTED:
			if (!do_rand_again(state))
				state = client_get_next_state(state);
			break;
		}
		i_assert(state <= STATE_LOGOUT);

		if (states[state].login_state > client->login_state ||
		    (client->login_state != LSTATE_NONAUTH &&
		     (state == STATE_AUTHENTICATE || state == STATE_LOGIN))) {
			/* can't do this now */
			continue;
		}

		client->plan[client->plan_size++] = state;
		if ((states[state].flags & FLAG_STATECHANGE) != 0)
			break;
	}

	i_assert(client->plan_size > 0);
	state = client->plan[0];
	i_assert(states[state].login_state <= client->login_state);
	return state;
}

static bool client_pending_cmds_allow_statechange(struct client *client,
						  enum client_state state)
{
	enum login_state old_lstate, new_lstate = 0;
	struct command *const *cmds;
	unsigned int i, count;

	new_lstate = flags2login_state(states[state].flags);

	cmds = array_get(&client->commands, &count);
	for (i = 0; i < count; i++) {
		if ((states[cmds[i]->state].flags & FLAG_STATECHANGE) != 0)
			return FALSE;

		old_lstate = states[cmds[i]->state].login_state;
		if (new_lstate < old_lstate)
			return FALSE;
		if (new_lstate == old_lstate && new_lstate == LSTATE_SELECTED)
			return FALSE;
	}
	return TRUE;
}

int client_plan_send_more_commands(struct client *client)
{
	enum state_flags pending_flags;
	enum login_state new_lstate;
	enum client_state state;

	while (array_count(&client->commands) < MAX_COMMAND_QUEUE_LEN) {
		state = client_update_plan(client);
		i_assert(state <= STATE_LOGOUT);

		if (client->append_unfinished)
			break;
		if (conf.no_pipelining && array_count(&client->commands) > 0)
			break;

		if ((states[state].flags & FLAG_STATECHANGE) != 0) {
			/* this command would change the state. check if there
			   are any pending commands that don't like the
			   change */
			if (!client_pending_cmds_allow_statechange(client,
								   state))
				break;
		}
		pending_flags = client_get_pending_cmd_flags(client,
							     &new_lstate);
		if ((states[state].flags & FLAG_STATECHANGE) == 0 &&
		    (pending_flags & FLAG_STATECHANGE) != 0) {
			/* we're changing state. allow this command if its
			   required login_state is lower than the current
			   state or the state we're changing to. */
			if (new_lstate <= states[state].login_state ||
			    client->login_state < states[state].login_state)
				break;
		}
		if ((states[state].flags & FLAG_MSGSET) != 0 &&
		    (pending_flags & (FLAG_EXPUNGES | FLAG_STATECHANGE)) != 0) {
			/* msgset may become invalid if we send it now */
			break;
		}
		if (state == STATE_SEARCH && client->search_ctx != NULL) {
			/* there can be only one search running at a time */
			continue;
		}

		if (client_plan_send_next_cmd(client) < 0)
			return -1;
	}

	if (!client->delayed && do_rand(STATE_DELAY)) {
		counters[STATE_DELAY]++;
		client_delay(client, rand() % DELAY_MSECS);
	}
	return 0;
}

static int client_append_more(struct client *client)
{
	struct mailbox_source *source = client->storage->source;
	struct istream *input, *input2;
	off_t ret;

	i_assert(client->append_vsize_left > 0);

	input = i_stream_create_limit(source->input, client->append_psize);
	input2 = i_stream_create_crlf(input);
	i_stream_skip(input2, client->append_skip);
	ret = o_stream_send_istream(client->output, input2);
	errno = input->stream_errno;
	i_stream_unref(&input2);
	i_stream_unref(&input);

	if (ret < 0) {
		if (errno != 0)
			i_error("APPEND failed: %m");
		return -1;
	}
	i_assert((uoff_t)ret <= client->append_vsize_left);
	client->append_vsize_left -= ret;
	client->append_skip += ret;

	if (client->append_vsize_left > 0) {
		/* unfinished */
		o_stream_set_flush_pending(client->output, TRUE);
		return 0;
	}

	/* finished this mail */
	client->append_started = FALSE;
	if ((client->capabilities & CAP_MULTIAPPEND) != 0 &&
	    states[STATE_APPEND].probability_again != 0 &&
	    client->plan_size > 0 && client->plan[0] == STATE_APPEND) {
		/* we want to append another message.
		   do it in the same transaction. */
		if (client_plan_send_next_cmd(client) < 0)
			return -1;
		if (client->append_started || !client->append_unfinished) {
			/* multiappend started / finished */
			return 0;
		}
		/* we didn't append a second message after all */
	}

	client->append_unfinished = FALSE;
	client->append_can_send = FALSE;
	o_stream_send_str(client->output, "\r\n");
	return 0;
}

int client_append(struct client *client, const char *args, bool add_datetime,
		  command_callback_t *callback, struct command **cmd_r)
{
	struct mailbox_source *source = client->storage->source;
	string_t *cmd;
	time_t t;
	int tz;

	*cmd_r = NULL;

	i_assert(client->append_vsize_left == 0);
	mailbox_source_get_next_size(source, &client->append_psize,
				     &client->append_vsize_left, &t, &tz);
	client->append_offset = source->input->v_offset;
	client->append_skip = 0;
	client->append_started = TRUE;

	cmd = t_str_new(128);
	if (client->append_unfinished) {
		/* MULTIAPPEND contination */
	} else {
		str_append(cmd, "APPEND ");
	}
	str_append(cmd, args);
	if (add_datetime)
		str_printfa(cmd, " \"%s\"", imap_to_datetime_tz(t, tz));

	str_printfa(cmd, " {%"PRIuUOFF_T, client->append_vsize_left);
	if ((client->capabilities & CAP_LITERALPLUS) != 0)
		str_append_c(cmd, '+');
	str_append_c(cmd, '}');

	if (client->append_unfinished) {
		/* continues the last APPEND call */
		str_append(cmd, "\r\n");
		o_stream_send_str(client->output, str_c(cmd));
	} else {
		client->state = STATE_APPEND;
		*cmd_r = command_send(client, str_c(cmd), callback);
		client->append_unfinished = TRUE;
	}

	if ((client->capabilities & CAP_LITERALPLUS) == 0) {
		/* we'll have to wait for "+" */
		i_stream_skip(source->input, client->append_psize);
		return 0;
	}

	client->append_can_send = TRUE;
	return client_append_more(client);
}

int client_append_full(struct client *client, const char *mailbox,
		       const char *flags, const char *datetime,
		       command_callback_t *callback, struct command **cmd_r)
{
	string_t *args;
	bool add_datetime = FALSE;

	args = t_str_new(128);
	if (!client->append_unfinished) {
		str_printfa(args, "\"%s\"", mailbox != NULL ? mailbox :
			    client->storage->name);
	}
	if (flags != NULL)
		str_printfa(args, " (%s)", flags);
	if (datetime == NULL)
		add_datetime = TRUE;
	else if (*datetime != '\0')
		str_printfa(args, " \"%s\"", datetime);

	return client_append(client, str_c(args), add_datetime,
			     callback, cmd_r);
}

int client_append_random(struct client *client)
{
	const char *flags = NULL, *datetime = NULL;
	struct command *cmd;

	if ((rand() % 2) == 0) {
		flags = mailbox_view_get_random_flags(client->view,
						      client->idx);
	}
	if ((rand() % 2) == 0)
		datetime = "";
	return client_append_full(client, NULL, flags, datetime,
				  state_callback, &cmd);
}

int client_append_continue(struct client *client)
{
	i_stream_seek(client->storage->source->input,
		      client->append_offset);
	return client_append_more(client);
}

static void
metadata_update_dirty(struct message_metadata_dynamic *metadata, bool set)
{
	if (set) {
		if (metadata->flagchange_dirty_type == FLAGCHANGE_DIRTY_MAYBE)
			metadata->flagchange_dirty_type = FLAGCHANGE_DIRTY_NO;
	} else {
		if (metadata->flagchange_dirty_type != FLAGCHANGE_DIRTY_WAITING)
			metadata->flagchange_dirty_type = FLAGCHANGE_DIRTY_YES;
	}
}

static void
seq_range_flags_ref(struct client *client,
		    const ARRAY_TYPE(seq_range) *seq_range,
		    int diff, bool update_dirty)
{
	struct message_metadata_dynamic *metadata;
	const struct seq_range *range;
	unsigned int i, count;
	uint32_t seq;

	if (!array_is_created(seq_range))
		return;

	range = array_get(seq_range, &count);
	for (i = 0; i < count; i++) {
		for (seq = range[i].seq1; seq <= range[i].seq2; seq++) {
			metadata = array_idx_modifiable(&client->view->messages,
							seq - 1);
			if (update_dirty)
				metadata_update_dirty(metadata, diff < 0);
			if (diff < 0) {
				/* if fetch_refcount=0 the message got expunged
				   before tagged FETCH reply. we already
				   complained about it. */
				if (metadata->fetch_refcount > 0)
					metadata->fetch_refcount--;
			} else {
				metadata->fetch_refcount++;
			}
		}
	}
}

struct store_verify_context {
	struct client *client;
	enum mail_flags flags_mask;
	uint8_t *keywords_bitmask;
	unsigned int max_keyword_bit;
	char type;
};

static bool
store_verify_parse(struct store_verify_context *ctx, struct client *client,
		   char type, const char *flags)
{
	const char *const *tmp;
	unsigned int max_size, idx;

	memset(ctx, 0, sizeof(*ctx));
	ctx->client = client;
	ctx->type = type;

	max_size = I_MAX(client->view->keyword_bitmask_alloc_size,
			 (array_count(&client->view->keywords) + 7) / 8);
	ctx->max_keyword_bit = 0;
	ctx->keywords_bitmask = max_size == 0 ? NULL : t_malloc0(max_size);
	if (*flags == '(')
		flags = t_strcut(flags + 1, ')');
	if (*flags == '\0')
		return FALSE;

	for (tmp = t_strsplit(flags, " "); *tmp != NULL; tmp++) {
		if (**tmp == '\\')
			ctx->flags_mask |= mail_flag_parse(*tmp);
		else if (!mailbox_view_keyword_find(client->view, *tmp, &idx)) {
			client_state_error(client,
				"STORE didn't create keyword: %s", *tmp);
		} else {
			/* @UNSAFE */
			i_assert(idx/8 < max_size);
			ctx->keywords_bitmask[idx/8] |= 1 << (idx%8);
			ctx->max_keyword_bit = I_MAX(ctx->max_keyword_bit, idx);
		}
	}
	return TRUE;
}

static bool
store_verify_seq(struct store_verify_context *ctx, uint32_t seq)
{
	struct message_metadata_dynamic *metadata;
	enum mail_flags test_flags, test_flags_result;
	const char *expunge_state;
	unsigned int i;
	bool ret, set, fail, mask;

	metadata = array_idx_modifiable(&ctx->client->view->messages, seq - 1);
	expunge_state = metadata->ms == NULL ? "?" :
		metadata->ms->expunged ? "yes" : "no";
	if ((metadata->mail_flags & MAIL_FLAGS_SET) == 0 ||
	    metadata->flagchange_dirty_type == FLAGCHANGE_DIRTY_YES) {
		client_state_error(ctx->client,
			"STORE didn't return FETCH FLAGS for seq %u "
			"(expunged=%s)", seq, expunge_state);
		return FALSE;
	}

	test_flags = metadata->mail_flags;
	test_flags_result = ctx->flags_mask;
	switch (ctx->type) {
	case '+':
		test_flags &= ctx->flags_mask;
		break;
	case '-':
		test_flags &= ctx->flags_mask;
		test_flags_result = 0;
		break;
	case '\0':
		break;
	}

	if (test_flags != test_flags_result) {
		client_state_error(ctx->client,
			"STORE didn't update flags for seq %u (expunged=%s)",
			seq, expunge_state);
		return FALSE;
	}

	ret = TRUE;
	for (i = 0; i < ctx->max_keyword_bit; i++) {
		set = i/8 < ctx->client->view->keyword_bitmask_alloc_size ?
			(metadata->keyword_bitmask[i/8] & (1 << (i%8))) != 0 :
			FALSE;
		mask = (ctx->keywords_bitmask[i/8] & (1 << (i%8))) != 0;
		switch (ctx->type) {
		case '+':
			fail = mask && !set;
			break;
		case '-':
			fail = mask && set;
			break;
		default:
			fail = mask != set;
			break;
		}
		if (fail) {
			struct mailbox_keyword *kw;

			kw = mailbox_view_keyword_get(ctx->client->view, i);
			client_state_error(ctx->client,
				"STORE didn't update keyword %s for seq %u "
				"(expunged=%s)",
				kw->name->name, seq, expunge_state);
			ret = FALSE;
		}
	}
	return ret;
}

static void
store_verify_result(struct client *client, char type, const char *flags,
		    const ARRAY_TYPE(seq_range) *seq_range)
{
	const struct seq_range *range;
	unsigned int i, count;
	struct store_verify_context ctx;
	uint32_t seq;

	if (!store_verify_parse(&ctx, client, type, flags))
		return;

	/* make sure all the referenced messages have the changes */
	range = array_get(seq_range, &count);
	for (i = 0; i < count; i++) {
		for (seq = range[i].seq1; seq <= range[i].seq2; seq++)
			store_verify_seq(&ctx, seq);
	}
}

static void client_try_create_mailbox(struct client *client)
{
	const char *str;

	if (!client->try_create_mailbox)
		return;

	str = t_strdup_printf("CREATE \"%s\"", client->storage->name);
	client->state = STATE_MCREATE;
	command_send(client, str, state_callback);
}

void client_handle_resp_text_code(struct client *client,
				  const struct imap_arg *args)
{
	struct mailbox_view *view = client->view;
	const char *key, *value, *p;

	if (args->type != IMAP_ARG_ATOM) {
		client_input_error(client, "Invalid resp-text");
		return;
	}

	value = imap_args_to_str(args);
	if (*value != '[') {
		if (*value == '\0')
			client_input_warn(client, "Missing text in resp-text");
		return;
	}
	p = strchr(value, ']');
	if (p == NULL) {
		client_input_error(client, "Missing ']' in resp-text");
		return;
	}
	if (p[1] == '\0' || p[1] != ' ' || p[2] == '\0')
		client_input_warn(client, "Missing text in resp-text");
	key = t_strdup_until(value + 1, p);

	value = strchr(key, ' ');
	if (value == NULL)
		value = "";
	else
		key = t_strdup_until(key, value++);

	if (strcmp(key, "READ-WRITE") == 0)
		view->readwrite = TRUE;
	else if (strcmp(key, "HIGHESTMODSEQ") == 0) {
		/* reset previous MODSEQ updates */
		client->highest_untagged_modseq = 0;
		if (str_to_uint64(value, &view->highest_modseq) < 0) {
			client_input_warn(client,
					  "Invalid HIGHESTMODSEQ %s", value);
		}
	} else if (strcmp(key, "CAPABILITY") == 0) {
		client_capability_parse(client, value);
	} else if (strcmp(key, "CLOSED") == 0) {
		/* QRESYNC: SELECTing another mailbox in SELECTED state */
		if (client->login_state != LSTATE_SELECTED) {
			client_input_warn(client,
				"CLOSED code sent in non-selected state %d",
				client->login_state);
		} else {
			/* we're temporarily in AUTHENTICATED state */
			client_mailbox_close(client);
		}
	} else if (strcmp(key, "PERMANENTFLAGS") == 0) {
		if (mailbox_state_set_permanent_flags(view, args + 1) < 0)
			client_input_error(client, "Broken PERMANENTFLAGS");
	} else if (strcmp(key, "UIDNEXT") == 0) {
		if (view->select_uidnext == 0)
			view->select_uidnext = strtoul(value, NULL, 10);
	} else if (strcmp(key, "UIDVALIDITY") == 0) {
		unsigned int new_uidvalidity;

		new_uidvalidity = strtoul(value, NULL, 10);
		if (new_uidvalidity != view->storage->uidvalidity) {
			if (view->storage->uidvalidity != 0 &&
			    !conf.no_tracking) {
				i_error("UIVALIDITY changed: %u -> %u",
					view->storage->uidvalidity,
					new_uidvalidity);
			}
			view->storage->uidvalidity = new_uidvalidity;
		}
		if (client->qresync_select_cache != NULL &&
		    client->qresync_select_cache->uidvalidity ==
		    view->storage->uidvalidity) {
			/* remember how many messages we currently have */
			client->qresync_pending_exists =
				array_count(&view->uidmap);
			mailbox_view_restore_offline_cache(view,
				client->qresync_select_cache);
			client_log_mailbox_view(client);
		}
	}
}

void client_handle_tagged_reply(struct client *client, struct command *cmd,
				const struct imap_arg *args,
				enum command_reply reply)
{
	const char *arg;

	/* command finished - we can update highest-modseq based on the
	   received MODSEQ values. do this before anything else to make sure
	   we can use the updated highest-modseq in e.g. view closing. */
	if (client->view->highest_modseq < client->highest_untagged_modseq)
		client->view->highest_modseq = client->highest_untagged_modseq;
	client->highest_untagged_modseq = 0;

	if (!imap_arg_get_atom(args, &arg))
		arg = NULL;

	/* Keep track of login_state */
	if (strncasecmp(cmd->cmdline, "select ", 7) == 0 ||
	    strncasecmp(cmd->cmdline, "examine ", 8) == 0) {
		if (reply == REPLY_OK)
			client->login_state = LSTATE_SELECTED;
		if (client->qresync_select_cache != NULL) {
			/* SELECT with QRESYNC finished */
			if (reply == REPLY_OK) {
				client_exists(client,
					      client->qresync_pending_exists);
				client->qresync_pending_exists = 0;
			}
			mailbox_offline_cache_unref(&client->qresync_select_cache);
		}
	} else if (strcasecmp(cmd->cmdline, "close") == 0 ||
		   strcasecmp(cmd->cmdline, "unselect") == 0) {
		if (reply == REPLY_OK && client->login_state == LSTATE_SELECTED)
			client_mailbox_close(client);
	}

	client_handle_resp_text_code(client, args);
}

static int client_handle_cmd_reply(struct client *client, struct command *cmd,
				   const struct imap_arg *args,
				   enum command_reply reply)
{
	const char *str, *line;
	unsigned int i;

	line = imap_args_to_str(args);
	switch (reply) {
	case REPLY_OK:
		if (cmd->state != STATE_DISCONNECT &&
		    (cmd->state != STATE_LOGOUT || !client->seen_bye))
			counters[cmd->state]++;
		if (cmd->state == STATE_AUTHENTICATE ||
		    cmd->state == STATE_LOGIN) {
			/* update before handling resp-text, so that
			   postlogin_capability is set */
			client->login_state = LSTATE_AUTH;
		}
		break;
	case REPLY_NO:
		switch (cmd->state) {
		case STATE_COPY:
		case STATE_MCREATE:
		case STATE_MDELETE:
			break;
		case STATE_FETCH:
		case STATE_FETCH2:
			/* possibly tried to fetch expunged messages.
			   don't hide all errors though. */
			if (strstr(line, "no longer exist") != NULL) {
				/* Zimbra */
				break;
			}
			if (strstr(line, "No matching messages") != NULL) {
				/* Cyrus */
				break;
			}
			if (strstr(line, "have been expunged") != NULL) {
				/* Archiveopteryx */
				break;
			}
			/* fall through */
		case STATE_STORE:
		case STATE_STORE_DEL:
			if (strcmp(line, "Cannot store on expunged messages") == 0) {
				/* Archiveopteryx */
				break;
			}
			if (strcmp(line, "STORE completed") == 0 ||
			    strcmp(line, "STORE failed") == 0) {
				/* Zimbra */
				break;
			}
			if (strstr(line, "have been deleted") != NULL) {
				/* Communigate Pro (FETCH/STORE) */
				break;
			}
			if (strstr(line, "Document has been deleted") != NULL) {
				/* Domino (FETCH/STORE/EXPUNGE) */
				break;
			}
		case STATE_APPEND:
			if (client->try_create_mailbox)
				break;
			/* fall through */
		default:
			client_state_error(client, "%s failed",
					   states[cmd->state].name);
			break;
		}
		break;

	case REPLY_BAD:
		client_input_warn(client, "%s replied BAD",
				  states[cmd->state].name);
		return -1;
	case REPLY_CONT:
		if (client->idle_wait_cont) {
			client->idle_wait_cont = FALSE;
			return 0;
		}
		if (cmd->state == STATE_APPEND) {
			/* finish appending */
			if (client_append_continue(client) < 0)
				return -1;
			return 0;
		}

		client_input_error(client, "%s: Unexpected continuation",
				   states[cmd->state].name);
		return -1;
	}
	/* call after login_state has been updated */
	client_handle_tagged_reply(client, cmd, args, reply);

	switch (cmd->state) {
	case STATE_AUTHENTICATE:
	case STATE_LOGIN:
		if (reply != REPLY_OK) {
			/* authentication failed */
			return -1;
		}

		/* successful logins, create some more clients */
		for (i = 0; i < 3 && !stalled && !no_new_clients; i++) {
			if (array_count(&clients) >= conf.clients_count)
				break;

			client_new(array_count(&clients),
				   client->storage->source,
				   user_get_random());
		}
		break;
	case STATE_LIST:
		if (reply == REPLY_OK)
			client_mailboxes_list_end(client);
		break;
	case STATE_SELECT:
		if (reply == REPLY_NO)
			client_try_create_mailbox(client);
		break;
	case STATE_STATUS:
		if (reply == REPLY_NO)
			client_try_create_mailbox(client);
		break;
	case STATE_FETCH:
		seq_range_flags_ref(client, &cmd->seq_range, -1, TRUE);
		break;
	case STATE_STORE:
	case STATE_STORE_DEL: {
		const char *p;
		char type;
		bool silent;

		if (strncmp(cmd->cmdline, "UID STORE ", 10) == 0) {
			/* FIXME: we should probably try handle this as well.
			   used by profile. */
			break;
		}

		i_assert(strncmp(cmd->cmdline, "STORE ", 6) == 0);
		p = strchr(cmd->cmdline + 6, ' ');
		i_assert(p != NULL);
		p++;
		type = *p == '+' || *p == '-' ? *p++ : '\0';
		silent = strncmp(p, "FLAGS.SILENT", 12) == 0;

		if (!silent && client->storage->assign_flag_owners &&
		    reply == REPLY_OK) {
			i_assert(type != '\0');
			i_assert(strncmp(p, "FLAGS ", 6) == 0);
			p += 6;
			store_verify_result(client, type, p, &cmd->seq_range);
		}

		seq_range_flags_ref(client, &cmd->seq_range, -1, TRUE);
		if (silent)
			seq_range_flags_ref(client, &cmd->seq_range, -1, TRUE);
		break;
	}
	case STATE_COPY:
		if (reply == REPLY_NO) {
			if (imap_arg_atom_equals(args, "[TRYCREATE]")) {
				str = t_strdup_printf("CREATE \"%s\"",
						      conf.copy_dest);
				client->state = STATE_COPY;
				command_send(client, str, state_callback);
				break;
			}
			client_state_error(client, "COPY failed");
		}
		break;
	case STATE_APPEND:
		if (reply == REPLY_NO)
			client_try_create_mailbox(client);
		break;
	case STATE_LOGOUT:
		if (client->login_state != LSTATE_NONAUTH) {
			/* untagged BYE sets state to DISCONNECT, so we
			   shouldn't get here. */
			client_state_error(client, "Server didn't send BYE");
		}
		client->login_state = LSTATE_NONAUTH;
		return -1;
	case STATE_IDLE:
		client->idling = FALSE;
		client->idle_done_sent = FALSE;
		break;
	case STATE_DISCONNECT:
		return -1;
	default:
		break;
	}

	return 0;
}

bool client_get_random_seq_range(struct client *client,
				 ARRAY_TYPE(seq_range) *range,
				 unsigned int count,
				 enum client_random_flag_type flag_type)
{
	struct message_metadata_dynamic *metadata;
	unsigned int i, msgs, seq, owner, tries;
	bool dirty_flags;

	msgs = array_count(&client->view->uidmap);
	if (count == 0 || msgs == 0)
		return FALSE;

	dirty_flags = flag_type == CLIENT_RANDOM_FLAG_TYPE_STORE ||
		flag_type == CLIENT_RANDOM_FLAG_TYPE_STORE_SILENT;
	msgs = array_count(&client->view->uidmap);
	for (i = tries = 0; i < count && tries < count*3; tries++) {
		seq = rand() % msgs + 1;
		metadata = array_idx_modifiable(&client->view->messages,
						seq - 1);
		owner = metadata->ms == NULL ? 0 :
			metadata->ms->owner_client_idx1;

		if (dirty_flags) {
			if (owner == client->idx+1) {
				/* we can change this */
			} else if (owner != 0) {
				/* someone else owns this */
				continue;
			} else if (client->storage->assign_msg_owners) {
				/* not assigned to anyone yet, wait */
				continue;
			}
		}

		seq_range_array_add_with_init(range, 10, seq);
		i++;
	}
	if (flag_type != CLIENT_RANDOM_FLAG_TYPE_NONE &&i > 0) {
		seq_range_flags_ref(client, range, 1, TRUE);
		if (flag_type == CLIENT_RANDOM_FLAG_TYPE_STORE_SILENT) {
			/* flag stays dirty until we can FETCH it after the
			   STORE has successfully finished. */
			seq_range_flags_ref(client, range, 1, TRUE);
		}
	}
	return i > 0;
}

static void
client_dirty_all_flags(struct client *client, ARRAY_TYPE(seq_range) *seq_range,
		       enum client_random_flag_type flag_type)
{
	struct seq_range range;

	range.seq1 = 1;
	range.seq2 = array_count(&client->view->uidmap);
	i_array_init(seq_range, 2);
	array_append(seq_range, &range, 1);

	seq_range_flags_ref(client, seq_range, 1, TRUE);
	if (flag_type == CLIENT_RANDOM_FLAG_TYPE_STORE_SILENT)
		seq_range_flags_ref(client, seq_range, 1, TRUE);
}

static void seq_range_to_imap_range(const ARRAY_TYPE(seq_range) *seq_range,
				    string_t *dest)
{
	const struct seq_range *range;
	unsigned int i, count;

	range = array_get(seq_range, &count);
	for (i = 0; i < count; i++) {
		if (i > 0)
			str_append_c(dest, ',');
		str_printfa(dest, "%u", range[i].seq1);
		if (range[i].seq1 != range[i].seq2)
			str_printfa(dest, ":%u", range[i].seq2);
	}
}

static void client_select_qresync(struct client *client)
{
	struct mailbox_offline_cache *cache = client->storage->cache;
	string_t *cmd;

	if (!client->qresync_enabled)
		command_send(client, "ENABLE QRESYNC", state_callback);

	cmd = t_str_new(128);
	str_printfa(cmd, "SELECT \"%s\"", client->storage->name);

	if (cache != NULL) {
		/* we have a cache - select using it */
		str_printfa(cmd, " (QRESYNC (%u %llu))", cache->uidvalidity,
			    (unsigned long long)cache->highest_modseq);
		cache->refcount++;
		client->qresync_select_cache = cache;
	}
	command_send(client, str_c(cmd), state_callback);
}

int client_plan_send_next_cmd(struct client *client)
{
	enum client_state state;
	struct command *icmd;
	string_t *cmd;
	const char *str;
	enum client_random_flag_type flag_type;
	ARRAY_TYPE(seq_range) seq_range = ARRAY_INIT;
	unsigned int i, j, seq1, seq2, count, msgs, owner;

	state = client_eat_first_plan(client);

	msgs = array_count(&client->view->uidmap);
	if (msgs == 0 && states[state].login_state == LSTATE_SELECTED) {
		/* no messages, no point in doing this command */
		return 0;
	}

	if (client->append_unfinished && state != STATE_APPEND) {
		i_assert(state == STATE_LOGOUT);
		client_disconnect(client);
		return -1;
	}

	client->state = state;
	switch (state) {
	case STATE_AUTHENTICATE:
		command_send(client, "AUTHENTICATE plain", auth_plain_callback);
		break;
	case STATE_LOGIN:
		o_stream_cork(client->output);
		str = t_strdup_printf("LOGIN \"%s\" \"%s\"",
				      str_escape(client->user->username),
				      str_escape(client->user->password));
		command_send(client, str, state_callback);
		if (conf.qresync)
			command_send(client, "ENABLE QRESYNC", state_callback);
		o_stream_uncork(client->output);
		break;
	case STATE_LIST:
		//str = t_strdup_printf("LIST \"\" * RETURN (X-STATUS (MESSAGES))");
		client_mailboxes_list_begin(client);
		str = t_strdup_printf("LIST \"\" *");
		command_send(client, str, state_callback);
		break;
	case STATE_MCREATE:
		if (rand() % 2)
			str = t_strdup_printf("CREATE \"test%c%d\"", 
					      IMAP_HIERARCHY_SEP, rand() % 20);
		else
			str = t_strdup_printf("CREATE \"test%c%d%c%d\"", 
					      IMAP_HIERARCHY_SEP, rand() % 20,
					      IMAP_HIERARCHY_SEP, rand() % 20);
		command_send(client, str, state_callback);
		break;
	case STATE_MDELETE:
		if (rand() % 2)
			str = t_strdup_printf("DELETE \"test%c%d\"", 
					      IMAP_HIERARCHY_SEP, rand() % 20);
		else
			str = t_strdup_printf("DELETE \"test%c%d%c%d\"", 
					      IMAP_HIERARCHY_SEP, rand() % 20,
					      IMAP_HIERARCHY_SEP, rand() % 20);
		command_send(client, str, state_callback);
		break;
	case STATE_SELECT:
		if (client->login_state == LSTATE_SELECTED) {
			/* already selected, don't do it agai */
			break;
		}
		if (conf.qresync) {
			client_select_qresync(client);
			break;
		}
		str = t_strdup_printf("SELECT \"%s\"", client->storage->name);
		if ((client->capabilities & CAP_CONDSTORE) != 0)
			str = t_strconcat(str, " (CONDSTORE)", NULL);
		command_send(client, str, state_callback);
		break;
	case STATE_FETCH: {
		static const char *fields[] = {
			"UID", "FLAGS", "ENVELOPE", "INTERNALDATE",
			"BODY", "BODYSTRUCTURE", "RFC822.SIZE"
		};
		static const char *header_fields[] = {
			"From", "To", "Cc", "Subject", "References",
			"In-Reply-To", "Message-ID", "Delivered-To"
		};
		count = I_MIN(msgs, 100);
		if (!client_get_random_seq_range(client, &seq_range, count,
						 CLIENT_RANDOM_FLAG_TYPE_FETCH))
			break;

		cmd = t_str_new(512);
		str_append(cmd, "FETCH ");
		seq_range_to_imap_range(&seq_range, cmd);
		str_append(cmd, " (");
		/* knowing UID and FLAGS when checkpointing improves
		   detecting problems. UID is required with QRESYNC to get
		   VANISHED working correctly for UIDs we haven't yet seen */
		if (conf.checkpoint_interval > 0 || client->qresync_enabled)
			str_append(cmd, "UID ");
		if (conf.checkpoint_interval > 0)
			str_append(cmd, "FLAGS ");
		for (i = (rand() % 4) + 1; i > 0; i--) {
			if ((rand() % 4) != 0) {
				str_append(cmd, fields[rand() %
						       N_ELEMENTS(fields)]);
			} else {
				str_append(cmd, "BODY.PEEK[HEADER.FIELDS (");
				for (j = (rand() % 4) + 1; j > 0; j--) {
					int idx = rand() %
						N_ELEMENTS(header_fields);
					str_append(cmd, header_fields[idx]);
					if (j != 1)
						str_append_c(cmd, ' ');
				}
				str_append(cmd, ")]");
			}
			if (i != 1)
				str_append_c(cmd, ' ');
		}
		str_append_c(cmd, ')');
		icmd = command_send(client, str_c(cmd), state_callback);
		icmd->seq_range = seq_range;
		break;
	}
	case STATE_FETCH2: {
		static const char *fields[] = {
			"BODY.PEEK[HEADER]", "RFC822.HEADER",
			"BODY.PEEK[]", "BODY.PEEK[1]", "BODY.PEEK[TEXT]"
		};
		/* Fetch also UID so that error logging can show it in
		   case of problems */
		str = t_strdup_printf("FETCH %lu (UID %s)",
				      (random() % msgs) + 1,
				      fields[rand()%N_ELEMENTS(fields)]);
		command_send(client, str, state_callback);
		break;
	}
	case STATE_SEARCH:
		search_command_send(client);
		break;
	case STATE_SORT: {
		static const char *fields[] = {
			"ARRIVAL", "CC", "DATE", "FROM", "SIZE", "SUBJECT", "TO"
		};
		cmd = t_str_new(512);
		str_append(cmd, "SORT (");
		i = rand() % N_ELEMENTS(fields);
		j = rand() % N_ELEMENTS(fields);

		if (rand() % 3 == 0)
			str_append(cmd, "REVERSE ");
		str_append(cmd, fields[i]);
		if (rand() % 3 == 0 && i != j) {
			str_append_c(cmd, ' ');
			if (rand() % 3 == 0)
				str_append(cmd, "REVERSE ");
			str_append(cmd, fields[j]);
		}
		str_append(cmd, ") US-ASCII ");
		switch (rand() % 3) {
		case 0:
			str_append(cmd, "ALL");
			break;
		case 1:
			str_append(cmd, "FLAGGED");
			break;
		case 2:
			str_printfa(cmd, "%u:%u", (rand() % msgs) + 1,
				    (rand() % msgs) + 1);
			break;
		}
		command_send(client, str_c(cmd), state_callback);
		break;
	}
	case STATE_THREAD:
		command_send(client, "THREAD REFERENCES US-ASCII ALL", state_callback);
		break;
	case STATE_COPY:
		i_assert(conf.copy_dest != NULL);

		seq1 = (rand() % msgs) + 1;
		seq2 = (rand() % (msgs - seq1 + 1));
		seq2 = seq1 + I_MIN(seq2, 5);
		str = t_strdup_printf("COPY %u:%u %s",
				      seq1, seq2, conf.copy_dest);
		command_send(client, str, state_callback);
		break;
	case STATE_STORE:
		count = rand() % (msgs < 10 ? msgs : I_MIN(msgs/5, 50));
		flag_type = conf.checkpoint_interval == 0 && rand() % 2 == 0 ?
			CLIENT_RANDOM_FLAG_TYPE_STORE_SILENT :
			CLIENT_RANDOM_FLAG_TYPE_STORE;
		if (!client_get_random_seq_range(client, &seq_range, count,
						 flag_type))
			break;

		cmd = t_str_new(512);
		str_append(cmd, "STORE ");
		seq_range_to_imap_range(&seq_range, cmd);
		str_append_c(cmd, ' ');
		switch (rand() % 3) {
		case 0:
			str_append_c(cmd, '+');
			break;
		case 1:
			str_append_c(cmd, '-');
			break;
		default:
			if (client->storage->assign_flag_owners) {
				/* we must not reset any flags */
				str_append_c(cmd, '+');
			}
			break;
		}
		str_append(cmd, "FLAGS");
		if (flag_type == CLIENT_RANDOM_FLAG_TYPE_STORE_SILENT)
			str_append(cmd, ".SILENT");
		str_printfa(cmd, " (%s)",
			    mailbox_view_get_random_flags(client->view,
							  client->idx));

		icmd = command_send(client, str_c(cmd), state_callback);
		icmd->seq_range = seq_range;
		break;
	case STATE_STORE_DEL:
		owner = client->storage->
			flags_owner_client_idx1[MAIL_FLAG_DELETED_IDX];
		if (client->storage->assign_flag_owners &&
		    owner != client->idx + 1) {
			/* own_msgs - only one client can delete messages */
			break;
		}

		if (msgs > conf.message_count_threshold + 5) {
			count = rand() % (msgs - conf.message_count_threshold);
			if (count > 1000) {
				/* avoid "command line too long" errors */
				count = 1000;
			}
		} else {
			count = rand() % 5;
		}
		flag_type = conf.checkpoint_interval == 0 && rand() % 2 == 0 ?
			CLIENT_RANDOM_FLAG_TYPE_STORE_SILENT :
			CLIENT_RANDOM_FLAG_TYPE_STORE;

		if (!client->storage->seen_all_recent &&
		    !client->storage->assign_msg_owners &&
		    conf.checkpoint_interval != 0 && msgs > 0) {
			/* expunge everything so we can start checking RECENT
			   counts */
			client_dirty_all_flags(client, &seq_range, flag_type);
		} else {
			if (!client_get_random_seq_range(client, &seq_range,
							 count, flag_type))
				break;
		}

		cmd = t_str_new(512);
		str_append(cmd, "STORE ");
		seq_range_to_imap_range(&seq_range, cmd);
		str_append(cmd, " +FLAGS");
		if (flag_type == CLIENT_RANDOM_FLAG_TYPE_STORE_SILENT)
			str_append(cmd, ".SILENT");
		str_append(cmd, " \\Deleted");

		icmd = command_send(client, str_c(cmd), state_callback);
		icmd->seq_range = seq_range;
		break;
	case STATE_EXPUNGE:
		command_send(client, "EXPUNGE", state_callback);
		break;
	case STATE_APPEND:
		if (msgs >= conf.message_count_threshold)
			break;

		if (client_append_random(client) < 0)
			return -1;
		break;
	case STATE_STATUS:
		str = t_strdup_printf("STATUS \"%s\" (MESSAGES UNSEEN RECENT)",
				      client->storage->name);
		command_send(client, str, state_callback);
		break;
	case STATE_NOOP:
		command_send(client, "NOOP", state_callback);
		break;
	case STATE_IDLE:
		command_send(client, "IDLE", state_callback);
		break;
	case STATE_CHECK:
		command_send(client, "CHECK", state_callback);
		break;
	case STATE_LOGOUT:
		command_send(client, "LOGOUT", state_callback);
		break;

	case STATE_BANNER:
	case STATE_DISCONNECT:
	case STATE_DELAY:
	case STATE_CHECKPOINT:
	case STATE_LMTP:
	case STATE_COUNT:
		i_unreached();
	}
	return 0;
}

void state_callback(struct client *client, struct command *cmd,
		    const struct imap_arg *args, enum command_reply reply)
{
	if (client_handle_cmd_reply(client, cmd, args + 1, reply) < 0)
		client_disconnect(client);
}

void client_cmd_reply_finish(struct client *client)
{
	if (client->checkpointing != NULL) {
		/* we're checkpointing */
		if (array_count(&client->commands) > 0)
			return;

		checkpoint_neg(client->storage);
		return;
	} else if (client->storage->checkpoint != NULL) {
		/* don't do anything until checkpointing is finished */
		return;
	} else if (client->state == STATE_LOGOUT) {
		return;
	}

	if (client_send_more_commands(client) < 0)
		client_disconnect(client);
}

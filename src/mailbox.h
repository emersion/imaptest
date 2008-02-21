#ifndef MAILBOX_H
#define MAILBOX_H

#include "seq-range-array.h"
#include "mail-types.h"

struct message_header {
	const char *name;
	const unsigned char *value;
	unsigned int value_len;
	unsigned int missing:1;
};
ARRAY_DEFINE_TYPE(message_header, struct message_header);

struct message_global {
	char *message_id;
	const char *body, *bodystructure, *envelope;
	uoff_t header_size, body_size, full_size, mime1_size;

	/* parsed fields: */
	const char *subject_utf8_tcase;
	time_t sent_date;
	int sent_date_tz;

	ARRAY_TYPE(message_header) headers;
};

struct message_metadata_static {
	uint32_t uid;
	unsigned int refcount;

	time_t internaldate;
	int internaldate_tz;
	unsigned int owner_client_idx1;

	struct message_global *msg;

	unsigned int expunged:1;
};

enum flagchange_dirty_type {
	FLAGCHANGE_DIRTY_NO = 0,
	/* We've sent at least one STORE command but haven't yet received any
	   FETCH replies for it */
	FLAGCHANGE_DIRTY_YES,
	/* We've received a FETCH reply, but there are multiple commands in
	   progress so we're most likely going to be receiving more FETCH
	   replies */
	FLAGCHANGE_DIRTY_WAITING,
	/* There's only one command in progress and we've received a FETCH
	   reply from it. However it's possible that it was actually an
	   unsolicited reply from server, and the actual expected FETCH reply
	   is still coming. */
	FLAGCHANGE_DIRTY_MAYBE
};

struct message_metadata_dynamic {
#define MAIL_FLAGS_SET 0x40000000
	/* flags and keywords are set only if MAIL_FLAGS_SET is set */
	enum mail_flags mail_flags;
	uint8_t *keyword_bitmask; /* [view->keyword_bitmask_alloc_size] */

	struct message_metadata_static *ms;
	/* Number of commands currently expected to return FETCH FLAGS for
	   this message. STORE +SILENT also increments this so dirtyness gets
	   handled right. */
	unsigned int fetch_refcount;

	enum flagchange_dirty_type flagchange_dirty_type;
};

struct mailbox_keyword_name {
	char *name;
	unsigned int owner_client_idx1;

	unsigned int seen_nonpermanent:1;
};

struct mailbox_keyword {
	struct mailbox_keyword_name *name;

	/* number of messages containing this keyword (that we know of) */
	unsigned int refcount;
	unsigned int flags_counter; /* should match view->flags_counter */

	unsigned int permanent:1;
};

struct mailbox_storage {
	struct mailbox_source *source;
	int refcount;
	char *name;

	struct mailbox_checkpoint_context *checkpoint;

	/* we assume that uidvalidity doesn't change while imaptest
	   is running */
	unsigned int uidvalidity;

	/* static metadata for this mailbox. sorted by UID. */
	ARRAY_DEFINE(static_metadata, struct message_metadata_static *);
	ARRAY_DEFINE(keyword_names, struct mailbox_keyword_name *);
	/* List of UIDs that are definitely expunged. May contain UIDs that
	   have never even existed. */
	ARRAY_TYPE(seq_range) expunged_uids;

#define MAIL_FLAGS_OWN_COUNT 5
#define MAIL_FLAG_DELETED_IDX 2
	unsigned int flags_owner_client_idx1[MAIL_FLAGS_OWN_COUNT];

	unsigned int assign_msg_owners:1;
	unsigned int assign_flag_owners:1;
	unsigned int flag_owner_clients_assigned:1;
	unsigned int seen_all_recent:1;
	unsigned int dont_track_recent:1;
};

struct mailbox_view {
	struct mailbox_storage *storage;
	unsigned int keyword_bitmask_alloc_size;
	unsigned int flags_counter;
	unsigned int recent_count;
	unsigned int select_uidnext; /* UIDNEXT received on SELECT */

	/* all keywords used currently in a mailbox */
	ARRAY_DEFINE(keywords, struct mailbox_keyword);

	/* seq -> uid */
	ARRAY_DEFINE(uidmap, uint32_t);
	/* seq -> metadata */
	ARRAY_DEFINE(messages, struct message_metadata_dynamic);

	unsigned int keywords_can_create_more:1;
};

extern struct hash_table *storages;
extern const char *mail_flag_names[]; /* enum mail_flags names */

struct mailbox_storage *
mailbox_storage_get(struct mailbox_source *source, const char *name);
void mailbox_storage_unref(struct mailbox_storage **storage);

struct mailbox_view *mailbox_view_new(struct mailbox_storage *storage);
void mailbox_view_free(struct mailbox_view **_mailbox);

bool mailbox_view_keyword_find(struct mailbox_view *view, const char *name,
			       unsigned int *idx_r);
struct mailbox_keyword *mailbox_view_keyword_get(struct mailbox_view *view,
						 unsigned int idx);
void mailbox_view_keyword_add(struct mailbox_view *view, const char *name);
void mailbox_keywords_clear(struct mailbox_view *view,
			    struct message_metadata_dynamic *metadata);
void mailbox_view_keywords_realloc(struct mailbox_view *view,
				   unsigned int new_alloc_size);

enum mail_flags mail_flag_parse(const char *str);
const char *mail_flags_to_str(enum mail_flags flags);
const char *mailbox_view_keywords_to_str(struct mailbox_view *view,
					 const uint8_t *bitmask);
const char *mailbox_view_get_random_flags(struct mailbox_view *view,
					  unsigned int client_idx);

struct message_metadata_static *
message_metadata_static_lookup_seq(struct mailbox_view *view, uint32_t seq);
struct message_metadata_static *
message_metadata_static_get(struct mailbox_storage *storage, uint32_t uid);
void message_metadata_static_unref(struct mailbox_storage *storage,
				   struct message_metadata_static **ms);
void mailbox_view_expunge(struct mailbox_view *view, unsigned int seq);

bool mailbox_global_get_sent_date(struct message_global *msg,
				  time_t *date_r, int *tz_r);
bool mailbox_global_get_subject_utf8(struct mailbox_source *source,
				     struct message_global *msg,
				     const char **subject_r);

void mailboxes_init(void);
void mailboxes_deinit(void);

#endif

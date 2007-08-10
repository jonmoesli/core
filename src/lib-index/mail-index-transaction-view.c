/* Copyright (C) 2004-2006 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "buffer.h"
#include "seq-range-array.h"
#include "mail-index-private.h"
#include "mail-index-view-private.h"
#include "mail-index-transaction-private.h"

struct mail_index_view_transaction {
	struct mail_index_view view;
	struct mail_index_view_vfuncs *super;
	struct mail_index_transaction *t;

	struct mail_index_map *lookup_map;
	struct mail_index_header hdr;
};

static void _tview_close(struct mail_index_view *view)
{
	struct mail_index_view_transaction *tview =
		(struct mail_index_view_transaction *)view;
	struct mail_index_transaction *t = tview->t;

	if (tview->lookup_map != NULL)
		mail_index_unmap(&tview->lookup_map);

	tview->super->close(view);
	mail_index_transaction_unref(&t);
}

static uint32_t _tview_get_message_count(struct mail_index_view *view)
{
	struct mail_index_view_transaction *tview =
                (struct mail_index_view_transaction *)view;

	return view->map->hdr.messages_count +
		(tview->t->last_new_seq == 0 ? 0 :
		 tview->t->last_new_seq - tview->t->first_new_seq);
}

static const struct mail_index_header *
_tview_get_header(struct mail_index_view *view)
{
	struct mail_index_view_transaction *tview =
                (struct mail_index_view_transaction *)view;
	const struct mail_index_header *hdr;
	uint32_t next_uid;

	/* FIXME: header counters may not be correct */
	hdr = tview->super->get_header(view);

	next_uid = mail_index_transaction_get_next_uid(tview->t);
	if (next_uid != hdr->next_uid) {
		tview->hdr = *hdr;
		tview->hdr.next_uid = next_uid;
		hdr = &tview->hdr;
	}
	return hdr;
}

static int _tview_lookup_full(struct mail_index_view *view, uint32_t seq,
			      struct mail_index_map **map_r,
			      const struct mail_index_record **rec_r)
{
	struct mail_index_view_transaction *tview =
                (struct mail_index_view_transaction *)view;
	int ret;

	if (seq >= tview->t->first_new_seq) {
		/* FIXME: is this right to return index map..?
		   it's not there yet. */
		*map_r = view->index->map;
		*rec_r = mail_index_transaction_lookup(tview->t, seq);
		return 1;
	}

	ret = tview->super->lookup_full(view, seq, map_r, rec_r);
	if (ret <= 0)
		return ret;

	/* if we're expunged within this transaction, return 0 */
	return array_is_created(&tview->t->expunges) &&
		seq_range_exists(&tview->t->expunges, seq) ? 0 : 1;

}

static int _tview_lookup_uid(struct mail_index_view *view, uint32_t seq,
			     uint32_t *uid_r)
{
	struct mail_index_view_transaction *tview =
		(struct mail_index_view_transaction *)view;

	if (seq >= tview->t->first_new_seq) {
		*uid_r = mail_index_transaction_lookup(tview->t, seq)->uid;
		return 0;
	} else {
		return tview->super->lookup_uid(view, seq, uid_r);
	}
}

static int _tview_lookup_uid_range(struct mail_index_view *view,
				   uint32_t first_uid, uint32_t last_uid,
				   uint32_t *first_seq_r, uint32_t *last_seq_r)
{
	struct mail_index_view_transaction *tview =
		(struct mail_index_view_transaction *)view;
	const struct mail_index_record *rec;
	uint32_t seq;

	if (tview->super->lookup_uid_range(view, first_uid, last_uid,
					   first_seq_r, last_seq_r) < 0)
		return -1;

	if (tview->t->last_new_seq == 0) {
		/* no new messages, the results are final. */
		return 0;
	}

	rec = mail_index_transaction_lookup(tview->t, tview->t->first_new_seq);
	if (rec->uid == 0) {
		/* new messages don't have UIDs */
		return 0;
	}
	if (last_uid < rec->uid) {
		/* all wanted messages were existing */
		return 0;
	}

	/* at least some of the wanted messages are newly created */
	if (*first_seq_r == 0)
		*first_seq_r = tview->t->first_new_seq;

	seq = tview->t->last_new_seq;
	for (; seq >= tview->t->first_new_seq; seq--) {
		rec = mail_index_transaction_lookup(tview->t, seq);
		if (rec->uid <= last_uid) {
			*last_seq_r = seq;
			break;
		}
	}
	i_assert(seq >= tview->t->first_new_seq);
	return 0;
}

static int _tview_lookup_first(struct mail_index_view *view,
			       enum mail_flags flags, uint8_t flags_mask,
			       uint32_t *seq_r)
{
	struct mail_index_view_transaction *tview =
		(struct mail_index_view_transaction *)view;
	const struct mail_index_record *rec;
	unsigned int append_count;
	uint32_t seq, message_count;

	if (tview->super->lookup_first(view, flags, flags_mask, seq_r) < 0)
		return -1;

	if (*seq_r != 0)
		return 0;

	rec = array_get(&tview->t->appends, &append_count);
	seq = tview->t->first_new_seq;
	message_count = tview->t->last_new_seq;
	i_assert(append_count == message_count - seq + 1);

	for (; seq <= message_count; seq++, rec++) {
		if ((rec->flags & flags_mask) == (uint8_t)flags) {
			*seq_r = seq;
			break;
		}
	}

	return 0;
}

static struct mail_index_map *
tview_get_lookup_map(struct mail_index_view_transaction *tview)
{
	if (tview->lookup_map == NULL) {
		tview->lookup_map =
			mail_index_map_clone(tview->view.index->map);
	}
	return tview->lookup_map;
}

static int
_tview_lookup_ext_full(struct mail_index_view *view, uint32_t seq,
		       uint32_t ext_id, struct mail_index_map **map_r,
		       const void **data_r)
{
	struct mail_index_view_transaction *tview =
		(struct mail_index_view_transaction *)view;
	const ARRAY_TYPE(seq_array) *ext_buf;
	const void *data;
	unsigned int idx;

	i_assert(ext_id < array_count(&view->index->extensions));

	if (array_is_created(&tview->t->ext_rec_updates) &&
	    ext_id < array_count(&tview->t->ext_rec_updates)) {
		/* there are some ext updates in transaction.
		   see if there's any for this sequence. */
		ext_buf = array_idx(&tview->t->ext_rec_updates, ext_id);
		if (array_is_created(ext_buf) &&
		    mail_index_seq_array_lookup(ext_buf, seq, &idx)) {
			data = array_idx(ext_buf, idx);
			*map_r = tview_get_lookup_map(tview);
			*data_r = CONST_PTR_OFFSET(data, sizeof(uint32_t));
			return 1;
		}
	}

	/* not updated, return the existing value */
	if (seq < tview->t->first_new_seq) {
		return tview->super->lookup_ext_full(view, seq, ext_id,
						     map_r, data_r);
	}

	*map_r = view->index->map;
	*data_r = NULL;
	return 1;
}

static int _tview_get_header_ext(struct mail_index_view *view,
				 struct mail_index_map *map, uint32_t ext_id,
				 const void **data_r, size_t *data_size_r)
{
	struct mail_index_view_transaction *tview =
		(struct mail_index_view_transaction *)view;

	/* FIXME: check updates */
	return tview->super->get_header_ext(view, map, ext_id,
					    data_r, data_size_r);
}

static bool _tview_ext_get_reset_id(struct mail_index_view *view,
				    struct mail_index_map *map,
				    uint32_t ext_id, uint32_t *reset_id_r)
{
	struct mail_index_view_transaction *tview =
		(struct mail_index_view_transaction *)view;
	const uint32_t *reset_id_p;

	if (array_is_created(&tview->t->ext_reset_ids) &&
	    ext_id < array_count(&tview->t->ext_reset_ids) &&
	    map == tview->lookup_map) {
		reset_id_p = array_idx(&tview->t->ext_reset_ids, ext_id);
		*reset_id_r = *reset_id_p;
		return TRUE;
	}

	return tview->super->ext_get_reset_id(view, map, ext_id, reset_id_r);
}

static struct mail_index_view_vfuncs trans_view_vfuncs = {
	_tview_close,
        _tview_get_message_count,
	_tview_get_header,
	_tview_lookup_full,
	_tview_lookup_uid,
	_tview_lookup_uid_range,
	_tview_lookup_first,
	_tview_lookup_ext_full,
	_tview_get_header_ext,
	_tview_ext_get_reset_id
};

struct mail_index_view *
mail_index_transaction_open_updated_view(struct mail_index_transaction *t)
{
	struct mail_index_view_transaction *tview;

	if (t->view->syncing) {
		/* transaction view is being synced. while it's done, it's not
		   possible to add new messages, but the view itself might
		   change. so we can't make a copy of the view. */
		mail_index_view_ref(t->view);
		return t->view;
	}

	tview = i_new(struct mail_index_view_transaction, 1);
	mail_index_view_clone(&tview->view, t->view);
	tview->view.v = trans_view_vfuncs;
	tview->super = &t->view->v;
	tview->t = t;

	mail_index_transaction_ref(t);
	return &tview->view;
}

/* Copyright (C) 2003 Timo Sirainen */

#include "lib.h"
#include "istream.h"
#include "buffer.h"
#include "str.h"
#include "message-date.h"
#include "message-parser.h"
#include "istream-header-filter.h"
#include "imap-envelope.h"
#include "imap-bodystructure.h"
#include "index-storage.h"
#include "index-mail.h"

#include <stdlib.h>

struct index_header_lookup_ctx {
	struct mailbox_header_lookup_ctx ctx;
	pool_t pool;

	size_t count;
	unsigned int *idx;
	const char **name;
};

static int header_line_cmp(const void *p1, const void *p2)
{
	const struct index_mail_line *l1 = p1, *l2 = p2;
	int diff;

	diff = (int)l1->field_idx - (int)l2->field_idx;
	return diff != 0 ? diff :
		(int)l1->line_num - (int)l2->line_num;
}

static void index_mail_parse_header_finish(struct index_mail *mail)
{
	static uint32_t null = 0;
	struct index_mail_line *lines;
	const unsigned char *header, *data;
	const uint8_t *match;
	buffer_t *buf;
	size_t i, j, size, data_size, match_idx, match_size;
	int noncontiguous;

	t_push();

	lines = buffer_get_modifyable_data(mail->header_lines, &size);
	size /= sizeof(*lines);

	/* sort it first so fields are grouped together and ordered by
	   line number */
	qsort(lines, size, sizeof(*lines), header_line_cmp);

	match = buffer_get_data(mail->header_match, &match_size);
	header = buffer_get_data(mail->header_data, NULL);
	buf = buffer_create_dynamic(pool_datastack_create(), 256, (size_t)-1);

	for (i = match_idx = 0; i < size; i = j) {
		while (match_idx < lines[i].field_idx &&
		       match_idx < match_size) {
			if (match[match_idx] == mail->header_match_value) {
				/* this header doesn't exist. remember that. */
				mail_cache_add(mail->trans->cache_trans,
					       mail->data.seq, match_idx,
					       NULL, 0);
			}
			match_idx++;
		}
		match_idx++;

		buffer_set_used_size(buf, 0);
		buffer_append(buf, &lines[i].line_num,
			      sizeof(lines[i].line_num));

		noncontiguous = FALSE;
		for (j = i+1; j < size; j++) {
			if (lines[j].field_idx != lines[i].field_idx)
				break;

			if (lines[j].start_pos != lines[j-1].end_pos)
				noncontiguous = TRUE;
			buffer_append(buf, &lines[j].line_num,
				      sizeof(lines[j].line_num));
		}
		buffer_append(buf, &null, sizeof(uint32_t));

		if (noncontiguous) {
			for (; i < j; i++) {
				buffer_append(buf, header + lines[i].start_pos,
					      lines[i].end_pos -
					      lines[i].start_pos);
			}
		} else {
			buffer_append(buf, header + lines[i].start_pos,
				      lines[j-1].end_pos - lines[i].start_pos);
		}

		data = buffer_get_data(buf, &data_size);
		mail_cache_add(mail->trans->cache_trans, mail->data.seq,
			       lines[i].field_idx, data, data_size);
	}

	t_pop();
}

void index_mail_parse_header_init(struct index_mail *mail,
				  struct mailbox_header_lookup_ctx *_headers)
{
	struct index_header_lookup_ctx *headers =
		(struct index_header_lookup_ctx *)_headers;
	size_t i;

	if (mail->header_data == NULL) {
		mail->header_data =
			buffer_create_dynamic(default_pool, 4096, (size_t)-1);
		mail->header_lines =
			buffer_create_dynamic(default_pool, 256, (size_t)-1);
		mail->header_match =
			buffer_create_dynamic(default_pool, 64, (size_t)-1);
	} else {
		buffer_set_used_size(mail->header_data, 0);
		buffer_set_used_size(mail->header_lines, 0);
	}

	if (++mail->header_match_value == 0) {
		/* wrapped, we'll have to clear the buffer */
		memset(buffer_get_modifyable_data(mail->header_match, NULL), 0,
		       buffer_get_size(mail->header_match));
		mail->header_match_value++;
	}

	if (headers != NULL) {
		for (i = 0; i < headers->count; i++) {
			buffer_write(mail->header_match, headers->idx[i],
				     &mail->header_match_value, 1);
		}
	}

	if (mail->wanted_headers != NULL && mail->wanted_headers != headers) {
		headers = mail->wanted_headers;
		for (i = 0; i < headers->count; i++) {
			buffer_write(mail->header_match, headers->idx[i],
				     &mail->header_match_value, 1);
		}
	}
}

static void index_mail_parse_finish_imap_envelope(struct index_mail *mail)
{
	string_t *str;

	str = str_new(mail->pool, 256);
	imap_envelope_write_part_data(mail->data.envelope_data, str);
	mail->data.envelope = str_c(str);

	mail_cache_add(mail->trans->cache_trans, mail->data.seq,
		       MAIL_CACHE_ENVELOPE, str_data(str), str_len(str));
}

int index_mail_parse_header(struct message_part *part,
			    struct message_header_line *hdr,
			    struct index_mail *mail)
{
	struct index_mail_data *data = &mail->data;
	enum mail_cache_decision_type decision;
	const char *cache_field_name;
	unsigned int field_idx;
	int timezone;

        data->parse_line_num++;

	if (data->save_bodystructure_header) {
		i_assert(part != NULL);
		imap_bodystructure_parse_header(mail->pool, part, hdr);
	}

	if (data->save_envelope) {
		imap_envelope_parse_header(mail->pool,
					   &data->envelope_data, hdr);

		if (hdr == NULL)
                        index_mail_parse_finish_imap_envelope(mail);
	}

	if (hdr == NULL) {
		/* end of headers */
		if (data->save_sent_date) {
			/* not found */
			data->sent_date.time = 0;
			data->sent_date.timezone = 0;
			data->save_sent_date = FALSE;
		}
		if (data->sent_date.time != (time_t)-1) {
                        mail_cache_add(mail->trans->cache_trans, data->seq,
				       MAIL_CACHE_SENT_DATE, &data->sent_date,
				       sizeof(data->sent_date));
		}
		index_mail_parse_header_finish(mail);
                data->save_bodystructure_header = FALSE;
		return TRUE;
	}

	if (data->save_sent_date && strcasecmp(hdr->name, "Date") == 0) {
		if (hdr->continues)
			hdr->use_full_value = TRUE;
		else {
			if (!message_date_parse(hdr->full_value,
						hdr->full_value_len,
						&data->sent_date.time,
						&timezone)) {
				/* 0 == parse error */
				data->sent_date.time = 0;
				timezone = 0;
			}
                        data->sent_date.timezone = timezone;
			data->save_sent_date = FALSE;
		}
	}

	if (!hdr->continued) {
		t_push();
		cache_field_name = t_strconcat("hdr.", hdr->name, NULL);
		data->parse_line.field_idx =
			mail_cache_register_lookup(mail->ibox->cache,
						   cache_field_name);
		t_pop();
	}
	field_idx = data->parse_line.field_idx;

	if (field_idx == (unsigned int)-1) {
		/* we don't want this field */
		return TRUE;
	}

	if (!hdr->continued) {
		decision = mail_cache_field_get_decision(mail->ibox->cache,
							 field_idx);
		data->parse_line.cache =
			(decision & ~MAIL_CACHE_DECISION_FORCED) !=
			MAIL_CACHE_DECISION_NO;
		if (data->parse_line.cache &&
		    mail_cache_field_exists(mail->trans->cache_view,
					    data->seq, field_idx) > 0) {
			/* already cached */
			data->parse_line.cache = FALSE;
		}
	}

	if (!data->parse_line.cache) {
		const uint8_t *match;
		size_t size;

		match = buffer_get_data(mail->header_match, &size);
		if (field_idx >= size ||
		    match[field_idx] != mail->header_match_value) {
			/* we don't need to do anything with this header */
			return TRUE;
		}
	}

	if (!hdr->continued) {
		data->parse_line.start_pos = str_len(mail->header_data);
		data->parse_line.line_num = data->parse_line_num;
		str_append(mail->header_data, hdr->name);
		str_append(mail->header_data, ": ");
	}
	str_append_n(mail->header_data, hdr->value, hdr->value_len);
	if (!hdr->no_newline)
		str_append(mail->header_data, "\n");
	if (!hdr->continues) {
		data->parse_line.end_pos = str_len(mail->header_data);
		buffer_append(mail->header_lines, &data->parse_line,
			      sizeof(data->parse_line));
	}
	return TRUE;
}

static void
index_mail_parse_header_cb(struct message_part *part,
			   struct message_header_line *hdr, void *context)
{
	struct index_mail *mail = context;

	(void)index_mail_parse_header(part, hdr, mail);
}

int index_mail_parse_headers(struct index_mail *mail)
{
	struct index_mail_data *data = &mail->data;

	if (data->stream == NULL) {
		if (mail->mail.get_stream(&mail->mail, NULL, NULL) == NULL)
			return FALSE;
	}

	index_mail_parse_header_init(mail, NULL);

	if (data->parts == NULL && data->parser_ctx == NULL) {
		/* initialize bodystructure parsing in case we read the whole
		   message. */
		data->parser_ctx =
			message_parser_init(mail->pool, data->stream);
		message_parser_parse_header(data->parser_ctx, &data->hdr_size,
					    index_mail_parse_header_cb, mail);
	} else {
		/* just read the header */
		message_parse_header(data->parts, data->stream, &data->hdr_size,
				     index_mail_parse_header_cb, mail);
	}
	data->hdr_size_set = TRUE;
	data->parse_header = FALSE;

	return TRUE;
}

static void
imap_envelope_parse_callback(struct message_part *part __attr_unused__,
			     struct message_header_line *hdr, void *context)
{
	struct index_mail *mail = context;

	imap_envelope_parse_header(mail->pool, &mail->data.envelope_data, hdr);

	if (hdr == NULL)
		index_mail_parse_finish_imap_envelope(mail);
}

void index_mail_headers_get_envelope(struct index_mail *mail)
{
	struct mailbox_header_lookup_ctx *header_ctx;
	struct istream *stream;

	header_ctx = mailbox_header_lookup_init(&mail->ibox->box,
						imap_envelope_headers);
	stream = mail->mail.get_headers(&mail->mail, header_ctx);
	if (mail->data.envelope == NULL) {
		/* we got the headers from cache - parse them to get the
		   envelope */
		message_parse_header(NULL, stream, NULL,
				     imap_envelope_parse_callback, mail);
		mail->data.save_envelope = FALSE;
	}
	mailbox_header_lookup_deinit(header_ctx);
}

static unsigned int
get_header_field_idx(struct index_mailbox *ibox, const char *field)
{
	struct mail_cache_field header_field = {
		NULL, 0, MAIL_CACHE_FIELD_HEADER, 0,
		MAIL_CACHE_DECISION_TEMP, 0, 0
	};
	const char *cache_field_name;
	unsigned int field_idx;

	t_push();
	cache_field_name = t_strconcat("hdr.", field, NULL);
	field_idx = mail_cache_register_lookup(ibox->cache, cache_field_name);
	if (field_idx == (unsigned int)-1) {
		header_field.name = cache_field_name;
		mail_cache_register_fields(ibox->cache, &header_field, 1);
		field_idx = header_field.idx;
	}
	t_pop();
	return field_idx;
}

const char *index_mail_get_header(struct mail *_mail, const char *field)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	const unsigned char *data;
	unsigned int field_idx;
	string_t *dest;
	size_t i, len, value_pos;
	int ret;

	field_idx = get_header_field_idx(mail->ibox, field);

	dest = str_new(mail->pool, 128);
	if (mail_cache_lookup_headers(mail->trans->cache_view, dest,
				      mail->data.seq, &field_idx, 1) <= 0) {
		/* not in cache / error */
		if (index_mail_parse_headers(mail) < 0)
			return NULL;

		ret = mail_cache_lookup_headers(mail->trans->cache_view, dest,
						mail->data.seq, &field_idx, 1);
		i_assert(ret != 0);
		if (ret < 0)
			return NULL;
	}

	/* cached. skip "header name: " in dest. */
	data = str_data(dest);
	len = str_len(dest);
	for (i = 0; i < len; i++) {
		if (data[i] == ':') {
			if (i+1 != len && data[++i] == ' ') i++;
			break;
		}
	}
	value_pos = i;

	/* return only the first field in case there's multiple. */
	for (; i < len; i++) {
		if (data[i] == '\n') {
			if (i+1 == len ||
			    (data[i+1] != ' ' && data[i+1] != '\t')) {
				buffer_set_used_size(dest, i);
				break;
			}
		}
	}

	return str_c(dest) + value_pos;
}

static void header_cache_callback(struct message_header_line *hdr,
				  int matched __attr_unused__, void *context)
{
	struct index_mail *mail = context;

	(void)index_mail_parse_header(NULL, hdr, mail);
}

struct istream *
index_mail_get_headers(struct mail *_mail,
		       struct mailbox_header_lookup_ctx *_headers)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_header_lookup_ctx *headers =
		(struct index_header_lookup_ctx *)_headers;
	string_t *dest;

	dest = str_new(mail->pool, 256);
	if (mail_cache_lookup_headers(mail->trans->cache_view, dest,
				      mail->data.seq, headers->idx,
				      headers->count) > 0) {
		return i_stream_create_from_data(mail->pool,
						 str_data(dest), str_len(dest));
	}
	/* not in cache / error */
	p_free(mail->pool, dest);

	if (mail->data.stream == NULL) {
		if (mail->mail.get_stream(&mail->mail, NULL, NULL) == NULL)
			return FALSE;
	}

	if (mail->data.filter_stream != NULL)
		i_stream_unref(mail->data.filter_stream);

	index_mail_parse_header_init(mail, _headers);
	mail->data.filter_stream =
		i_stream_create_header_filter(mail->pool, mail->data.stream,
					      FALSE,
					      headers->name, headers->count,
					      header_cache_callback, mail);
	return mail->data.filter_stream;
}

static int strcasecmp_p(const void *p1, const void *p2)
{
	const char *const *s1 = p1, *const *s2 = p2;

	return strcasecmp(*s1, *s2);
}

struct mailbox_header_lookup_ctx *
index_header_lookup_init(struct mailbox *box, const char *const headers[])
{
	struct index_mailbox *ibox = (struct index_mailbox *)box;
	struct mail_cache_field *fields, header_field = {
		NULL, 0, MAIL_CACHE_FIELD_HEADER, 0,
		MAIL_CACHE_DECISION_TEMP, 0, 0
	};
	struct index_header_lookup_ctx *ctx;
	const char *const *name;
	const char **sorted_headers;
	buffer_t *buf;
	pool_t pool;
	size_t i, size;

	for (size = 0, name = headers; *name != NULL; name++)
		size++;

	t_push();

	if (size > 0) {
		/* headers need to be sorted for filter stream. */
		sorted_headers = t_new(const char *, size);
		memcpy(sorted_headers, headers, size * sizeof(*sorted_headers));
		qsort(sorted_headers, size, sizeof(*sorted_headers),
		      strcasecmp_p);
		headers = sorted_headers;
	}

	buf = buffer_create_dynamic(pool_datastack_create(), 128, (size_t)-1);
	for (i = 0; i < size; i++) {
		header_field.name = t_strconcat("hdr.", headers[i], NULL);
		buffer_append(buf, &header_field, sizeof(header_field));
	}

	fields = buffer_get_modifyable_data(buf, &size);
	size /= sizeof(*fields);
	mail_cache_register_fields(ibox->cache, fields, size);

	pool = pool_alloconly_create("index_header_lookup_ctx", 256);
	ctx = p_new(pool, struct index_header_lookup_ctx, 1);
	ctx->ctx.box = box;
	ctx->pool = pool;
	ctx->count = size;

	if (size > 0) {
		ctx->idx = p_new(pool, unsigned int, size);
		ctx->name = p_new(pool, const char *, size);

		/* @UNSAFE */
		for (i = 0; i < size; i++) {
			ctx->idx[i] = fields[i].idx;
			ctx->name[i] = p_strdup(pool, sorted_headers[i]);
		}
	}

	t_pop();
	return &ctx->ctx;
}

void index_header_lookup_deinit(struct mailbox_header_lookup_ctx *_ctx)
{
	struct index_header_lookup_ctx *ctx =
		(struct index_header_lookup_ctx *)_ctx;

	pool_unref(ctx->pool);
}

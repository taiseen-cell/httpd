/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000-2001 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 *
 * Portions of this software are based upon public domain software
 * originally written at the National Center for Supercomputing Applications,
 * University of Illinois, Urbana-Champaign.
 */

/*
 * http_include.c: Handles the server-parsed HTML documents
 * 
 * Original by Rob McCool; substantial fixups by David Robinson;
 * incorporated into the Apache module framework by rst.
 * 
 */

#include "apr.h"
#include "apr_strings.h"
#include "apr_thread_proc.h"
#include "apr_hash.h"
#include "apr_user.h"
#include "apr_lib.h"
#include "apr_optional.h"

#define APR_WANT_STRFUNC
#include "apr_want.h"

#define CORE_PRIVATE

#include "ap_config.h"
#include "util_filter.h"
#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_request.h"
#include "http_core.h"
#include "http_protocol.h"
#include "http_log.h"
#include "http_main.h"
#include "util_script.h"
#include "http_core.h"
#include "mod_include.h"
#include "util_ebcdic.h"

module AP_MODULE_DECLARE_DATA include_module;
static apr_hash_t *include_hash;
static APR_OPTIONAL_FN_TYPE(ap_register_include_handler) *ssi_pfn_register;

#define BYTE_COUNT_THRESHOLD AP_MIN_BYTES_TO_WRITE

/* ------------------------ Environment function -------------------------- */

/* XXX: could use ap_table_overlap here */
static void add_include_vars(request_rec *r, char *timefmt)
{
    char *pwname;
    apr_table_t *e = r->subprocess_env;
    char *t;
    apr_time_t date = r->request_time;

    apr_table_setn(e, "DATE_LOCAL", ap_ht_time(r->pool, date, timefmt, 0));
    apr_table_setn(e, "DATE_GMT", ap_ht_time(r->pool, date, timefmt, 1));
    apr_table_setn(e, "LAST_MODIFIED",
              ap_ht_time(r->pool, r->finfo.mtime, timefmt, 0));
    apr_table_setn(e, "DOCUMENT_URI", r->uri);
    apr_table_setn(e, "DOCUMENT_PATH_INFO", r->path_info);
    if (apr_get_username(&pwname, r->finfo.user, r->pool) == APR_SUCCESS) {
        apr_table_setn(e, "USER_NAME", pwname);
    }
    else {
        apr_table_setn(e, "USER_NAME", "<unknown>");
    }
    if ((t = strrchr(r->filename, '/'))) {
        apr_table_setn(e, "DOCUMENT_NAME", ++t);
    }
    else {
        apr_table_setn(e, "DOCUMENT_NAME", r->uri);
    }
    if (r->args) {
        char *arg_copy = apr_pstrdup(r->pool, r->args);

        ap_unescape_url(arg_copy);
        apr_table_setn(e, "QUERY_STRING_UNESCAPED",
                  ap_escape_shell_cmd(r->pool, arg_copy));
    }
}



/* --------------------------- Parser functions --------------------------- */

/* This function returns either a pointer to the split bucket containing the
 * first byte of the BEGINNING_SEQUENCE (after finding a complete match) or it
 * returns NULL if no match found.
 */
static apr_bucket *find_start_sequence(apr_bucket *dptr, include_ctx_t *ctx,
                                      apr_bucket_brigade *bb, int *do_cleanup)
{
    apr_size_t len;
    const char *c;
    const char *buf;
    const char *str = STARTING_SEQUENCE;
    apr_bucket *tmp_bkt;
    apr_size_t  start_index;

    *do_cleanup = 0;

    do {
        if (APR_BUCKET_IS_EOS(dptr)) {
            break;
        }
        apr_bucket_read(dptr, &buf, &len, APR_BLOCK_READ);
        /* XXX handle retcodes */
        if (len == 0) { /* end of pipe? */
            break;
        }
        c = buf;
        while (c - buf != len) {
            if (ctx->bytes_parsed >= BYTE_COUNT_THRESHOLD) {
                apr_bucket *start_bucket;

                if (ctx->head_start_index > 0) {
                    start_index  = ctx->head_start_index;
                    start_bucket = ctx->head_start_bucket;
                }
                else {
                    start_index  = (c - buf);
                    start_bucket = dptr;
                }
                apr_bucket_split(start_bucket, start_index);
                tmp_bkt = APR_BUCKET_NEXT(start_bucket);
                if (ctx->head_start_index > 0) {
                    ctx->head_start_index  = 0;
                    ctx->head_start_bucket = tmp_bkt;
                }

                return tmp_bkt;
            }

            if (*c == str[ctx->parse_pos]) {
                if (ctx->state == PRE_HEAD) {
                    ctx->state             = PARSE_HEAD;
                    ctx->head_start_bucket = dptr;
                    ctx->head_start_index  = c - buf;
                }
                ctx->parse_pos++;
            }
            else {
                if (str[ctx->parse_pos] == '\0') {
                    /* We want to split the bucket at the '<'. */
                    ctx->bytes_parsed++;
                    ctx->state            = PARSE_DIRECTIVE;
                    ctx->tag_length       = 0;
                    ctx->parse_pos        = 0;
                    ctx->tag_start_bucket = dptr;
                    ctx->tag_start_index  = c - buf;
                    if (ctx->head_start_index > 0) {
                        start_index = (c - buf) - ctx->head_start_index;
                        apr_bucket_split(ctx->head_start_bucket, ctx->head_start_index);
                        tmp_bkt = APR_BUCKET_NEXT(ctx->head_start_bucket);
                        if (dptr == ctx->head_start_bucket) {
                            ctx->tag_start_bucket = tmp_bkt;
                            ctx->tag_start_index  = start_index;
                        }
                        ctx->head_start_bucket = tmp_bkt;
                        ctx->head_start_index  = 0;
                    }
                    return ctx->head_start_bucket;
                }
                else if (ctx->parse_pos != 0) {
                    /* The reason for this, is that we need to make sure 
                     * that we catch cases like <<!--#.  This makes the 
                     * second check after the original check fails.
                     * If parse_pos was already 0 then we already checked this.
                     */
                    *do_cleanup = 1;
                    if (*c == str[0]) {
                        ctx->parse_pos         = 1;
                        ctx->state             = PARSE_HEAD;
                        ctx->head_start_bucket = dptr;
                        ctx->head_start_index  = c - buf;
                    }
                    else {
                        ctx->parse_pos         = 0;
                        ctx->state             = PRE_HEAD;
                        ctx->head_start_bucket = NULL;
                        ctx->head_start_index  = 0;
                    }
                }
            }
            c++;
            ctx->bytes_parsed++;
        }
        dptr = APR_BUCKET_NEXT(dptr);
    } while (dptr != APR_BRIGADE_SENTINEL(bb));
    return NULL;
}

static apr_bucket *find_end_sequence(apr_bucket *dptr, include_ctx_t *ctx, apr_bucket_brigade *bb)
{
    apr_size_t len;
    const char *c;
    const char *buf;
    const char *str = ENDING_SEQUENCE;

    do {
        if (APR_BUCKET_IS_EOS(dptr)) {
            break;
        }
        apr_bucket_read(dptr, &buf, &len, APR_BLOCK_READ);
        /* XXX handle retcodes */
        if (len == 0) { /* end of pipe? */
            break;
        }
        if (dptr == ctx->tag_start_bucket) {
            c = buf + ctx->tag_start_index;
        }
        else {
            c = buf;
        }
        while (c - buf != len) {
            if (ctx->bytes_parsed >= BYTE_COUNT_THRESHOLD) {
                return dptr;
            }

            if (*c == str[ctx->parse_pos]) {
                if (ctx->state != PARSE_TAIL) {
                    ctx->state             = PARSE_TAIL;
                    ctx->tail_start_bucket = dptr;
                    ctx->tail_start_index  = c - buf;
                }
                ctx->parse_pos++;
            }
            else {
                if (ctx->state == PARSE_DIRECTIVE) {
                    if (ctx->tag_length == 0) {
                        if (!apr_isspace(*c)) {
                            ctx->tag_start_bucket = dptr;
                            ctx->tag_start_index  = c - buf;
                            ctx->tag_length       = 1;
                            ctx->directive_length = 1;
                        }
                    }
                    else {
                        if (!apr_isspace(*c)) {
                            ctx->directive_length++;
                        }
                        else {
                            ctx->state = PARSE_TAG;
                        }
                        ctx->tag_length++;
                    }
                }
                else if (ctx->state == PARSE_TAG) {
                    ctx->tag_length++;
                }
                else {
                    if (str[ctx->parse_pos] == '\0') {
                        apr_bucket *tmp_buck = dptr;

                        /* We want to split the bucket at the '>'. The
                         * end of the END_SEQUENCE is in the current bucket.
                         * The beginning might be in a previous bucket.
                         */
                        ctx->bytes_parsed++;
                        ctx->state = PARSED;
                        if ((c - buf) > 0) {
                            apr_bucket_split(dptr, c - buf);
                            tmp_buck = APR_BUCKET_NEXT(dptr);
                        }
                        return (tmp_buck);
                    }
                    else if (ctx->parse_pos != 0) {
                        /* The reason for this, is that we need to make sure 
                         * that we catch cases like --->.  This makes the 
                         * second check after the original check fails.
                         * If parse_pos was already 0 then we already checked this.
                         */
                         ctx->tag_length += ctx->parse_pos;

                         if (*c == str[0]) {
                             ctx->state             = PARSE_TAIL;
                             ctx->tail_start_bucket = dptr;
                             ctx->tail_start_index  = c - buf;
                             ctx->tag_length       += ctx->parse_pos;
                             ctx->parse_pos         = 1;
                         }
                         else {
                             if (ctx->tag_length > ctx->directive_length) {
                                 ctx->state = PARSE_TAG;
                             }
                             else {
                                 ctx->state = PARSE_DIRECTIVE;
                                 ctx->directive_length += ctx->parse_pos;
                             }
                             ctx->tail_start_bucket = NULL;
                             ctx->tail_start_index  = 0;
                             ctx->tag_length       += ctx->parse_pos;
                             ctx->parse_pos         = 0;
                         }
                    }
                }
            }
            c++;
            ctx->bytes_parsed++;
        }
        dptr = APR_BUCKET_NEXT(dptr);
    } while (dptr != APR_BRIGADE_SENTINEL(bb));
    return NULL;
}

/* This function culls through the buckets that have been set aside in the 
 * ssi_tag_brigade and copies just the directive part of the SSI tag (none
 * of the start and end delimiter bytes are copied).
 */
static apr_status_t get_combined_directive (include_ctx_t *ctx,
                                            request_rec *r,
                                            apr_bucket_brigade *bb,
                                            char *tmp_buf, int tmp_buf_size)
{
    int         done = 0;
    apr_bucket  *dptr;
    const char *tmp_from;
    apr_size_t tmp_from_len;

    /* If the tag length is longer than the tmp buffer, allocate space. */
    if (ctx->tag_length > tmp_buf_size-1) {
        if ((ctx->combined_tag = apr_pcalloc(r->pool, ctx->tag_length + 1)) == NULL) {
            return (APR_ENOMEM);
        }
    }     /* Else, just use the temp buffer. */
    else {
        ctx->combined_tag = tmp_buf;
    }

    /* Prime the pump. Start at the beginning of the tag... */
    dptr = ctx->tag_start_bucket;
    apr_bucket_read (dptr, &tmp_from, &tmp_from_len, 0);  /* Read the bucket... */

    /* Adjust the pointer to start at the tag within the bucket... */
    if (dptr == ctx->tail_start_bucket) {
        tmp_from_len -= (tmp_from_len - ctx->tail_start_index);
    }
    tmp_from          = &tmp_from[ctx->tag_start_index];
    tmp_from_len     -= ctx->tag_start_index;
    ctx->curr_tag_pos = ctx->combined_tag;

    /* Loop through the buckets from the tag_start_bucket until before
     * the tail_start_bucket copying the contents into the buffer.
     */
    do {
        memcpy (ctx->curr_tag_pos, tmp_from, tmp_from_len);
        ctx->curr_tag_pos += tmp_from_len;

        if (dptr == ctx->tail_start_bucket) {
            done = 1;
        }
        else {
            dptr = APR_BUCKET_NEXT (dptr);
            apr_bucket_read (dptr, &tmp_from, &tmp_from_len, 0);
            /* Adjust the count to stop at the beginning of the tail. */
            if (dptr == ctx->tail_start_bucket) {
                tmp_from_len -= (tmp_from_len - ctx->tail_start_index);
            }
        }
    } while ((!done) &&
             ((ctx->curr_tag_pos - ctx->combined_tag) < ctx->tag_length));

    ctx->combined_tag[ctx->tag_length] = '\0';
    ctx->curr_tag_pos = ctx->combined_tag;

    return (APR_SUCCESS);
}

/*
 * decodes a string containing html entities or numeric character references.
 * 's' is overwritten with the decoded string.
 * If 's' is syntatically incorrect, then the followed fixups will be made:
 *   unknown entities will be left undecoded;
 *   references to unused numeric characters will be deleted.
 *   In particular, &#00; will not be decoded, but will be deleted.
 *
 * drtr
 */

/* maximum length of any ISO-LATIN-1 HTML entity name. */
#define MAXENTLEN (6)

/* The following is a shrinking transformation, therefore safe. */

static void decodehtml(char *s)
{
    int val, i, j;
    char *p = s;
    const char *ents;
    static const char * const entlist[MAXENTLEN + 1] =
    {
        NULL,                   /* 0 */
        NULL,                   /* 1 */
        "lt\074gt\076",         /* 2 */
        "amp\046ETH\320eth\360",        /* 3 */
        "quot\042Auml\304Euml\313Iuml\317Ouml\326Uuml\334auml\344euml\353\
iuml\357ouml\366uuml\374yuml\377",      /* 4 */
        "Acirc\302Aring\305AElig\306Ecirc\312Icirc\316Ocirc\324Ucirc\333\
THORN\336szlig\337acirc\342aring\345aelig\346ecirc\352icirc\356ocirc\364\
ucirc\373thorn\376",            /* 5 */
        "Agrave\300Aacute\301Atilde\303Ccedil\307Egrave\310Eacute\311\
Igrave\314Iacute\315Ntilde\321Ograve\322Oacute\323Otilde\325Oslash\330\
Ugrave\331Uacute\332Yacute\335agrave\340aacute\341atilde\343ccedil\347\
egrave\350eacute\351igrave\354iacute\355ntilde\361ograve\362oacute\363\
otilde\365oslash\370ugrave\371uacute\372yacute\375"     /* 6 */
    };

    for (; *s != '\0'; s++, p++) {
        if (*s != '&') {
            *p = *s;
            continue;
        }
        /* find end of entity */
        for (i = 1; s[i] != ';' && s[i] != '\0'; i++) {
            continue;
        }

        if (s[i] == '\0') {     /* treat as normal data */
            *p = *s;
            continue;
        }

        /* is it numeric ? */
        if (s[1] == '#') {
            for (j = 2, val = 0; j < i && apr_isdigit(s[j]); j++) {
                val = val * 10 + s[j] - '0';
            }
            s += i;
            if (j < i || val <= 8 || (val >= 11 && val <= 31) ||
                (val >= 127 && val <= 160) || val >= 256) {
                p--;            /* no data to output */
            }
            else {
                *p = RAW_ASCII_CHAR(val);
            }
        }
        else {
            j = i - 1;
            if (j > MAXENTLEN || entlist[j] == NULL) {
                /* wrong length */
                *p = '&';
                continue;       /* skip it */
            }
            for (ents = entlist[j]; *ents != '\0'; ents += i) {
                if (strncmp(s + 1, ents, j) == 0) {
                    break;
                }
            }

            if (*ents == '\0') {
                *p = '&';       /* unknown */
            }
            else {
                *p = RAW_ASCII_CHAR(((const unsigned char *) ents)[j]);
                s += i;
            }
        }
    }

    *p = '\0';
}

/*
 * Extract the next tag name and value.
 * If there are no more tags, set the tag name to NULL.
 * The tag value is html decoded if dodecode is non-zero.
 * The tag value may be NULL if there is no tag value..
 *    format:
 *        [WS]<Tag>[WS]=[WS]['|"]<Value>['|"|WS]
 */

#define SKIP_TAG_WHITESPACE(ptr) while ((*ptr != '\0') && (apr_isspace (*ptr))) ptr++

static void ap_ssi_get_tag_and_value(include_ctx_t *ctx, char **tag,
                              char **tag_val, int dodecode)
{
    char *c = ctx->curr_tag_pos;
    int   shift_val = 0; 
    char  term = '\0';

    *tag_val = NULL;
    SKIP_TAG_WHITESPACE(c);
    *tag = c;             /* First non-whitespace character (could be NULL). */

    while ((*c != '\0') && (*c != '=') && (!apr_isspace(*c))) {
        *c = apr_tolower(*c);    /* find end of tag, lowercasing as we go... */
        c++;
    }

    if ((*c == '\0') || (**tag == '=')) {
        if ((**tag == '\0') || (**tag == '=')) {
            *tag = NULL;
        }
        ctx->curr_tag_pos = c;
        return;      /* We have found the end of the buffer. */
    }                /* We might have a tag, but definitely no value. */

    if (*c == '=') {
        *c++ = '\0';     /* Overwrite the '=' with a terminating byte after tag. */
    }
    else {               /* Try skipping WS to find the '='. */
        *c++ = '\0';     /* Terminate the tag... */
        SKIP_TAG_WHITESPACE(c);
        
        if (*c != '=') {     /* There needs to be an equal sign if there's a value. */
            ctx->curr_tag_pos = c;
            return;       /* There apparently was no value. */
        }
        else {
            c++; /* Skip the equals sign. */
        }
    }

    SKIP_TAG_WHITESPACE(c);
    if (*c == '"' || *c == '\'') {    /* Allow quoted values for space inclusion. */
        term = *c++;     /* NOTE: This does not pass the quotes on return. */
    }
    
    *tag_val = c;
    while ((*c != '\0') &&
           (((term != '\0') && (*c != term)) ||
            ((term == '\0') && (!apr_isspace(*c))))) {
        if (*c == '\\') {  /* Accept \" and \' as valid char in string. */
            c++;
            if (*c == term) { /* Overwrite the "\" during the embedded  */
                shift_val++;  /* escape sequence of '\"' or "\'". Shift */
            }                 /* bytes from here to next delimiter.     */
            if (shift_val > 0) {
                *(c-shift_val) = *c;
            }
        }

        c++;
        if (shift_val > 0) {
            *(c-shift_val) = *c;
        }
    }
    
    *c++ = '\0'; /* Overwrites delimiter (term or WS) with NULL. */
    ctx->curr_tag_pos = c;
    if (dodecode) {
        decodehtml(*tag_val);
    }

    return;
}


/*
 * Do variable substitution on strings
 */
static void ap_ssi_parse_string(request_rec *r, const char *in, char *out,
                         size_t length, int leave_name)
{
    char ch;
    char *next = out;
    char *end_out;

    /* leave room for nul terminator */
    end_out = out + length - 1;

    while ((ch = *in++) != '\0') {
        switch (ch) {
        case '\\':
	    if (next == end_out) {
		/* truncated */
		*next = '\0';
		return;
	    }
            if (*in == '$') {
                *next++ = *in++;
            }
            else {
                *next++ = ch;
            }
            break;
        case '$':
            {
		const char *start_of_var_name;
		char *end_of_var_name;	/* end of var name + 1 */
		const char *expansion, *temp_end, *val;
                char        tmp_store;
		size_t l;

		/* guess that the expansion won't happen */
		expansion = in - 1;
		if (*in == '{') {
		    ++in;
		    start_of_var_name = in;
		    in = ap_strchr_c(in, '}');
		    if (in == NULL) {
                        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR,
				    0, r, "Missing '}' on variable \"%s\"",
				    expansion);
                        *next = '\0';
                        return;
                    }
		    temp_end = in;
                    end_of_var_name = (char *)temp_end;
		    ++in;
		}
		else {
		    start_of_var_name = in;
		    while (apr_isalnum(*in) || *in == '_') {
			++in;
		    }
                    temp_end = in;
		    end_of_var_name = (char *)temp_end;
		}
		/* what a pain, too bad there's no table_getn where you can
		 * pass a non-nul terminated string */
		l = end_of_var_name - start_of_var_name;
		if (l != 0) {
                    tmp_store        = *end_of_var_name;
                    *end_of_var_name = '\0';
                    val = apr_table_get(r->subprocess_env, start_of_var_name);
                    *end_of_var_name = tmp_store;

		    if (val) {
			expansion = val;
			l = strlen(expansion);
		    }
		    else if (leave_name) {
			l = in - expansion;
		    }
		    else {
			break;	/* no expansion to be done */
		    }
		}
		else {
		    /* zero-length variable name causes just the $ to be copied */
		    l = 1;
		}
		l = ((int)l > end_out - next) ? (end_out - next) : l;
		memcpy(next, expansion, l);
		next += l;
                break;
            }
        default:
	    if (next == end_out) {
		/* truncated */
		*next = '\0';
		return;
	    }
            *next++ = ch;
            break;
        }
    }
    *next = '\0';
    return;
}

/* --------------------------- Action handlers ---------------------------- */

/* ensure that path is relative, and does not contain ".." elements
 * ensentially ensure that it does not match the regex:
 * (^/|(^|/)\.\.(/|$))
 * XXX: Needs to become apr_is_path_relative() test
 */
static int is_only_below(const char *path)
{
#ifdef HAVE_DRIVE_LETTERS
    if (path[1] == ':') 
	return 0;
#endif
#ifdef NETWARE
    if (strchr(path, ':')
	return 0;
#endif
    if (path[0] == '/') {
	return 0;
    }
    while (*path) {
        int dots = 0;
        while (path[dots] == '.')
            ++dots;
#if defined(WIN32) 
        /* If the name is canonical this is redundant
         * but in security, redundancy is worthwhile.
         * Does OS2 belong here (accepts ... for ..)?
         */
        if (dots > 1 && (!path[dots] || path[dots] == '/'))
            return 0;
#else
        if (dots == 2 && (!path[dots] || path[dots] == '/'))
            return 0;
#endif
        path += dots;
        while (*path && *(path++) != '/')
            ++path;
    }
    return 1;
}

static int handle_include(include_ctx_t *ctx, apr_bucket_brigade **bb, request_rec *r,
                          ap_filter_t *f, apr_bucket *head_ptr, apr_bucket **inserted_head)
{
    char *tag     = NULL;
    char *tag_val = NULL;
    apr_bucket  *tmp_buck;
    char parsed_string[MAX_STRING_LEN];

    *inserted_head = NULL;
    if (ctx->flags & FLAG_PRINTING) {
        while (1) {
            ap_ssi_get_tag_and_value(ctx, &tag, &tag_val, 1);
            if (tag_val == NULL) {
                if (tag == NULL) {
                    return (0);
                }
                else {
                    return (1);
                }
            }
            if (!strcmp(tag, "file") || !strcmp(tag, "virtual")) {
                request_rec *rr = NULL;
                char *error_fmt = NULL;

                ap_ssi_parse_string(r, tag_val, parsed_string, sizeof(parsed_string), 0);
                if (tag[0] == 'f') {
                    /* be safe; only files in this directory or below allowed */
    		if (!is_only_below(parsed_string)) {
                        error_fmt = "unable to include file \"%s\" "
                                    "in parsed file %s";
                    }
                    else {
                        rr = ap_sub_req_lookup_file(parsed_string, r, f->next);
                    }
                }
                else {
                    rr = ap_sub_req_lookup_uri(parsed_string, r, f->next);
                }

                if (!error_fmt && rr->status != HTTP_OK) {
                    error_fmt = "unable to include \"%s\" in parsed file %s";
                }

                if (!error_fmt && (ctx->flags & FLAG_NO_EXEC) && rr->content_type
                    && (strncmp(rr->content_type, "text/", 5))) {
                    error_fmt = "unable to include potential exec \"%s\" "
                        "in parsed file %s";
                }
                if (error_fmt == NULL) {
    		/* try to avoid recursive includes.  We do this by walking
    		 * up the r->main list of subrequests, and at each level
    		 * walking back through any internal redirects.  At each
    		 * step, we compare the filenames and the URIs.  
    		 *
    		 * The filename comparison catches a recursive include
    		 * with an ever-changing URL, eg.
    		 * <!--#include virtual=
    		 *      "$REQUEST_URI/$QUERY_STRING?$QUERY_STRING/x"-->
    		 * which, although they would eventually be caught because
    		 * we have a limit on the length of files, etc., can 
    		 * recurse for a while.
    		 *
    		 * The URI comparison catches the case where the filename
    		 * is changed while processing the request, so the 
    		 * current name is never the same as any previous one.
    		 * This can happen with "DocumentRoot /foo" when you
    		 * request "/" on the server and it includes "/".
    		 * This only applies to modules such as mod_dir that 
    		 * (somewhat improperly) mess with r->filename outside 
    		 * of a filename translation phase.
    		 */
    		int founddupe = 0;
                    request_rec *p;
                    for (p = r; p != NULL && !founddupe; p = p->main) {
    		    request_rec *q;
    		    for (q = p; q != NULL; q = q->prev) {
    			if ( (strcmp(q->filename, rr->filename) == 0) ||
    			     (strcmp(q->uri, rr->uri) == 0) ){
    			    founddupe = 1;
    			    break;
    			}
    		    }
    		}

                    if (p != NULL) {
                        error_fmt = "Recursive include of \"%s\" "
                            "in parsed file %s";
                    }
                }

    	    /* See the Kludge in send_parsed_file for why */
                /* Basically, it puts a bread crumb in here, then looks */
                /*   for the crumb later to see if its been here.       */
    	    if (rr) 
    		ap_set_module_config(rr->request_config, &include_module, r);

                if (!error_fmt) {
                    SPLIT_AND_PASS_PRETAG_BUCKETS(*bb, ctx, f->next);
                    
                    if (ap_run_sub_req(rr)) {
                        error_fmt = "unable to include \"%s\" in parsed file %s";
                    }
                }
                if (error_fmt) {
                    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR,
    			    0, r, error_fmt, tag_val, r->filename);
                    CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
                }

    	    /* destroy the sub request if it's not a nested include (crumb) */
                if (rr != NULL
    		&& ap_get_module_config(rr->request_config, &include_module)
    		    != NESTED_INCLUDE_MAGIC) {
    		ap_destroy_sub_req(rr);
                }
            }
            else {
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                            "unknown parameter \"%s\" to tag include in %s",
                            tag, r->filename);
                CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
            }
        }
    }
    return 0;
}


static int handle_echo(include_ctx_t *ctx, apr_bucket_brigade **bb, request_rec *r,
                       ap_filter_t *f, apr_bucket *head_ptr, apr_bucket **inserted_head)
{
    char       *tag       = NULL;
    char       *tag_val   = NULL;
    const char *echo_text = NULL;
    apr_bucket  *tmp_buck;
    apr_size_t e_len, e_wrt;
    enum {E_NONE, E_URL, E_ENTITY} encode;

    encode = E_ENTITY;

    *inserted_head = NULL;
    if (ctx->flags & FLAG_PRINTING) {
        while (1) {
            ap_ssi_get_tag_and_value(ctx, &tag, &tag_val, 1);
            if (tag_val == NULL) {
                if (tag != NULL) {
                    return 1;
                }
                else {
                    return 0;
                }
            }
            if (!strcmp(tag, "var")) {
                const char *val = apr_table_get(r->subprocess_env, tag_val);
                int b_copy = 0;

                if (val) {
                    switch(encode) {
                    case E_NONE:   echo_text = val;  b_copy = 1;             break;
                    case E_URL:    echo_text = ap_escape_uri(r->pool, val);  break;
                    case E_ENTITY: echo_text = ap_escape_html(r->pool, val); break;
            	}

                    e_len = strlen(echo_text);
                    tmp_buck = apr_bucket_heap_create(echo_text, e_len, 1, &e_wrt);
                }
                else {
                    tmp_buck = apr_bucket_immortal_create("(none)", sizeof("none"));
                }
                APR_BUCKET_INSERT_BEFORE(head_ptr, tmp_buck);
                if (*inserted_head == NULL) {
                    *inserted_head = tmp_buck;
                }
            }
            else if (!strcmp(tag, "encoding")) {
                if (!strcasecmp(tag_val, "none")) encode = E_NONE;
                else if (!strcasecmp(tag_val, "url")) encode = E_URL;
                else if (!strcasecmp(tag_val, "entity")) encode = E_ENTITY;
                else {
                    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                                  "unknown value \"%s\" to parameter \"encoding\" of "
                                  "tag echo in %s", tag_val, r->filename);
                    CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
                }
            }
            else {
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                            "unknown parameter \"%s\" in tag echo of %s",
                            tag, r->filename);
                CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
            }

        }
    }
    return 0;
}

/* error and tf must point to a string with room for at 
 * least MAX_STRING_LEN characters 
 */
static int handle_config(include_ctx_t *ctx, apr_bucket_brigade **bb, request_rec *r,
                         ap_filter_t *f, apr_bucket *head_ptr, apr_bucket **inserted_head)
{
    char *tag     = NULL;
    char *tag_val = NULL;
    char parsed_string[MAX_STRING_LEN];
    apr_table_t *env = r->subprocess_env;

    *inserted_head = NULL;
    if (ctx->flags & FLAG_PRINTING) {
        while (1) {
            ap_ssi_get_tag_and_value(ctx, &tag, &tag_val, 0);
            if (tag_val == NULL) {
                if (tag == NULL) {
                    return 0;  /* Reached the end of the string. */
                }
                else {
                    return 1;  /* tags must have values. */
                }
            }
            if (!strcmp(tag, "errmsg")) {
                ap_ssi_parse_string(r, tag_val, ctx->error_str, MAX_STRING_LEN, 0);
                ctx->error_length = strlen(ctx->error_str);
            }
            else if (!strcmp(tag, "timefmt")) {
                apr_time_t date = r->request_time;

                ap_ssi_parse_string(r, tag_val, ctx->time_str, MAX_STRING_LEN, 0);
                apr_table_setn(env, "DATE_LOCAL", ap_ht_time(r->pool, date, ctx->time_str, 0));
                apr_table_setn(env, "DATE_GMT", ap_ht_time(r->pool, date, ctx->time_str, 1));
                apr_table_setn(env, "LAST_MODIFIED",
                               ap_ht_time(r->pool, r->finfo.mtime, ctx->time_str, 0));
            }
            else if (!strcmp(tag, "sizefmt")) {
                ap_ssi_parse_string(r, tag_val, parsed_string, sizeof(parsed_string), 0);
                decodehtml(parsed_string);
                if (!strcmp(parsed_string, "bytes")) {
                    ctx->flags |= FLAG_SIZE_IN_BYTES;
                }
                else if (!strcmp(parsed_string, "abbrev")) {
                    ctx->flags &= FLAG_SIZE_ABBREV;
                }
            }
            else {
                apr_bucket  *tmp_buck;

                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                            "unknown parameter \"%s\" to tag config in %s",
                            tag, r->filename);
                CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
            }
        }
    }
    return 0;
}


static int find_file(request_rec *r, const char *directive, const char *tag,
                     char *tag_val, apr_finfo_t *finfo)
{
    char *to_send = tag_val;
    request_rec *rr = NULL;
    int ret=0;
    char *error_fmt = NULL;
    apr_status_t rv = APR_SUCCESS;

    if (!strcmp(tag, "file")) {
        /* be safe; only files in this directory or below allowed */
        if (!is_only_below(tag_val)) {
            error_fmt = "unable to access file \"%s\" "
                        "in parsed file %s";
        }
        else {
            ap_getparents(tag_val);    /* get rid of any nasties */

            /* note: it is okay to pass NULL for the "next filter" since
               we never attempt to "run" this sub request. */
            rr = ap_sub_req_lookup_file(tag_val, r, NULL);

            if (rr->status == HTTP_OK && rr->finfo.filetype != 0) {
                to_send = rr->filename;
                if ((rv = apr_stat(finfo, to_send, APR_FINFO_GPROT 
                                | APR_FINFO_MIN, rr->pool)) != APR_SUCCESS
                                                     && rv != APR_INCOMPLETE) {
                    error_fmt = "unable to get information about \"%s\" "
                        "in parsed file %s";
                }
            }
            else {
                error_fmt = "unable to lookup information about \"%s\" "
                            "in parsed file %s";
            }
        }

        if (error_fmt) {
            ret = -1;
            ap_log_rerror(APLOG_MARK, APLOG_ERR | (rv ? 0 : APLOG_NOERRNO),
                          rv, r, error_fmt, to_send, r->filename);
        }

        if (rr) ap_destroy_sub_req(rr);
        
        return ret;
    }
    else if (!strcmp(tag, "virtual")) {
        /* note: it is okay to pass NULL for the "next filter" since
           we never attempt to "run" this sub request. */
        rr = ap_sub_req_lookup_uri(tag_val, r, NULL);

        if (rr->status == HTTP_OK && rr->finfo.filetype != 0) {
            memcpy((char *) finfo, (const char *) &rr->finfo,
                   sizeof(rr->finfo));
            ap_destroy_sub_req(rr);
            return 0;
        }
        else {
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                        "unable to get information about \"%s\" "
                        "in parsed file %s",
                        tag_val, r->filename);
            ap_destroy_sub_req(rr);
            return -1;
        }
    }
    else {
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                    "unknown parameter \"%s\" to tag %s in %s",
                    tag, directive, r->filename);
        return -1;
    }
}

#define NEG_SIGN  "    -"
#define ZERO_K    "   0k"
#define ONE_K     "   1k"

static void generate_size(apr_ssize_t size, char *buff, apr_size_t buff_size)
{
    /* XXX: this -1 thing is a gross hack */
    if (size == (apr_ssize_t)-1) {
	memcpy (buff, NEG_SIGN, sizeof(NEG_SIGN)+1);
    }
    else if (!size) {
	memcpy (buff, ZERO_K, sizeof(ZERO_K)+1);
    }
    else if (size < 1024) {
	memcpy (buff, ONE_K, sizeof(ONE_K)+1);
    }
    else if (size < 1048576) {
        apr_snprintf(buff, buff_size, "%4" APR_SSIZE_T_FMT "k", (size + 512) / 1024);
    }
    else if (size < 103809024) {
        apr_snprintf(buff, buff_size, "%4.1fM", size / 1048576.0);
    }
    else {
        apr_snprintf(buff, buff_size, "%4" APR_SSIZE_T_FMT "M", (size + 524288) / 1048576);
    }
}

static int handle_fsize(include_ctx_t *ctx, apr_bucket_brigade **bb, request_rec *r,
                        ap_filter_t *f, apr_bucket *head_ptr, apr_bucket **inserted_head)
{
    char *tag     = NULL;
    char *tag_val = NULL;
    apr_finfo_t  finfo;
    apr_size_t  s_len, s_wrt;
    apr_bucket   *tmp_buck;
    char parsed_string[MAX_STRING_LEN];

    *inserted_head = NULL;
    if (ctx->flags & FLAG_PRINTING) {
        while (1) {
            ap_ssi_get_tag_and_value(ctx, &tag, &tag_val, 1);
            if (tag_val == NULL) {
                if (tag == NULL) {
                    return 0;
                }
                else {
                    return 1;
                }
            }
            else {
                ap_ssi_parse_string(r, tag_val, parsed_string, sizeof(parsed_string), 0);
                if (!find_file(r, "fsize", tag, parsed_string, &finfo)) {
                    char buff[50];

                    if (!(ctx->flags & FLAG_SIZE_IN_BYTES)) {
                        generate_size(finfo.size, buff, sizeof(buff));
                        s_len = strlen (buff);
                    }
                    else {
                        int l, x, pos = 0;
                        char tmp_buff[50];

                        apr_snprintf(tmp_buff, sizeof(tmp_buff), "%" APR_OFF_T_FMT, finfo.size);
                        l = strlen(tmp_buff);    /* grrr */
                        for (x = 0; x < l; x++) {
                            if (x && (!((l - x) % 3))) {
                                buff[pos++] = ',';
                            }
                            buff[pos++] = tmp_buff[x];
                        }
                        buff[pos] = '\0';
                        s_len = pos;
                    }

                    tmp_buck = apr_bucket_heap_create(buff, s_len, 1, &s_wrt);
                    APR_BUCKET_INSERT_BEFORE(head_ptr, tmp_buck);
                    if (*inserted_head == NULL) {
                        *inserted_head = tmp_buck;
                    }
                }
                else {
                    CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
                }
            }
        }
    }
    return 0;
}

static int handle_flastmod(include_ctx_t *ctx, apr_bucket_brigade **bb, request_rec *r,
                           ap_filter_t *f, apr_bucket *head_ptr, apr_bucket **inserted_head)
{
    char *tag     = NULL;
    char *tag_val = NULL;
    apr_finfo_t  finfo;
    apr_size_t  t_len, t_wrt;
    apr_bucket   *tmp_buck;
    char parsed_string[MAX_STRING_LEN];

    *inserted_head = NULL;
    if (ctx->flags & FLAG_PRINTING) {
        while (1) {
            ap_ssi_get_tag_and_value(ctx, &tag, &tag_val, 1);
            if (tag_val == NULL) {
                if (tag == NULL) {
                    return 0;
                }
                else {
                    return 1;
                }
            }
            else {
                ap_ssi_parse_string(r, tag_val, parsed_string, sizeof(parsed_string), 0);
                if (!find_file(r, "flastmod", tag, parsed_string, &finfo)) {
                    char *t_val;

                    t_val = ap_ht_time(r->pool, finfo.mtime, ctx->time_str, 0);
                    t_len = strlen(t_val);

                    tmp_buck = apr_bucket_heap_create(t_val, t_len, 1, &t_wrt);
                    APR_BUCKET_INSERT_BEFORE(head_ptr, tmp_buck);
                    if (*inserted_head == NULL) {
                        *inserted_head = tmp_buck;
                    }
                }
                else {
                    CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
                }
            }
        }
    }
    return 0;
}

static int re_check(request_rec *r, char *string, char *rexp)
{
    regex_t *compiled;
    int regex_error;

    compiled = ap_pregcomp(r->pool, rexp, REG_EXTENDED | REG_NOSUB);
    if (compiled == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                    "unable to compile pattern \"%s\"", rexp);
        return -1;
    }
    regex_error = ap_regexec(compiled, string, 0, (regmatch_t *) NULL, 0);
    ap_pregfree(r->pool, compiled);
    return (!regex_error);
}

enum token_type {
    token_string,
    token_and, token_or, token_not, token_eq, token_ne,
    token_rbrace, token_lbrace, token_group,
    token_ge, token_le, token_gt, token_lt
};
struct token {
    enum token_type type;
    char value[MAX_STRING_LEN];
};

/* there is an implicit assumption here that string is at most MAX_STRING_LEN-1
 * characters long...
 */
static const char *get_ptoken(request_rec *r, const char *string, struct token *token,
                              int *unmatched)
{
    char ch;
    int next = 0;
    int qs = 0;
    int tkn_fnd = 0;

    /* Skip leading white space */
    if (string == (char *) NULL) {
        return (char *) NULL;
    }
    while ((ch = *string++)) {
        if (!apr_isspace(ch)) {
            break;
        }
    }
    if (ch == '\0') {
        return (char *) NULL;
    }

    token->type = token_string; /* the default type */
    switch (ch) {
    case '(':
        token->type = token_lbrace;
        return (string);
    case ')':
        token->type = token_rbrace;
        return (string);
    case '=':
        token->type = token_eq;
        return (string);
    case '!':
        if (*string == '=') {
            token->type = token_ne;
            return (string + 1);
        }
        else {
            token->type = token_not;
            return (string);
        }
    case '\'':
        token->type = token_string;
        qs = 1;
        break;
    case '|':
        if (*string == '|') {
            token->type = token_or;
            return (string + 1);
        }
        break;
    case '&':
        if (*string == '&') {
            token->type = token_and;
            return (string + 1);
        }
        break;
    case '>':
        if (*string == '=') {
            token->type = token_ge;
            return (string + 1);
        }
        else {
            token->type = token_gt;
            return (string);
        }
    case '<':
        if (*string == '=') {
            token->type = token_le;
            return (string + 1);
        }
        else {
            token->type = token_lt;
            return (string);
        }
    default:
        token->type = token_string;
        break;
    }
    /* We should only be here if we are in a string */
    if (!qs) {
        token->value[next++] = ch;
    }

    /* 
     * Yes I know that goto's are BAD.  But, c doesn't allow me to
     * exit a loop from a switch statement.  Yes, I could use a flag,
     * but that is (IMHO) even less readable/maintainable than the goto.
     */
    /* 
     * I used the ++string throughout this section so that string
     * ends up pointing to the next token and I can just return it
     */
    for (ch = *string; ((ch != '\0') && (!tkn_fnd)); ch = *++string) {
        if (ch == '\\') {
            if ((ch = *++string) == '\0') {
                tkn_fnd = 1;
            }
            else {
                token->value[next++] = ch;
            }
        }
        else {
            if (!qs) {
                if (apr_isspace(ch)) {
                    tkn_fnd = 1;
                }
                else {
                    switch (ch) {
                    case '(':
                    case ')':
                    case '=':
                    case '!':
                    case '<':
                    case '>':
                        tkn_fnd = 1;
                        break;
                    case '|':
                        if (*(string + 1) == '|') {
                            tkn_fnd = 1;
                        }
                        break;
                    case '&':
                        if (*(string + 1) == '&') {
                            tkn_fnd = 1;
                        }
                        break;
                    }
                    if (!tkn_fnd) {
                        token->value[next++] = ch;
                    }
                }
            }
            else {
                if (ch == '\'') {
                    qs = 0;
                    ++string;
                    tkn_fnd = 1;
                }
                else {
                    token->value[next++] = ch;
                }
            }
        }
    }

    /* If qs is still set, I have an unmatched ' */
    if (qs) {
        *unmatched = 1;
        next = 0;
    }
    token->value[next] = '\0';
    return (string);
}


/*
 * Hey I still know that goto's are BAD.  I don't think that I've ever
 * used two in the same project, let alone the same file before.  But,
 * I absolutely want to make sure that I clean up the memory in all
 * cases.  And, without rewriting this completely, the easiest way
 * is to just branch to the return code which cleans it up.
 */
/* there is an implicit assumption here that expr is at most MAX_STRING_LEN-1
 * characters long...
 */
static int parse_expr(request_rec *r, const char *expr, int *was_error, 
                      int *was_unmatched, char *debug)
{
    struct parse_node {
        struct parse_node *left, *right, *parent;
        struct token token;
        int value, done;
    }         *root, *current, *new;
    const char *parse;
    char buffer[MAX_STRING_LEN];
    apr_pool_t *expr_pool;
    int retval = 0;
    apr_size_t debug_pos = 0;

    debug[debug_pos] = '\0';
    *was_error       = 0;
    *was_unmatched   = 0;
    if ((parse = expr) == (char *) NULL) {
        return (0);
    }
    root = current = (struct parse_node *) NULL;
    if (apr_pool_create(&expr_pool, r->pool) != APR_SUCCESS)
		return 0;

    /* Create Parse Tree */
    while (1) {
        new = (struct parse_node *) apr_palloc(expr_pool,
                                           sizeof(struct parse_node));
        new->parent = new->left = new->right = (struct parse_node *) NULL;
        new->done = 0;
        if ((parse = get_ptoken(r, parse, &new->token, was_unmatched)) == (char *) NULL) {
            break;
        }
        switch (new->token.type) {

        case token_string:
#ifdef DEBUG_INCLUDE
            debug_pos += sprintf (&debug[debug_pos], "     Token: string (%s)\n",
                                  new->token.value);
#endif
            if (current == (struct parse_node *) NULL) {
                root = current = new;
                break;
            }
            switch (current->token.type) {
            case token_string:
                if (current->token.value[0] != '\0') {
                    strncat(current->token.value, " ",
                         sizeof(current->token.value)
			    - strlen(current->token.value) - 1);
                }
                strncat(current->token.value, new->token.value,
                         sizeof(current->token.value)
			    - strlen(current->token.value) - 1);
		current->token.value[sizeof(current->token.value) - 1] = '\0';
                break;
            case token_eq:
            case token_ne:
            case token_and:
            case token_or:
            case token_lbrace:
            case token_not:
            case token_ge:
            case token_gt:
            case token_le:
            case token_lt:
                new->parent = current;
                current = current->right = new;
                break;
            default:
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                            "Invalid expression \"%s\" in file %s",
                            expr, r->filename);
                *was_error = 1;
                goto RETURN;
            }
            break;

        case token_and:
        case token_or:
#ifdef DEBUG_INCLUDE
            memcpy (&debug[debug_pos], "     Token: and/or\n",
                    sizeof ("     Token: and/or\n"));
            debug_pos += sizeof ("     Token: and/or\n");
#endif
            if (current == (struct parse_node *) NULL) {
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                            "Invalid expression \"%s\" in file %s",
                            expr, r->filename);
                *was_error = 1;
                goto RETURN;
            }
            /* Percolate upwards */
            while (current != (struct parse_node *) NULL) {
                switch (current->token.type) {
                case token_string:
                case token_group:
                case token_not:
                case token_eq:
                case token_ne:
                case token_and:
                case token_or:
                case token_ge:
                case token_gt:
                case token_le:
                case token_lt:
                    current = current->parent;
                    continue;
                case token_lbrace:
                    break;
                default:
                    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                                "Invalid expression \"%s\" in file %s",
                                expr, r->filename);
                    *was_error = 1;
                    goto RETURN;
                }
                break;
            }
            if (current == (struct parse_node *) NULL) {
                new->left = root;
                new->left->parent = new;
                new->parent = (struct parse_node *) NULL;
                root = new;
            }
            else {
                new->left = current->right;
                current->right = new;
                new->parent = current;
            }
            current = new;
            break;

        case token_not:
#ifdef DEBUG_INCLUDE
            memcpy (&debug[debug_pos], "     Token: not\n",
                    sizeof ("     Token: not\n"));
            debug_pos += sizeof ("     Token: not\n");
#endif
            if (current == (struct parse_node *) NULL) {
                root = current = new;
                break;
            }
            /* Percolate upwards */
            while (current != (struct parse_node *) NULL) {
                switch (current->token.type) {
                case token_not:
                case token_eq:
                case token_ne:
                case token_and:
                case token_or:
                case token_lbrace:
                case token_ge:
                case token_gt:
                case token_le:
                case token_lt:
                    break;
                default:
                    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                                "Invalid expression \"%s\" in file %s",
                                expr, r->filename);
                    *was_error = 1;
                    goto RETURN;
                }
                break;
            }
            if (current == (struct parse_node *) NULL) {
                new->left = root;
                new->left->parent = new;
                new->parent = (struct parse_node *) NULL;
                root = new;
            }
            else {
                new->left = current->right;
                current->right = new;
                new->parent = current;
            }
            current = new;
            break;

        case token_eq:
        case token_ne:
        case token_ge:
        case token_gt:
        case token_le:
        case token_lt:
#ifdef DEBUG_INCLUDE
            memcpy (&debug[debug_pos], "     Token: eq/ne/ge/gt/le/lt\n",
                    sizeof ("     Token: eq/ne/ge/gt/le/lt\n"));
            debug_pos += sizeof ("     Token: eq/ne/ge/gt/le/lt\n");
#endif
            if (current == (struct parse_node *) NULL) {
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                            "Invalid expression \"%s\" in file %s",
                            expr, r->filename);
                *was_error = 1;
                goto RETURN;
            }
            /* Percolate upwards */
            while (current != (struct parse_node *) NULL) {
                switch (current->token.type) {
                case token_string:
                case token_group:
                    current = current->parent;
                    continue;
                case token_lbrace:
                case token_and:
                case token_or:
                    break;
                case token_not:
                case token_eq:
                case token_ne:
                case token_ge:
                case token_gt:
                case token_le:
                case token_lt:
                default:
                    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                                "Invalid expression \"%s\" in file %s",
                                expr, r->filename);
                    *was_error = 1;
                    goto RETURN;
                }
                break;
            }
            if (current == (struct parse_node *) NULL) {
                new->left = root;
                new->left->parent = new;
                new->parent = (struct parse_node *) NULL;
                root = new;
            }
            else {
                new->left = current->right;
                current->right = new;
                new->parent = current;
            }
            current = new;
            break;

        case token_rbrace:
#ifdef DEBUG_INCLUDE
            memcpy (&debug[debug_pos], "     Token: rbrace\n",
                    sizeof ("     Token: rbrace\n"));
            debug_pos += sizeof ("     Token: rbrace\n");
#endif
            while (current != (struct parse_node *) NULL) {
                if (current->token.type == token_lbrace) {
                    current->token.type = token_group;
                    break;
                }
                current = current->parent;
            }
            if (current == (struct parse_node *) NULL) {
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                            "Unmatched ')' in \"%s\" in file %s",
			    expr, r->filename);
                *was_error = 1;
                goto RETURN;
            }
            break;

        case token_lbrace:
#ifdef DEBUG_INCLUDE
            memcpy (&debug[debug_pos], "     Token: lbrace\n",
                    sizeof ("     Token: lbrace\n"));
            debug_pos += sizeof ("     Token: lbrace\n");
#endif
            if (current == (struct parse_node *) NULL) {
                root = current = new;
                break;
            }
            /* Percolate upwards */
            while (current != (struct parse_node *) NULL) {
                switch (current->token.type) {
                case token_not:
                case token_eq:
                case token_ne:
                case token_and:
                case token_or:
                case token_lbrace:
                case token_ge:
                case token_gt:
                case token_le:
                case token_lt:
                    break;
                case token_string:
                case token_group:
                default:
                    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                                "Invalid expression \"%s\" in file %s",
                                expr, r->filename);
                    *was_error = 1;
                    goto RETURN;
                }
                break;
            }
            if (current == (struct parse_node *) NULL) {
                new->left = root;
                new->left->parent = new;
                new->parent = (struct parse_node *) NULL;
                root = new;
            }
            else {
                new->left = current->right;
                current->right = new;
                new->parent = current;
            }
            current = new;
            break;
        default:
            break;
        }
    }

    /* Evaluate Parse Tree */
    current = root;
    while (current != (struct parse_node *) NULL) {
        switch (current->token.type) {
        case token_string:
#ifdef DEBUG_INCLUDE
            memcpy (&debug[debug_pos], "     Evaluate string\n",
                    sizeof ("     Evaluate string\n"));
            debug_pos += sizeof ("     Evaluate string\n");
#endif
            ap_ssi_parse_string(r, current->token.value, buffer, sizeof(buffer), 0);
	    apr_cpystrn(current->token.value, buffer, sizeof(current->token.value));
            current->value = (current->token.value[0] != '\0');
            current->done = 1;
            current = current->parent;
            break;

        case token_and:
        case token_or:
#ifdef DEBUG_INCLUDE
            memcpy (&debug[debug_pos], "     Evaluate and/or\n",
                    sizeof ("     Evaluate and/or\n"));
            debug_pos += sizeof ("     Evaluate and/or\n");
#endif
            if (current->left == (struct parse_node *) NULL ||
                current->right == (struct parse_node *) NULL) {
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                            "Invalid expression \"%s\" in file %s",
                            expr, r->filename);
                *was_error = 1;
                goto RETURN;
            }
            if (!current->left->done) {
                switch (current->left->token.type) {
                case token_string:
                    ap_ssi_parse_string(r, current->left->token.value,
                                 buffer, sizeof(buffer), 0);
                    apr_cpystrn(current->left->token.value, buffer,
                            sizeof(current->left->token.value));
		    current->left->value = (current->left->token.value[0] != '\0');
                    current->left->done = 1;
                    break;
                default:
                    current = current->left;
                    continue;
                }
            }
            if (!current->right->done) {
                switch (current->right->token.type) {
                case token_string:
                    ap_ssi_parse_string(r, current->right->token.value,
                                 buffer, sizeof(buffer), 0);
                    apr_cpystrn(current->right->token.value, buffer,
                            sizeof(current->right->token.value));
		    current->right->value = (current->right->token.value[0] != '\0');
                    current->right->done = 1;
                    break;
                default:
                    current = current->right;
                    continue;
                }
            }
#ifdef DEBUG_INCLUDE
            debug_pos += sprintf (&debug[debug_pos], "     Left: %c\n",
                                  current->left->value ? '1' : '0');
            debug_pos += sprintf (&debug[debug_pos], "     Right: %c\n",
                                  current->right->value ? '1' : '0');
#endif
            if (current->token.type == token_and) {
                current->value = current->left->value && current->right->value;
            }
            else {
                current->value = current->left->value || current->right->value;
            }
#ifdef DEBUG_INCLUDE
            debug_pos += sprintf (&debug[debug_pos], "     Returning %c\n",
                                  current->value ? '1' : '0');
#endif
            current->done = 1;
            current = current->parent;
            break;

        case token_eq:
        case token_ne:
#ifdef DEBUG_INCLUDE
            memcpy (&debug[debug_pos], "     Evaluate eq/ne\n",
                    sizeof ("     Evaluate eq/ne\n"));
            debug_pos += sizeof ("     Evaluate eq/ne\n");
#endif
            if ((current->left == (struct parse_node *) NULL) ||
                (current->right == (struct parse_node *) NULL) ||
                (current->left->token.type != token_string) ||
                (current->right->token.type != token_string)) {
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                            "Invalid expression \"%s\" in file %s",
                            expr, r->filename);
                *was_error = 1;
                goto RETURN;
            }
            ap_ssi_parse_string(r, current->left->token.value,
                         buffer, sizeof(buffer), 0);
            apr_cpystrn(current->left->token.value, buffer,
			sizeof(current->left->token.value));
            ap_ssi_parse_string(r, current->right->token.value,
                         buffer, sizeof(buffer), 0);
            apr_cpystrn(current->right->token.value, buffer,
			sizeof(current->right->token.value));
            if (current->right->token.value[0] == '/') {
                int len;
                len = strlen(current->right->token.value);
                if (current->right->token.value[len - 1] == '/') {
                    current->right->token.value[len - 1] = '\0';
                }
                else {
                    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                                "Invalid rexp \"%s\" in file %s",
                                current->right->token.value, r->filename);
                    *was_error = 1;
                    goto RETURN;
                }
#ifdef DEBUG_INCLUDE
                debug_pos += sprintf (&debug[debug_pos],
                                      "     Re Compare (%s) with /%s/\n",
                                      current->left->token.value,
                                      &current->right->token.value[1]);
#endif
                current->value =
                    re_check(r, current->left->token.value,
                             &current->right->token.value[1]);
            }
            else {
#ifdef DEBUG_INCLUDE
                debug_pos += sprintf (&debug[debug_pos],
                                      "     Compare (%s) with (%s)\n",
                                      current->left->token.value,
                                      current->right->token.value);
#endif
                current->value =
                    (strcmp(current->left->token.value,
                            current->right->token.value) == 0);
            }
            if (current->token.type == token_ne) {
                current->value = !current->value;
            }
#ifdef DEBUG_INCLUDE
            debug_pos += sprintf (&debug[debug_pos], "     Returning %c\n",
                                  current->value ? '1' : '0');
#endif
            current->done = 1;
            current = current->parent;
            break;
        case token_ge:
        case token_gt:
        case token_le:
        case token_lt:
#ifdef DEBUG_INCLUDE
            memcpy (&debug[debug_pos], "     Evaluate ge/gt/le/lt\n",
                    sizeof ("     Evaluate ge/gt/le/lt\n"));
            debug_pos += sizeof ("     Evaluate ge/gt/le/lt\n");
#endif
            if ((current->left == (struct parse_node *) NULL) ||
                (current->right == (struct parse_node *) NULL) ||
                (current->left->token.type != token_string) ||
                (current->right->token.type != token_string)) {
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                            "Invalid expression \"%s\" in file %s",
                            expr, r->filename);
                *was_error = 1;
                goto RETURN;
            }
            ap_ssi_parse_string(r, current->left->token.value,
                         buffer, sizeof(buffer), 0);
            apr_cpystrn(current->left->token.value, buffer,
			sizeof(current->left->token.value));
            ap_ssi_parse_string(r, current->right->token.value,
                         buffer, sizeof(buffer), 0);
            apr_cpystrn(current->right->token.value, buffer,
			sizeof(current->right->token.value));
#ifdef DEBUG_INCLUDE
            debug_pos += sprintf (&debug[debug_pos],
                                  "     Compare (%s) with (%s)\n",
                                  current->left->token.value,
                                  current->right->token.value);
#endif
            current->value =
                strcmp(current->left->token.value,
                       current->right->token.value);
            if (current->token.type == token_ge) {
                current->value = current->value >= 0;
            }
            else if (current->token.type == token_gt) {
                current->value = current->value > 0;
            }
            else if (current->token.type == token_le) {
                current->value = current->value <= 0;
            }
            else if (current->token.type == token_lt) {
                current->value = current->value < 0;
            }
            else {
                current->value = 0;     /* Don't return -1 if unknown token */
            }
#ifdef DEBUG_INCLUDE
            debug_pos += sprintf (&debug[debug_pos], "     Returning %c\n",
                                  current->value ? '1' : '0');
#endif
            current->done = 1;
            current = current->parent;
            break;

        case token_not:
            if (current->right != (struct parse_node *) NULL) {
                if (!current->right->done) {
                    current = current->right;
                    continue;
                }
                current->value = !current->right->value;
            }
            else {
                current->value = 0;
            }
#ifdef DEBUG_INCLUDE
            debug_pos += sprintf (&debug[debug_pos], "     Evaluate !: %c\n",
                                  current->value ? '1' : '0');
#endif
            current->done = 1;
            current = current->parent;
            break;

        case token_group:
            if (current->right != (struct parse_node *) NULL) {
                if (!current->right->done) {
                    current = current->right;
                    continue;
                }
                current->value = current->right->value;
            }
            else {
                current->value = 1;
            }
#ifdef DEBUG_INCLUDE
            debug_pos += sprintf (&debug[debug_pos], "     Evaluate (): %c\n",
                                  current->value ? '1' : '0');
#endif
            current->done = 1;
            current = current->parent;
            break;

        case token_lbrace:
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                        "Unmatched '(' in \"%s\" in file %s",
                        expr, r->filename);
            *was_error = 1;
            goto RETURN;

        case token_rbrace:
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                        "Unmatched ')' in \"%s\" in file %s",
                        expr, r->filename);
            *was_error = 1;
            goto RETURN;

        default:
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                          "bad token type");
            *was_error = 1;
            goto RETURN;
        }
    }

    retval = (root == (struct parse_node *) NULL) ? 0 : root->value;
  RETURN:
    apr_pool_destroy(expr_pool);
    return (retval);
}

/*-------------------------------------------------------------------------*/
#ifdef DEBUG_INCLUDE

/* XXX overlaying the static string pointed to by cond_txt isn't cool */

#define MAX_DEBUG_SIZE MAX_STRING_LEN
#define LOG_COND_STATUS(cntx, t_buck, h_ptr, ins_head, tag_text)           \
{                                                                          \
    char *cond_txt = "**** X     conditional_status=\"0\"\n";              \
    apr_size_t c_wrt;                                                      \
                                                                           \
    if (cntx->flags & FLAG_COND_TRUE) {                                    \
        cond_txt[31] = '1';                                                \
    }                                                                      \
    memcpy(&cond_txt[5], tag_text, sizeof(tag_text));                      \
    t_buck = apr_bucket_heap_create(cond_txt, sizeof(cond_txt), 1, &c_wrt); \
    APR_BUCKET_INSERT_BEFORE(h_ptr, t_buck);                                \
                                                                           \
    if (ins_head == NULL) {                                                \
        ins_head = t_buck;                                                 \
    }                                                                      \
}
#define DUMP_PARSE_EXPR_DEBUG(t_buck, h_ptr, d_buf, ins_head)            \
{                                                                        \
    apr_size_t b_wrt;                                                    \
    if (d_buf[0] != '\0') {                                              \
        t_buck = apr_bucket_heap_create(d_buf, strlen(d_buf), 1, &b_wrt); \
        APR_BUCKET_INSERT_BEFORE(h_ptr, t_buck);                          \
                                                                         \
        if (ins_head == NULL) {                                          \
            ins_head = t_buck;                                           \
        }                                                                \
    }                                                                    \
}
#else

#define MAX_DEBUG_SIZE 10
#define LOG_COND_STATUS(cntx, t_buck, h_ptr, ins_head, tag_text)
#define DUMP_PARSE_EXPR_DEBUG(t_buck, h_ptr, d_buf, ins_head)

#endif
/*-------------------------------------------------------------------------*/

/* pjr - These seem to allow expr="fred" expr="joe" where joe overwrites fred. */
static int handle_if(include_ctx_t *ctx, apr_bucket_brigade **bb, request_rec *r,
                     ap_filter_t *f, apr_bucket *head_ptr, apr_bucket **inserted_head)
{
    char *tag     = NULL;
    char *tag_val = NULL;
    char *expr    = NULL;
    int   expr_ret, was_error, was_unmatched;
    apr_bucket *tmp_buck;
    char debug_buf[MAX_DEBUG_SIZE];

    *inserted_head = NULL;
    if (!ctx->flags & FLAG_PRINTING) {
        ctx->if_nesting_level++;
    }
    else {
        while (1) {
            ap_ssi_get_tag_and_value(ctx, &tag, &tag_val, 0);
            if (tag == NULL) {
                if (expr == NULL) {
                    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                                  "missing expr in if statement: %s", r->filename);
                    CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
                    return 1;
                }
                expr_ret = parse_expr(r, expr, &was_error, &was_unmatched, debug_buf);
                if (was_error) {
                    CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
                    return 1;
                }
                if (was_unmatched) {
                    DUMP_PARSE_EXPR_DEBUG(tmp_buck, head_ptr, "\nUnmatched '\n",
                                          *inserted_head);
                }
                DUMP_PARSE_EXPR_DEBUG(tmp_buck, head_ptr, debug_buf, *inserted_head);
                
                if (expr_ret) {
                    ctx->flags |= (FLAG_PRINTING | FLAG_COND_TRUE);
                }
                else {
                    ctx->flags &= FLAG_CLEAR_PRINT_COND;
                }
                LOG_COND_STATUS(ctx, tmp_buck, head_ptr, *inserted_head, "   if");
                ctx->if_nesting_level = 0;
                return 0;
            }
            else if (!strcmp(tag, "expr")) {
                expr = tag_val;
#ifdef DEBUG_INCLUDE
                if (1) {
                    apr_size_t d_len = 0, d_wrt = 0;
                    d_len = sprintf(debug_buf, "**** if expr=\"%s\"\n", expr);
                    tmp_buck = apr_bucket_heap_create(debug_buf, d_len, 1, &d_wrt);
                    APR_BUCKET_INSERT_BEFORE(head_ptr, tmp_buck);

                    if (*inserted_head == NULL) {
                        *inserted_head = tmp_buck;
                    }
                }
#endif
            }
            else {
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                            "unknown parameter \"%s\" to tag if in %s", tag, r->filename);
                CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
            }

        }
    }
    return 0;
}

static int handle_elif(include_ctx_t *ctx, apr_bucket_brigade **bb, request_rec *r,
                       ap_filter_t *f,  apr_bucket *head_ptr, apr_bucket **inserted_head)
{
    char *tag     = NULL;
    char *tag_val = NULL;
    char *expr    = NULL;
    int   expr_ret, was_error, was_unmatched;
    apr_bucket *tmp_buck;
    char debug_buf[MAX_DEBUG_SIZE];

    *inserted_head = NULL;
    if (!ctx->if_nesting_level) {
        while (1) {
            ap_ssi_get_tag_and_value(ctx, &tag, &tag_val, 0);
            if (tag == '\0') {
                LOG_COND_STATUS(ctx, tmp_buck, head_ptr, *inserted_head, " elif");
                
                if (ctx->flags & FLAG_COND_TRUE) {
                    ctx->flags &= FLAG_CLEAR_PRINTING;
                    return (0);
                }
                if (expr == NULL) {
                    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                                  "missing expr in elif statement: %s", r->filename);
                    CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
                    return (1);
                }
                expr_ret = parse_expr(r, expr, &was_error, &was_unmatched, debug_buf);
                if (was_error) {
                    CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
                    return 1;
                }
                if (was_unmatched) {
                    DUMP_PARSE_EXPR_DEBUG(tmp_buck, head_ptr, "\nUnmatched '\n",
                                          *inserted_head);
                }
                DUMP_PARSE_EXPR_DEBUG(tmp_buck, head_ptr, debug_buf, *inserted_head);
                
                if (expr_ret) {
                    ctx->flags |= (FLAG_PRINTING | FLAG_COND_TRUE);
                }
                else {
                    ctx->flags &= FLAG_CLEAR_PRINT_COND;
                }
                LOG_COND_STATUS(ctx, tmp_buck, head_ptr, *inserted_head, " elif");
                return (0);
            }
            else if (!strcmp(tag, "expr")) {
                expr = tag_val;
#ifdef DEBUG_INCLUDE
                if (1) {
                    apr_size_t d_len = 0, d_wrt = 0;
                    d_len = sprintf(debug_buf, "**** elif expr=\"%s\"\n", expr);
                    tmp_buck = apr_bucket_heap_create(debug_buf, d_len, 1, &d_wrt);
                    APR_BUCKET_INSERT_BEFORE(head_ptr, tmp_buck);

                    if (*inserted_head == NULL) {
                        *inserted_head = tmp_buck;
                    }
                }
#endif
            }
            else {
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                            "unknown parameter \"%s\" to tag if in %s", tag, r->filename);
                CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
            }
        }
    }
    return 0;
}

static int handle_else(include_ctx_t *ctx, apr_bucket_brigade **bb, request_rec *r,
                       ap_filter_t *f, apr_bucket *head_ptr, apr_bucket **inserted_head)
{
    char *tag = NULL;
    char *tag_val = NULL;
    apr_bucket *tmp_buck;

    *inserted_head = NULL;
    if (!ctx->if_nesting_level) {
        ap_ssi_get_tag_and_value(ctx, &tag, &tag_val, 1);
        if ((tag != NULL) || (tag_val != NULL)) {
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                        "else directive does not take tags in %s", r->filename);
            if (ctx->flags & FLAG_PRINTING) {
                CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
            }
            return -1;
        }
        else {
            LOG_COND_STATUS(ctx, tmp_buck, head_ptr, *inserted_head, " else");
            
            if (ctx->flags & FLAG_COND_TRUE) {
                ctx->flags &= FLAG_CLEAR_PRINTING;
            }
            else {
                ctx->flags |= (FLAG_PRINTING | FLAG_COND_TRUE);
            }
            return 0;
        }
    }
    return 0;
}

static int handle_endif(include_ctx_t *ctx, apr_bucket_brigade **bb, request_rec *r,
                        ap_filter_t *f, apr_bucket *head_ptr, apr_bucket **inserted_head)
{
    char *tag     = NULL;
    char *tag_val = NULL;
    apr_bucket *tmp_buck;

    *inserted_head = NULL;
    if (!ctx->if_nesting_level) {
        ap_ssi_get_tag_and_value(ctx, &tag, &tag_val, 1);
        if ((tag != NULL) || (tag_val != NULL)) {
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                        "endif directive does not take tags in %s", r->filename);
            CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
            return -1;
        }
        else {
            LOG_COND_STATUS(ctx, tmp_buck, head_ptr, *inserted_head, "endif");
            ctx->flags |= (FLAG_PRINTING | FLAG_COND_TRUE);
            return 0;
        }
    }
    else {
        ctx->if_nesting_level--;
        return 0;
    }
}

static int handle_set(include_ctx_t *ctx, apr_bucket_brigade **bb, request_rec *r,
                      ap_filter_t *f, apr_bucket *head_ptr, apr_bucket **inserted_head)
{
    char *tag     = NULL;
    char *tag_val = NULL;
    char *var     = NULL;
    apr_bucket *tmp_buck;
    char parsed_string[MAX_STRING_LEN];

    *inserted_head = NULL;
    if (ctx->flags & FLAG_PRINTING) {
        while (1) {
            ap_ssi_get_tag_and_value(ctx, &tag, &tag_val, 1);
            if ((tag == NULL) && (tag_val == NULL)) {
                return 0;
            }
            else if (tag_val == NULL) {
                return 1;
            }
            else if (!strcmp(tag, "var")) {
                var = tag_val;
            }
            else if (!strcmp(tag, "value")) {
                if (var == (char *) NULL) {
                    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                                "variable must precede value in set directive in %s",
    			    r->filename);
                    CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
                    return (-1);
                }
                ap_ssi_parse_string(r, tag_val, parsed_string, sizeof(parsed_string), 0);
                apr_table_setn(r->subprocess_env, apr_pstrdup(r->pool, var),
                               apr_pstrdup(r->pool, parsed_string));
            }
            else {
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                            "Invalid tag for set directive in %s", r->filename);
                CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
                return -1;
            }
        }
    }
    return 0;
}

static int handle_printenv(include_ctx_t *ctx, apr_bucket_brigade **bb, request_rec *r,
                           ap_filter_t *f, apr_bucket *head_ptr, apr_bucket **inserted_head)
{
    char *tag     = NULL;
    char *tag_val = NULL;
    apr_bucket *tmp_buck;

    if (ctx->flags & FLAG_PRINTING) {
        ap_ssi_get_tag_and_value(ctx, &tag, &tag_val, 1);
        if ((tag == NULL) && (tag_val == NULL)) {
            apr_array_header_t *arr = apr_table_elts(r->subprocess_env);
            apr_table_entry_t *elts = (apr_table_entry_t *)arr->elts;
            int i;
            char *key_text, *val_text;
            apr_size_t   k_len, v_len, t_wrt;

            *inserted_head = NULL;
            for (i = 0; i < arr->nelts; ++i) {
                key_text = ap_escape_html(r->pool, elts[i].key);
                val_text = ap_escape_html(r->pool, elts[i].val);
                k_len = strlen(key_text);
                v_len = strlen(val_text);

                /*  Key_text                                               */
                tmp_buck = apr_bucket_heap_create(key_text, k_len, 1, &t_wrt);
                APR_BUCKET_INSERT_BEFORE(head_ptr, tmp_buck);
                if (*inserted_head == NULL) {
                    *inserted_head = tmp_buck;
                }
                /*            =                                            */
                tmp_buck = apr_bucket_immortal_create("=", 1);
                APR_BUCKET_INSERT_BEFORE(head_ptr, tmp_buck);
                /*              Value_text                                 */
                tmp_buck = apr_bucket_heap_create(val_text, v_len, 1, &t_wrt);
                APR_BUCKET_INSERT_BEFORE(head_ptr, tmp_buck);
                /*                        newline...                       */
                tmp_buck = apr_bucket_immortal_create("\n", 1);
                APR_BUCKET_INSERT_BEFORE(head_ptr, tmp_buck);
            }
            return 0;
        }
        else {
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                        "printenv directive does not take tags in %s", r->filename);
            CREATE_ERROR_BUCKET(ctx, tmp_buck, head_ptr, *inserted_head);
            return -1;
        }
    }
    return 0;
}

/* -------------------------- The main function --------------------------- */

static void send_parsed_content(apr_bucket_brigade **bb, request_rec *r, 
                                ap_filter_t *f)
{
    include_ctx_t *ctx = f->ctx;
    apr_bucket *dptr = APR_BRIGADE_FIRST(*bb);
    apr_bucket *tmp_dptr;
    apr_bucket_brigade *tag_and_after;
    int ret;

    if (r->args) {              /* add QUERY stuff to env cause it ain't yet */
        char *arg_copy = apr_pstrdup(r->pool, r->args);

        apr_table_setn(r->subprocess_env, "QUERY_STRING", r->args);
        ap_unescape_url(arg_copy);
        apr_table_setn(r->subprocess_env, "QUERY_STRING_UNESCAPED",
                  ap_escape_shell_cmd(r->pool, arg_copy));
    }

    while (dptr != APR_BRIGADE_SENTINEL(*bb)) {
        /* State to check for the STARTING_SEQUENCE. */
        if ((ctx->state == PRE_HEAD) || (ctx->state == PARSE_HEAD)) {
            int do_cleanup = 0;
            apr_size_t cleanup_bytes = ctx->parse_pos;

            tmp_dptr = find_start_sequence(dptr, ctx, *bb, &do_cleanup);

            /* The few bytes stored in the ssi_tag_brigade turned out not to
             * be a tag after all. This can only happen if the starting
             * tag actually spans brigades. This should be very rare.
             */
            if ((do_cleanup) && (!APR_BRIGADE_EMPTY(ctx->ssi_tag_brigade))) {
                apr_bucket *tmp_bkt;

                tmp_bkt = apr_bucket_immortal_create(STARTING_SEQUENCE, cleanup_bytes);
                APR_BRIGADE_INSERT_HEAD(*bb, tmp_bkt);

                while (!APR_BRIGADE_EMPTY(ctx->ssi_tag_brigade)) {
                    tmp_bkt = APR_BRIGADE_FIRST(ctx->ssi_tag_brigade);
                    apr_bucket_delete(tmp_bkt);
                }
            }

            /* If I am inside a conditional (if, elif, else) that is false
             *   then I need to throw away anything contained in it.
             */
            if ((!(ctx->flags & FLAG_PRINTING)) && (tmp_dptr != NULL) &&
                (dptr != APR_BRIGADE_SENTINEL(*bb))) {
                while ((dptr != APR_BRIGADE_SENTINEL(*bb)) &&
                       (dptr != tmp_dptr)) {
                    apr_bucket *free_bucket = dptr;

                    dptr = APR_BUCKET_NEXT (dptr);
                    apr_bucket_delete(free_bucket);
                }
            }

            /* Adjust the current bucket position based on what was found... */
            if ((tmp_dptr != NULL) && (ctx->state == PARSE_DIRECTIVE)) {
                if (ctx->tag_start_bucket != NULL) {
                    dptr = ctx->tag_start_bucket;
                }
                else {
                    dptr = APR_BRIGADE_SENTINEL(*bb);
                }
            }
            else if ((tmp_dptr != NULL) && (ctx->bytes_parsed >= BYTE_COUNT_THRESHOLD)) {
                               /* Send the large chunk of pre-tag bytes...  */
                tag_and_after = apr_brigade_split(*bb, tmp_dptr);
                ap_pass_brigade(f->next, *bb);
                *bb  = tag_and_after;
                dptr = tmp_dptr;
                ctx->bytes_parsed = 0;
            }
            else if (tmp_dptr == NULL) { /* There was no possible SSI tag in the */
                dptr = APR_BRIGADE_SENTINEL(*bb);  /* remainder of this brigade...    */
            }
        }

        /* State to check for the ENDING_SEQUENCE. */
        if (((ctx->state == PARSE_DIRECTIVE) ||
             (ctx->state == PARSE_TAG)       ||
             (ctx->state == PARSE_TAIL))       &&
            (dptr != APR_BRIGADE_SENTINEL(*bb))) {
            tmp_dptr = find_end_sequence(dptr, ctx, *bb);

            if (tmp_dptr != NULL) {
                dptr = tmp_dptr;  /* Adjust bucket pos... */
                
                /* If some of the tag has already been set aside then set
                 * aside remainder of tag. Now the full tag is in ssi_tag_brigade.
                 * If none has yet been set aside, then leave it all where it is.
                 * In any event after this the entire set of tag buckets will be
                 * in one place or another.
                 */
                if (!APR_BRIGADE_EMPTY(ctx->ssi_tag_brigade)) {
                    tag_and_after = apr_brigade_split(*bb, dptr);
                    APR_BRIGADE_CONCAT(ctx->ssi_tag_brigade, *bb);
                    *bb = tag_and_after;
                }
                else if (ctx->bytes_parsed >= BYTE_COUNT_THRESHOLD) {
                    SPLIT_AND_PASS_PRETAG_BUCKETS(*bb, ctx, f->next);
                }
            }
            else {
                dptr = APR_BRIGADE_SENTINEL(*bb);  /* remainder of this brigade...    */
            }
        }

        /* State to processed the directive... */
        if (ctx->state == PARSED) {
            apr_bucket    *content_head = NULL, *tmp_bkt;
            apr_size_t    tmp_i;
            char          tmp_buf[TMP_BUF_SIZE];
            int (*handle_func)(include_ctx_t *, apr_bucket_brigade **, request_rec *,
                           ap_filter_t *, apr_bucket *, apr_bucket **);

            /* By now the full tag (all buckets) should either be set aside into
             *  ssi_tag_brigade or contained within the current bb. All tag
             *  processing from here on can assume that.
             */

            /* At this point, everything between ctx->head_start_bucket and
             * ctx->tail_start_bucket is an SSI
             * directive, we just have to deal with it now.
             */
            if (get_combined_directive(ctx, r, *bb, tmp_buf,
                                        TMP_BUF_SIZE) != APR_SUCCESS) {
		ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
			    "mod_include: error copying directive in %s",
			    r->filename);
                CREATE_ERROR_BUCKET(ctx, tmp_bkt, dptr, content_head);

                /* DO CLEANUP HERE!!!!! */
                tmp_dptr = ctx->head_start_bucket;
                if (!APR_BRIGADE_EMPTY(ctx->ssi_tag_brigade)) {
                    while (!APR_BRIGADE_EMPTY(ctx->ssi_tag_brigade)) {
                        tmp_bkt = APR_BRIGADE_FIRST(ctx->ssi_tag_brigade);
                        apr_bucket_delete(tmp_bkt);
                    }
                }
                else {
                    do {
                        tmp_bkt  = tmp_dptr;
                        tmp_dptr = APR_BUCKET_NEXT (tmp_dptr);
                        apr_bucket_delete(tmp_bkt);
                    } while ((tmp_dptr != dptr) &&
                             (tmp_dptr != APR_BRIGADE_SENTINEL(*bb)));
                }

                return;
            }

            /* Even if I don't generate any content, I know at this point that
             *   I will at least remove the discovered SSI tag, thereby making
             *   the content shorter than it was. This is the safest point I can
             *   find to unset this field.
             */
            apr_table_unset(f->r->headers_out, "Content-Length");

            /* Can't destroy the tag buckets until I'm done processing
             *  because the combined_tag might just be pointing to
             *  the contents of a single bucket!
             */

            /* Retrieve the handler function to be called for this directive from the
             *  functions registered in the hash table.
             * Need to lower case the directive for proper matching. Also need to have
             *  it NULL terminated (and include the NULL in the length) for proper
             *  hash matching.
             */
            for (tmp_i = 0; tmp_i < ctx->directive_length; tmp_i++) {
                ctx->combined_tag[tmp_i] = apr_tolower(ctx->combined_tag[tmp_i]);
            }
            ctx->combined_tag[ctx->directive_length] = '\0';
            ctx->curr_tag_pos = &ctx->combined_tag[ctx->directive_length+1];

            handle_func = 
                (int (*)(include_ctx_t *, apr_bucket_brigade **, request_rec *,
                    ap_filter_t *, apr_bucket *, apr_bucket **))
                apr_hash_get(include_hash, ctx->combined_tag, ctx->directive_length+1);
            if (handle_func != NULL) {
                ret = (*handle_func)(ctx, bb, r, f, dptr, &content_head);
            }
            else {
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                              "unknown directive \"%s\" in parsed doc %s",
                              ctx->combined_tag, r->filename);
                CREATE_ERROR_BUCKET(ctx, tmp_bkt, dptr, content_head);
            }

            /* This chunk of code starts at the first bucket in the chain
             * of tag buckets (assuming that by this point the bucket for
             * the STARTING_SEQUENCE has been split) and loops through to
             * the end of the tag buckets freeing them all.
             *
             * Remember that some part of this may have been set aside
             * into the ssi_tag_brigade and the remainder (possibly as
             * little as one byte) will be in the current brigade.
             *
             * The value of dptr should have been set during the
             * PARSE_TAIL state to the first bucket after the
             * ENDING_SEQUENCE.
             *
             * The value of content_head may have been set during processing
             * of the directive. If so, the content was inserted in front
             * of the dptr bucket. The inserted buckets should not be thrown
             * away here, but they should also not be parsed later.
             */
            if (content_head == NULL) {
                content_head = dptr;
            }
            tmp_dptr = ctx->head_start_bucket;
            if (!APR_BRIGADE_EMPTY(ctx->ssi_tag_brigade)) {
                while (!APR_BRIGADE_EMPTY(ctx->ssi_tag_brigade)) {
                    tmp_bkt = APR_BRIGADE_FIRST(ctx->ssi_tag_brigade);
                    apr_bucket_delete(tmp_bkt);
                }
            }
            else {
                do {
                    tmp_bkt  = tmp_dptr;
                    tmp_dptr = APR_BUCKET_NEXT (tmp_dptr);
                    apr_bucket_delete(tmp_bkt);
                } while ((tmp_dptr != content_head) &&
                         (tmp_dptr != APR_BRIGADE_SENTINEL(*bb)));
            }
            if (ctx->combined_tag == tmp_buf) {
                memset (ctx->combined_tag, '\0', ctx->tag_length);
                ctx->combined_tag = NULL;
            }

            /* Don't reset the flags or the nesting level!!! */
            ctx->parse_pos         = 0;
            ctx->head_start_bucket = NULL;
            ctx->head_start_index  = 0;
            ctx->tag_start_bucket  = NULL;
            ctx->tag_start_index   = 0;
            ctx->tail_start_bucket = NULL;
            ctx->tail_start_index  = 0;
            ctx->curr_tag_pos      = NULL;
            ctx->tag_length        = 0;
            ctx->directive_length  = 0;

            if (!APR_BRIGADE_EMPTY(ctx->ssi_tag_brigade)) {
                while (!APR_BRIGADE_EMPTY(ctx->ssi_tag_brigade)) {
                    tmp_bkt = APR_BRIGADE_FIRST(ctx->ssi_tag_brigade);
                    apr_bucket_delete(tmp_bkt);
                }
            }

            ctx->state     = PRE_HEAD;
        }
    }

    /* If I am in the middle of parsing an SSI tag then I need to set aside
     *   the pertinent trailing buckets and pass on the initial part of the
     *   brigade. The pertinent parts of the next brigades will be added to
     *   these set aside buckets to form the whole tag and will be processed
     *   once the whole tag has been found.
     */
    if (ctx->state == PRE_HEAD) {
        /* Inside a false conditional (if, elif, else), so toss it all... */
        if ((dptr != APR_BRIGADE_SENTINEL(*bb)) &&
            (!(ctx->flags & FLAG_PRINTING))) {
            apr_bucket *free_bucket;
            do {
                free_bucket = dptr;
                dptr = APR_BUCKET_NEXT (dptr);
                apr_bucket_delete(free_bucket);
            } while (dptr != APR_BRIGADE_SENTINEL(*bb));
        }
        else { /* Otherwise pass it along... */
            ap_pass_brigade(f->next, *bb);  /* No SSI tags in this brigade... */
            ctx->bytes_parsed = 0;
        }
    }
    else if (ctx->state == PARSED) {     /* Invalid internal condition... */
        apr_bucket *content_head = NULL, *tmp_bkt;
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, r,
                      "Invalid mod_include state during file %s", r->filename);
        CREATE_ERROR_BUCKET(ctx, tmp_bkt, APR_BRIGADE_FIRST(*bb), content_head);
    }
    else {                 /* Entire brigade is middle chunk of SSI tag... */
        if (!APR_BRIGADE_EMPTY(ctx->ssi_tag_brigade)) {
            APR_BRIGADE_CONCAT(ctx->ssi_tag_brigade, *bb);
        }
        else {             /* End of brigade contains part of SSI tag... */
            if (ctx->head_start_index > 0) {
                apr_bucket_split(ctx->head_start_bucket, ctx->head_start_index);
                ctx->head_start_bucket = APR_BUCKET_NEXT(ctx->head_start_bucket);
                ctx->head_start_index  = 0;
            }
                           /* Set aside tag, pass pre-tag... */
            tag_and_after = apr_brigade_split(*bb, ctx->head_start_bucket);
            ap_save_brigade(f, &ctx->ssi_tag_brigade, &tag_and_after);
            ap_pass_brigade(f->next, *bb);
            ctx->bytes_parsed = 0;
        }
    }
}

/*****************************************************************
 *
 * XBITHACK.  Sigh...  NB it's configurable per-directory; the compile-time
 * option only changes the default.
 */

module include_module;
enum xbithack {
    xbithack_off, xbithack_on, xbithack_full
};

typedef struct {
    char *default_error_msg;
    char *default_time_fmt;
    enum xbithack *xbithack;
} include_dir_config;

#ifdef XBITHACK
#define DEFAULT_XBITHACK xbithack_full
#else
#define DEFAULT_XBITHACK xbithack_off
#endif

static void *create_includes_dir_config(apr_pool_t *p, char *dummy)
{
    include_dir_config *result =
        (include_dir_config *)apr_palloc(p, sizeof(include_dir_config));
    enum xbithack *xbh = (enum xbithack *) apr_palloc(p, sizeof(enum xbithack));
    *xbh = DEFAULT_XBITHACK;
    result->default_error_msg = DEFAULT_ERROR_MSG;
    result->default_time_fmt = DEFAULT_TIME_FORMAT;
    result->xbithack = xbh;
    return result;
    return result;
}

static const char *set_xbithack(cmd_parms *cmd, void *xbp, const char *arg)
{
    include_dir_config *conf = (include_dir_config *)xbp;

    if (!strcasecmp(arg, "off")) {
        *conf->xbithack = xbithack_off;
    }
    else if (!strcasecmp(arg, "on")) {
        *conf->xbithack = xbithack_on;
    }
    else if (!strcasecmp(arg, "full")) {
        *conf->xbithack = xbithack_full;
    }
    else {
        return "XBitHack must be set to Off, On, or Full";
    }

    return NULL;
}

static int includes_filter(ap_filter_t *f, apr_bucket_brigade *b)
{
    request_rec *r = f->r;
    include_ctx_t *ctx = f->ctx;
    request_rec *parent;
    include_dir_config *conf = 
                   (include_dir_config *)ap_get_module_config(r->per_dir_config,
                                                              &include_module);

    if (!(ap_allow_options(r) & OPT_INCLUDES)) {
        return ap_pass_brigade(f->next, b);
    }
    r->allowed |= (1 << M_GET);
    if (r->method_number != M_GET) {
        return ap_pass_brigade(f->next, b);
    }

    if (!f->ctx) {
        f->ctx    = ctx      = apr_pcalloc(f->c->pool, sizeof(*ctx));
        if (ctx != NULL) {
            ctx->state           = PRE_HEAD;
            ctx->flags           = (FLAG_PRINTING | FLAG_COND_TRUE);
            if (ap_allow_options(r) & OPT_INCNOEXEC) {
                ctx->flags |= FLAG_NO_EXEC;
            }
            ctx->ssi_tag_brigade = apr_brigade_create(f->c->pool);

            apr_cpystrn(ctx->error_str, conf->default_error_msg, sizeof(ctx->error_str));
            apr_cpystrn(ctx->time_str, conf->default_time_fmt, sizeof(ctx->time_str));
            ctx->error_length = strlen(ctx->error_str);
        }
        else {
            ap_pass_brigade(f->next, b);
            return APR_ENOMEM;
        }
    }
    else {
        ctx->bytes_parsed = 0;
    }

    /* Assure the platform supports Group protections */
    if ((*conf->xbithack == xbithack_full)
        && (r->finfo.valid & APR_FINFO_GPROT)
        && (r->finfo.protection & APR_GEXECUTE)) {
        ap_update_mtime(r, r->finfo.mtime);
        ap_set_last_modified(r);
    }

    if ((parent = ap_get_module_config(r->request_config, &include_module))) {
	/* Kludge --- for nested includes, we want to keep the subprocess
	 * environment of the base document (for compatibility); that means
	 * torquing our own last_modified date as well so that the
	 * LAST_MODIFIED variable gets reset to the proper value if the
	 * nested document resets <!--#config timefmt-->.
	 * We also insist that the memory for this subrequest not be
	 * destroyed, that's dealt with in handle_include().
	 */
        r->subprocess_env = r->main->subprocess_env;
        apr_pool_join(r->main->pool, r->pool);
        r->finfo.mtime = r->main->finfo.mtime;
    }
    else {
	/* we're not a nested include, so we create an initial
	 * environment */
        ap_add_common_vars(r);
        ap_add_cgi_vars(r);
        add_include_vars(r, conf->default_time_fmt);
    }
    /* XXX: this is bogus, at some point we're going to do a subrequest,
     * and when we do it we're going to be subjecting code that doesn't
     * expect to be signal-ready to SIGALRM.  There is no clean way to
     * fix this, except to put alarm support into BUFF. -djg
     */


    send_parsed_content(&b, r, f);

    if (parent) {
	/* signify that the sub request should not be killed */
	ap_set_module_config(r->request_config, &include_module,
	    NESTED_INCLUDE_MAGIC);
    }

    return OK;
}

static void ap_register_include_handler(char *tag, include_handler func)
{
    apr_hash_set(include_hash, tag, strlen(tag) + 1, (const void *)func);
}

static void include_post_config(apr_pool_t *p, apr_pool_t *plog,
                                apr_pool_t *ptemp, server_rec *s)
{
    include_hash = apr_hash_make(p);

    ssi_pfn_register = APR_RETRIEVE_OPTIONAL_FN(ap_register_include_handler);

    if(ssi_pfn_register) {
        ssi_pfn_register("if", handle_if);
        ssi_pfn_register("set", handle_set);
        ssi_pfn_register("else", handle_else);
        ssi_pfn_register("elif", handle_elif);
        ssi_pfn_register("echo", handle_echo);
        ssi_pfn_register("endif", handle_endif);
        ssi_pfn_register("fsize", handle_fsize);
        ssi_pfn_register("config", handle_config);
        ssi_pfn_register("include", handle_include);
        ssi_pfn_register("flastmod", handle_flastmod);
        ssi_pfn_register("printenv", handle_printenv);
    }
}

static const char *set_default_error_msg(cmd_parms *cmd, void *mconfig, const char *msg)
{
    include_dir_config *conf = (include_dir_config *)mconfig;
    conf->default_error_msg = apr_pstrdup(cmd->pool, msg);
    return NULL;
}

static const char *set_default_time_fmt(cmd_parms *cmd, void *mconfig, const char *fmt)
{
    include_dir_config *conf = (include_dir_config *)mconfig;
    conf->default_time_fmt = apr_pstrdup(cmd->pool, fmt);
    return NULL;
}

/*
 * Module definition and configuration data structs...
 */
static const command_rec includes_cmds[] =
{
    AP_INIT_TAKE1("XBitHack", set_xbithack, NULL, OR_OPTIONS, 
                  "Off, On, or Full"),
    AP_INIT_TAKE1("SSIErrorMsg", set_default_error_msg, NULL, OR_ALL, 
                  "a string"),
    AP_INIT_TAKE1("SSITimeFormat", set_default_time_fmt, NULL, OR_ALL,
                  "a strftime(3) formatted string"),
    {NULL}
};

static void register_hooks(apr_pool_t *p)
{
    APR_REGISTER_OPTIONAL_FN(ap_ssi_get_tag_and_value);
    APR_REGISTER_OPTIONAL_FN(ap_ssi_parse_string);
    APR_REGISTER_OPTIONAL_FN(ap_register_include_handler);
    ap_hook_post_config(include_post_config, NULL, NULL, APR_HOOK_REALLY_FIRST);
    ap_register_output_filter("INCLUDES", includes_filter, AP_FTYPE_CONTENT);
}

module AP_MODULE_DECLARE_DATA include_module =
{
    STANDARD20_MODULE_STUFF,
    create_includes_dir_config, /* dir config creater */
    NULL,                       /* dir merger --- default is to override */
    NULL,                       /* server config */
    NULL,                       /* merge server config */
    includes_cmds,              /* command apr_table_t */
    register_hooks		/* register hooks */
};

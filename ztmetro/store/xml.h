// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: the little XML reading AppStream needs (appstream.c), shared with
 * Flathub's catalogue (flathub.c). Not a general parser: enough for AppStream. */
#ifndef ZTSTORE_XML_H
#define ZTSTORE_XML_H

#include <stdbool.h>
#include <stddef.h>

/* text with its entities replaced and white space run together, on one line */
char *xml_unescape(const char *s, size_t n);
/* the value of attribute `name` inside the tag text [tag, tag_end) */
bool xml_attr(const char *tag, const char *tag_end, const char *name, char *out, size_t size);
/* the next element called `name` in [p, end): where its tag starts (the
 * return), where the tag ends, and its text */
const char *xml_element(const char *p, const char *end, const char *name, const char **tag_end,
	const char **text, const char **text_end);
/* a gzipped (or plain) file's whole text, NUL-terminated */
char *gunzip_file(const char *path);

/* AppStream's description markup (<p>, <ul>/<ol> of <li>, <em>, <code>) in
 * [s, e) as plain text: paragraphs apart by a blank line, list items as
 * "• item" or "1. item" lines. Translated paragraphs (xml:lang) are left out. */
char *xml_markup_text(const char *s, const char *e);

#endif

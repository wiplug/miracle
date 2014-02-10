/*
 * MiracleCast - Wifi-Display/Miracast Implementation
 *
 * Copyright (c) 2013-2014 David Herrmann <dh.herrmann@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#define LOG_SUBSYSTEM "link"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <systemd/sd-bus.h>
#include "miracled.h"
#include "miracled-wifi.h"
#include "shl_dlist.h"
#include "shl_log.h"
#include "shl_util.h"

static const char *link_type_to_str_table[LINK_CNT] = {
	[LINK_VIRTUAL] = "virtual",
	[LINK_WIFI] = "wifi",
};

const char *link_type_to_str(unsigned int type)
{
	if (type >= LINK_CNT)
		return NULL;

	return link_type_to_str_table[type];
}

unsigned int link_type_from_str(const char *str)
{
	unsigned int i;

	if (!str)
		return LINK_CNT;

	for (i = 0; i < LINK_CNT; ++i)
		if (link_type_to_str_table[i] &&
		    !strcmp(link_type_to_str_table[i], str))
			return i;

	return LINK_CNT;
}

int link_make_name(unsigned int type, const char *interface, char **out)
{
	const char *tname;
	char *name, *res;
	size_t tlen, ilen;

	tname = link_type_to_str(type);
	if (!tname || !interface)
		return -EINVAL;

	/* hard-coded maximum of 255 just to be safe */
	tlen = strlen(tname);
	ilen = strlen(interface);
	if (!tlen || tlen > 255 || !ilen || ilen > 255)
		return -EINVAL;

	if (!out)
		return 0;

	name = shl_strjoin(tname, ":", interface, NULL);
	if (!name)
		return log_ENOMEM();

	res = sd_bus_label_escape(name);
	free(name);
	if (!res)
		return log_ENOMEM();

	*out = res;
	return 0;
}

/*
 * Wifi Handling
 */

static void link_wifi_event_fn(struct wifi *w, void *data,
			       struct wifi_event *ev)
{
	struct link *l = data;
	struct peer *p;

	switch (ev->type) {
	case WIFI_HUP:
		/* destroy this link */
		link_free(l);
		break;
	case WIFI_DEV_FOUND:
		peer_new_wifi(l, ev->dev_found.dev, NULL);
		break;
	case WIFI_DEV_LOST:
		p = wifi_dev_get_data(ev->dev_lost.dev);
		if (!p)
			break;

		peer_free(p);
		break;
	case WIFI_DEV_PROVISION:
	case WIFI_DEV_CONNECT:
	case WIFI_DEV_DISCONNECT:
		p = wifi_dev_get_data(ev->dev_lost.dev);
		if (!p)
			break;

		peer_process_wifi(p, ev);
		break;
	default:
		log_debug("unhandled WIFI event: %u", ev->type);
		break;
	}
}

static int link_wifi_init(struct link *l)
{
	struct wifi_dev *d;
	int r;
	char *path;

	r = wifi_new(l->m->event, link_wifi_event_fn, l, &l->w);
	if (r < 0)
		return r;

	path = shl_strcat("/run/wpa_supplicant/", l->interface);
	if (!path)
		return log_ENOMEM();

	r = wifi_open(l->w, path);
	free(path);

	if (r < 0)
		return r;

	r = wifi_set_name(l->w, l->friendly_name);
	if (r < 0)
		return r;

	for (d = wifi_get_devs(l->w); d; d = wifi_dev_next(d))
		peer_new_wifi(l, d, NULL);

	return 0;
}

static void link_wifi_destroy(struct link *l)
{
	wifi_close(l->w);
	wifi_free(l->w);
}

/*
 * Link Handling
 */

int link_new(struct manager *m,
	     unsigned int type,
	     const char *interface,
	     struct link **out)
{
	size_t hash = 0;
	char *name;
	struct link *l;
	int r;

	if (!m)
		return log_EINVAL();

	r = link_make_name(type, interface, &name);
	if (r < 0)
		return r;

	if (shl_htable_lookup_str(&m->links, name, &hash, NULL)) {
		free(name);
		return -EALREADY;
	}

	log_debug("new link: %s", name);

	l = calloc(1, sizeof(*l));
	if (!l) {
		free(name);
		return log_ENOMEM();
	}

	l->m = m;
	l->type = type;
	l->name = name;
	shl_dlist_init(&l->peers);

	l->interface = strdup(interface);
	if (!l->interface) {
		r = log_ENOMEM();
		goto error;
	}

	l->friendly_name = strdup(m->friendly_name);
	if (!l->friendly_name) {
		r = log_ENOMEM();
		goto error;
	}

	switch (l->type) {
	case LINK_VIRTUAL:
		break;
	case LINK_WIFI:
		r = link_wifi_init(l);
		if (r < 0)
			goto error;
		break;
	}

	r = shl_htable_insert_str(&m->links, &l->name, &hash);
	if (r < 0) {
		log_vERR(r);
		goto error;
	}

	++m->link_cnt;
	log_info("new managed link: %s", l->name);

	if (out)
		*out = l;

	return 0;

error:
	link_free(l);
	return r;
}

void link_free(struct link *l)
{
	struct peer *p;

	if (!l)
		return;

	log_debug("free link: %s", l->name);

	while ((p = LINK_FIRST_PEER(l)))
		peer_free(p);

	if (shl_htable_remove_str(&l->m->links, l->name, NULL, NULL)) {
		log_info("remove managed link: %s", l->name);
		--l->m->link_cnt;
	}

	link_wifi_destroy(l);
	free(l->friendly_name);
	free(l->name);
	free(l->interface);
	free(l);
}

int link_set_friendly_name(struct link *l, const char *name)
{
	char *dup;
	int r;

	if (!l || !name)
		return log_EINVAL();

	dup = strdup(name);
	if (!dup)
		return log_ENOMEM();

	r = 0;
	switch (l->type) {
	case LINK_WIFI:
		r = wifi_set_name(l->w, name);
		break;
	}

	if (r < 0) {
		free(dup);
		return r;
	}

	free(l->friendly_name);
	l->friendly_name = dup;

	return 0;
}
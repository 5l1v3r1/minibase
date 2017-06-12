#include <nlusctl.h>
#include <heap.h>
#include <util.h>
#include <format.h>
#include <string.h>
#include <output.h>

#include "common.h"
#include "wictl.h"

static int cmp_int(attr at, attr bt, int key)
{
	int* na = uc_sub_int(at, key);
	int* nb = uc_sub_int(bt, key);

	if(!na && nb)
		return -1;
	if(na && !nb)
		return  1;
	if(!na || !nb)
		return 0;
	if(*na < *nb)
		return -1;
	if(*na > *nb)
		return  1;

	return 0;
}

static int cmp_str(attr at, attr bt, int key)
{
	char* na = uc_sub_str(at, key);
	char* nb = uc_sub_str(bt, key);

	if(!na || !nb)
		return 0;

	return strcmp(na, nb);
}

static int scan_ord(const void* a, const void* b, long p)
{
	attr at = *((attr*)a);
	attr bt = *((attr*)b);
	int ret;

	if((ret = cmp_int(at, bt, ATTR_PRIO)))
		return -ret;
	if((ret = cmp_int(at, bt, ATTR_SIGNAL)))
		return -ret;
	if((ret = cmp_int(at, bt, ATTR_FREQ)))
		return ret;

	return 0;
}

static int link_ord(const void* a, const void* b, long p)
{
	attr at = *((attr*)a);
	attr bt = *((attr*)b);
	int ret;

	if((ret = cmp_int(at, bt, ATTR_TYPE)))
		return -ret;
	if((ret = cmp_str(at, bt, ATTR_NAME)))
		return ret;

	return 0;
}

#define DICTEND -1

static struct dict {
	int val;
	char name[16];
} linkstates[] = {
	{ LINK_OFF,      "off"        },
	{ LINK_ENABLED,  "enabled"    },
	{ LINK_CARRIER,  "carrier"    },
	{ LINK_STARTING, "starting"   },
	{ LINK_STOPPING, "stopping"   },
	{ DICTEND,       ""           }
}, wifimodes[] = {
	{ WIFI_DISABLED, "no-conf"    },
	{ WIFI_ROAMING,  "roaming"    },
	{ WIFI_FIXEDAP,  "fixedap"    },
	{ DICTEND,       ""           }
};

static char* fmt_kv(char* p, char* e, attr at, int key, struct dict* dc)
{
	int* val;
	struct dict* kv;

	if(!(val = uc_sub_int(at, key)))
		return p;

	for(kv = dc; kv->val != DICTEND; kv++)
		if(kv->val == *val)
			break;

	if(kv->val == DICTEND)
		return p;

	p = fmtstr(p, e, " ");
	p = fmtstr(p, e, kv->name);

	return p;
}

static char* fmt_ip(char* p, char* e, attr at)
{
	uint8_t* ip = uc_sub_bin(at, ATTR_IPADDR, 4);
	int* mask = uc_sub_int(at, ATTR_IPMASK);

	if(!ip || !mask) return p;

	p = fmtstr(p, e, " ");
	p = fmtip(p, e, ip);
	p = fmtstr(p, e, "/");
	p = fmtint(p, e, *mask);

	return p;
}

static void dump_link(CTX, AT)
{
	char buf[200];
	char* p = buf;
	char* e = buf + sizeof(buf) - 1;

	char* name = uc_sub_str(at, ATTR_NAME);
	int* ifi = uc_sub_int(at, ATTR_IFI);

	if(!ifi || !name) return;

	p = fmtstr(p, e, "Link");
	p = fmtstr(p, e, " ");
	p = fmtint(p, e, *ifi);
	p = fmtstr(p, e, " ");
	p = fmtstr(p, e, name);
	p = fmt_kv(p, e, at, ATTR_STATE, linkstates);
	p = fmt_ip(p, e, at);
	*p++ = '\n';

	output(ctx, buf, p - buf);
}

static char* fmt_ssid(char* p, char* e, uint8_t* ssid, int slen)
{
	int i;

	for(i = 0; i < slen; i++) {
		if(ssid[i] >= 0x20) {
			p = fmtchar(p, e, ssid[i]);
		} else {
			p = fmtstr(p, e, "\\x");
			p = fmtbyte(p, e, ssid[i]);
		}
	}

	return p;
}

static void dump_scan(CTX, AT)
{
	char buf[200];
	char* p = buf;
	char* e = buf + sizeof(buf) - 1;

	struct ucattr* ssid = uc_sub(at, ATTR_SSID);
	uint8_t* bssid = uc_sub_bin(at, ATTR_BSSID, 6);
	int* freq = uc_sub_int(at, ATTR_FREQ);
	int* signal = uc_sub_int(at, ATTR_SIGNAL);

	p = fmtstr(p, e, "AP ");
	p = fmtint(p, e, (*signal)/100);
	p = fmtstr(p, e, " ");
	p = fmtint(p, e, *freq);
	p = fmtstr(p, e, " ");
	p = fmtmac(p, e, bssid);
	p = fmtstr(p, e, "  ");
	p = fmt_ssid(p, e, uc_payload(ssid), uc_paylen(ssid));
	*p++ = '\n';

	output(ctx, buf, p - buf);
}

static char* fmt_wifi_iface(char* p, char* e, attr at)
{
	char* name;

	if(!(name = uc_sub_str(at, ATTR_NAME)))
		goto out;

	p = fmtstr(p, e, " ");
	p = fmtstr(p, e, name);
out:
	return p;
}

static char* fmt_wifi_freq(char* p, char* e, attr at)
{
	int* freq;

	if(!(freq = uc_sub_int(at, ATTR_FREQ)))
		goto out;

	p = fmtstr(p, e, " @ ");
	p = fmtint(p, e, *freq);
out:
	return p;
}

static char* fmt_wifi_ssid(char* p, char* e, attr at)
{
	attr ssid;

	if(!(ssid = uc_sub(at, ATTR_SSID)))
		goto out;

	p = fmtstr(p, e, " AP ");
	p = fmt_ssid(p, e, uc_payload(ssid), uc_paylen(ssid));
out:
	return p;
}

static char* fmt_wifi_bssid(char* p, char* e, attr at)
{
	uint8_t* bssid;

	if(!(bssid = uc_sub_bin(at, ATTR_BSSID, 6)))
		goto out;

	p = fmtstr(p, e, " ");
	p = fmtmac(p, e, bssid);
out:
	return p;
}

static void dump_wifi(CTX, MSG)
{
	char buf[200];
	char* p = buf;
	char* e = buf + sizeof(buf) - 1;
	attr at;

	if(!(at = uc_get(msg, ATTR_WIFI)))
		return;

	p = fmtstr(p, e, "WiFi");
	p = fmt_wifi_iface(p, e, at);
	p = fmt_kv(p, e, at, ATTR_MODE, wifimodes);
	p = fmt_wifi_ssid(p, e, at);
	p = fmt_wifi_bssid(p, e, at);
	p = fmt_wifi_freq(p, e, at);
	*p++ = '\n';

	output(ctx, buf, p - buf);
}

static attr* prep_list(CTX, MSG, int key, qcmp cmp)
{
	int n = 0, i = 0;
	attr at;

	for(at = uc_get_0(msg); at; at = uc_get_n(msg, at))
		if(at->key == key)
			n++;

	attr* refs = halloc(&ctx->hp, (n+1)*sizeof(void*));

	for(at = uc_get_0(msg); at && i < n; at = uc_get_n(msg, at))
		if(at->key == key)
			refs[i++] = at;
	refs[i] = NULL;

	qsort(refs, i, sizeof(void*), cmp, 0);

	return refs;
}

static void dump_list(CTX, attr* list, void (*dump)(CTX, AT))
{
	for(attr* ap = list; *ap; ap++)
		dump(ctx, *ap);
}

static void empty_line(CTX)
{
	output(ctx, "\n", 1);
}

void dump_scanlist(CTX, MSG)
{
	attr* scans = prep_list(ctx, msg, ATTR_SCAN, scan_ord);

	init_output(ctx);
	dump_list(ctx, scans, dump_scan);
	fini_output(ctx);
}

void dump_status(CTX, MSG)
{
	attr* scans = prep_list(ctx, msg, ATTR_SCAN, scan_ord);
	attr* links = prep_list(ctx, msg, ATTR_LINK, link_ord);

	init_output(ctx);

	dump_list(ctx, links, dump_link);
	dump_wifi(ctx, msg);

	if(*scans) empty_line(ctx);

	dump_list(ctx, scans, dump_scan);

	fini_output(ctx);
}

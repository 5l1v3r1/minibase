#include <netlink.h>
#include <netlink/attr.h>
#include <cdefs.h>

int nl_paylen(struct nlattr* at)
{
	return at->len - sizeof(*at);
}

void* nl_payload(struct nlattr* at)
{
	return at->payload;
}

struct nlattr* nl_get(struct nlgen* msg, uint16_t type)
{
	return nl_attr_k_in(NLPAYLOAD(msg), type);
}

struct nlattr* nl_get_0(struct nlgen* msg)
{
	return nl_attr_0_in(NLPAYLOAD(msg));
}

struct nlattr* nl_get_n(struct nlgen* msg, struct nlattr* cur)
{
	return nl_attr_n_in(NLPAYLOAD(msg), cur);
}

void* nl_get_of_len(struct nlgen* msg, uint16_t type, size_t len)
{
	struct nlattr* at = nl_get(msg, type);
	if(!at) return NULL;
	return (at->len == sizeof(*at) + len) ? at->payload : NULL;
}
struct nlattr* nl_get_nest(struct nlgen* msg, uint16_t type)
{
	return nl_nest(nl_get(msg, type));
}

char* nl_get_str(struct nlgen* msg, uint16_t type)
{
	return nl_str(nl_get(msg, type));
}

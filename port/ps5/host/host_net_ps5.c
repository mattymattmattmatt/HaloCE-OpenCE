/*
HOST_NET_PS5.C

What the console has for the game's network where posix_net.c (compiled
for the PS5 with its HALO_PS5 branches, port/ps5/PS5.md, phase 5) finds
nothing in libc:

- the game's addresses turned round between Winsock's layout and BSD's on
  their way through posix_net.c (the link wraps its calls);
- names looked up through libSceNet's resolver (the platform layer's
  getaddrinfo refuses every lookup): ps5_net_resolve_ipv4, for
  posix_resolve_ipv4;
- at the start, internet play's brokers.txt written beside config.toml,
  this machine's address (posix_net.c's) logged beside the console's own
  word on it (sceNetCtlGetInfo), and the DTLS certificate made and its
  hash logged;
- Mbed TLS's entropy (DTLS with browsers, posix_dtls.c), from
  arc4random_buf (mbedtls_config_ps5.h), and the lookup its net_sockets.c
  connects with (the updater's HTTPS, posix_update.c): getaddrinfo's
  answer for one IPv4 address, through the resolver, which
  tools/ps5_build.py gives that file as ps5_tls_getaddrinfo;
- the step 1 smoke test (HALO_PS5_NET_TEST=1 in environment.txt), run by
  host_main.c before the game's thread: the console's sockets, its
  resolver and its random numbers tried one by one in a title, each result
  logged with its errno on a line of its own, prefixed "net test:".

The console's calls are declared by hand, as the SDK has no header for
them, after OpenOrbis's PS4 headers (OpenOrbis-PS4-Toolchain,
include/orbis/Net.h and include/orbis/NetCtl.h, and _types/net.h), which
are the public source for their signatures; the PS5's modules export the
same names (the SDK's libSceNet.so and libSceNetCtl.so stubs).
*/

#include "host.h"
#include "ps5_host.h"
#include "posix.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------- the console's calls (OpenOrbis's Net.h and NetCtl.h) */

int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceNetResolverCreate(const char *name, int pool, int flags);
int sceNetResolverDestroy(int resolver);
/* the address in network byte order; the timeout in microseconds (0 is
the library's own), retry the attempts after the first */
int sceNetResolverStartNtoa(int resolver, const char *name, struct in_addr *address, int timeout, int retry,
	int flags);
int sceNetCtlInit(void);
int sceNetCtlGetInfo(int code, void *info);

/* OpenOrbis's ORBIS_NET_CTL_INFO_IP_ADDRESS: the address as text, in a
union (OrbisNetCtlInfo) of which the largest member is 256 bytes */
#define NET_CTL_INFO_IP_ADDRESS 14
union net_ctl_info
{
	char ip_address[16];
	unsigned char bytes[512];
};

/* the pool the resolvers are made in: 16 KB holds a few lookups at once
(the brokers' and STUN's names) */
#define RESOLVER_POOL_SIZE (16 * 1024)
#define RESOLVE_TIMEOUT_US 5000000
#define RESOLVE_RETRIES 1

static const char *ipv4_text(uint32_t address, char *text);

/* ---------- bringing the network up */

static pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;
static int net_initialized;
static int net_init_result;
static int resolver_pool = -1;

/* sceNetInit once; its result */
static int net_init(void)
{
	pthread_mutex_lock(&net_lock);
	if (!net_initialized)
	{
		net_init_result = sceNetInit();
		net_initialized = 1;
	}
	pthread_mutex_unlock(&net_lock);
	return net_init_result;
}

/* the resolvers' pool, made once (after sceNetInit, which the library may
need first); -1 if it cannot be made, said once */
static int pool_get(void)
{
	static int said;
	int pool;

	pthread_mutex_lock(&net_lock);
	if (resolver_pool < 0)
	{
		if (!net_initialized)
		{
			net_init_result = sceNetInit();
			net_initialized = 1;
		}
		resolver_pool = sceNetPoolCreate("halo_resolver", RESOLVER_POOL_SIZE, 0);
		if (resolver_pool < 0 && !said)
		{
			said = 1;
			ps5_log("network: the resolver's pool cannot be made (sceNetPoolCreate 0x%08x, after sceNetInit "
				"0x%08x): names are not looked up", (unsigned)resolver_pool, (unsigned)net_init_result);
		}
	}
	pool = resolver_pool;
	pthread_mutex_unlock(&net_lock);
	return pool;
}

/* ---------- name lookups */

/* the names whose failure was logged, so each is said once */
#define FAILED_NAMES 16
static char failed_names[FAILED_NAMES][64];
static int failed_count;

static void note_failure(const char *name, const char *what, int code)
{
	int index;

	pthread_mutex_lock(&net_lock);
	for (index = 0; index < failed_count; index++)
	{
		if (!strncmp(failed_names[index], name, sizeof(failed_names[index]) - 1))
		{
			pthread_mutex_unlock(&net_lock);
			return;
		}
	}
	if (failed_count < FAILED_NAMES)
		snprintf(failed_names[failed_count++], sizeof(failed_names[0]), "%s", name);
	pthread_mutex_unlock(&net_lock);
	ps5_log("network: %s cannot be looked up: %s 0x%08x", name, what, (unsigned)code);
}

/* One resolver for each lookup, in the one pool: the game looks names up
from several threads at once (p2p_signal.c opens every broker together),
and a resolver is the library's unit of a lookup under way, so none waits
for another's timeout. */
static uint32_t resolve(const char *name, int *code, double *milliseconds)
{
	struct in_addr address;
	uint64_t start = ps5_now_ns();
	int pool = pool_get();
	int resolver, result;

	address.s_addr = 0;
	*code = 0;
	if (pool < 0)
	{
		*code = pool;
		*milliseconds = 0;
		return 0;
	}
	resolver = sceNetResolverCreate("halo_resolve", pool, 0);
	if (resolver < 0)
	{
		*code = resolver;
		*milliseconds = (ps5_now_ns() - start) / 1e6;
		return 0;
	}
	result = sceNetResolverStartNtoa(resolver, name, &address, RESOLVE_TIMEOUT_US, RESOLVE_RETRIES, 0);
	sceNetResolverDestroy(resolver);
	*milliseconds = (ps5_now_ns() - start) / 1e6;
	if (result < 0)
	{
		*code = result;
		return 0;
	}
	return address.s_addr;
}

/* the IPv4 address of a name (network byte order), or 0; blocks while it
is looked up (5 s and one retry at most) */
uint32_t ps5_net_resolve_ipv4(const char *name)
{
	double milliseconds;
	int code;
	uint32_t address = resolve(name, &code, &milliseconds);

	if (!address)
		note_failure(name, code ? "sceNetResolverStartNtoa" : "no address", code);
	return address;
}

/* ---------- the game's addresses (posix_net.c's calls, wrapped)

The game's are Winsock's (and Linux's, which posix_net.c was written for):
a 16-bit family, then the port and the address. The console's are BSD's:
an 8-bit length and an 8-bit family, then the port and the address at the
same offsets. Passed as they are, the console reads the family as 0 and
refuses the address (internet play's tunnel did not open: "cannot open its
socket"), as the Switch found (port/switch/host/host_net.c). So the link
wraps posix_net.c's seven calls that take or give an address
(tools/ps5_build.py, HOST_WRAPS), and each address is turned round here on
the way in and the way out, posix_net.c unchanged. */

int __real_posix_socket_bind(int socket, const void *address, int address_length);
int __real_posix_socket_connect(int socket, const void *address, int address_length);
int __real_posix_socket_accept(int socket, void *address, int *address_length);
int __real_posix_socket_sendto(int socket, const void *buffer, int length, int flags, const void *address,
	int address_length);
int __real_posix_socket_recvfrom(int socket, void *buffer, int length, int flags, void *address,
	int *address_length);
int __real_posix_socket_getsockname(int socket, void *address, int *address_length);
int __real_posix_socket_getpeername(int socket, void *address, int *address_length);
int __wrap_posix_socket_bind(int socket, const void *address, int address_length);
int __wrap_posix_socket_connect(int socket, const void *address, int address_length);
int __wrap_posix_socket_accept(int socket, void *address, int *address_length);
int __wrap_posix_socket_sendto(int socket, const void *buffer, int length, int flags, const void *address,
	int address_length);
int __wrap_posix_socket_recvfrom(int socket, void *buffer, int length, int flags, void *address,
	int *address_length);
int __wrap_posix_socket_getsockname(int socket, void *address, int *address_length);
int __wrap_posix_socket_getpeername(int socket, void *address, int *address_length);

/* the game's address as the console's; its length (NULL stays NULL) */
static const void *address_in(const void *address, int length, struct sockaddr_storage *converted,
	int *converted_length)
{
	const unsigned char *bytes = address;

	*converted_length = length;
	if (!address || length < 2)
		return address;
	if ((size_t)length > sizeof(*converted))
		length = (int)sizeof(*converted);
	memset(converted, 0, sizeof(*converted));
	memcpy(converted, address, (size_t)length);
	converted->ss_len = (unsigned char)length;
	converted->ss_family = (sa_family_t)(bytes[0] | (bytes[1] << 8));
	*converted_length = length;
	return converted;
}

/* the console's address as the game's, into what the game gave (its
length the whole address's, as the call's own) */
static void address_out(const struct sockaddr_storage *converted, int converted_length, void *address,
	int *length)
{
	unsigned char *bytes = address;
	int size;

	if (!address || !length)
		return;
	size = converted_length < *length ? converted_length : *length;
	if (size > 0)
		memcpy(address, converted, (size_t)size);
	if (size >= 2)
	{
		bytes[0] = (unsigned char)converted->ss_family;
		bytes[1] = 0;
	}
	*length = converted_length;
}

int __wrap_posix_socket_bind(int socket, const void *address, int address_length)
{
	struct sockaddr_storage converted;
	int length;
	const void *host = address_in(address, address_length, &converted, &length);

	return __real_posix_socket_bind(socket, host, length);
}

int __wrap_posix_socket_connect(int socket, const void *address, int address_length)
{
	struct sockaddr_storage converted;
	int length;
	const void *host = address_in(address, address_length, &converted, &length);

	return __real_posix_socket_connect(socket, host, length);
}

int __wrap_posix_socket_sendto(int socket, const void *buffer, int length, int flags, const void *address,
	int address_length)
{
	struct sockaddr_storage converted;
	int converted_length;
	const void *host = address_in(address, address_length, &converted, &converted_length);

	return __real_posix_socket_sendto(socket, buffer, length, flags, host, converted_length);
}

/* (the calls that give an address: into the console's form, then turned
round into the game's) */
#define ADDRESS_OUT_CALL(call)                                                         \
	struct sockaddr_storage converted;                                                  \
	int converted_length = (int)sizeof(converted);                                      \
	int result = (call);                                                                \
                                                                                        \
	if (result >= 0)                                                                    \
		address_out(&converted, converted_length, address, address_length);            \
	return result

int __wrap_posix_socket_accept(int socket, void *address, int *address_length)
{
	ADDRESS_OUT_CALL(__real_posix_socket_accept(socket, address && address_length ? &converted : NULL,
		address && address_length ? &converted_length : NULL));
}

int __wrap_posix_socket_recvfrom(int socket, void *buffer, int length, int flags, void *address,
	int *address_length)
{
	ADDRESS_OUT_CALL(__real_posix_socket_recvfrom(socket, buffer, length, flags,
		address && address_length ? &converted : NULL, address && address_length ? &converted_length : NULL));
}

int __wrap_posix_socket_getsockname(int socket, void *address, int *address_length)
{
	ADDRESS_OUT_CALL(__real_posix_socket_getsockname(socket, &converted, &converted_length));
}

int __wrap_posix_socket_getpeername(int socket, void *address, int *address_length)
{
	ADDRESS_OUT_CALL(__real_posix_socket_getpeername(socket, &converted, &converted_length));
}

/* ---------- what miniupnpc calls (tools/ps5_build.py compiles it with
NO_GETADDRINFO, and these names for its gethostbyname, herror,
if_nametoindex, if_indextoname, getnameinfo and gai_strerror, which no
module a title loads exports) */

struct hostent *ps5_upnp_gethostbyname(const char *name);
void ps5_upnp_herror(const char *text);
unsigned int ps5_upnp_if_nametoindex(const char *name);
char *ps5_upnp_if_indextoname(unsigned int index, char *name);
int ps5_upnp_getnameinfo(const struct sockaddr *address, socklen_t length, char *host, size_t host_size,
	char *service, size_t service_size, int flags);
const char *ps5_upnp_gai_strerror(int error);

/* the router's address, which miniupnpc has from its description's URL: a
dotted quad, or a name for the resolver; one thread asks at a time
(posix_upnp.c), so the answer may be static */
struct hostent *ps5_upnp_gethostbyname(const char *name)
{
	static struct in_addr address;
	static char *addresses[2] = { (char *)&address, NULL };
	static char *aliases[1] = { NULL };
	static char host_name[256];
	static struct hostent entry;

	if (inet_pton(AF_INET, name, &address) != 1)
	{
		address.s_addr = ps5_net_resolve_ipv4(name);
		if (!address.s_addr)
			return NULL;
	}
	snprintf(host_name, sizeof(host_name), "%s", name);
	entry.h_name = host_name;
	entry.h_aliases = aliases;
	entry.h_addrtype = AF_INET;
	entry.h_length = sizeof(address);
	entry.h_addr_list = addresses;
	return &entry;
}

void ps5_upnp_herror(const char *text)
{
	ps5_log("UPnP: %s: no address", text);
}

/* (miniupnpc names the multicast interface by address here, never by name) */
unsigned int ps5_upnp_if_nametoindex(const char *name)
{
	(void)name;
	return 0;
}

/* the address this machine reaches the router from, as text
(MINIUPNPC_GET_SRC_ADDR: the forwarding's internal client); numeric only,
as miniupnpc asks (NI_NUMERICHOST | NI_NUMERICSERV) */
int ps5_upnp_getnameinfo(const struct sockaddr *address, socklen_t length, char *host, size_t host_size,
	char *service, size_t service_size, int flags)
{
	const struct sockaddr_in *in = (const struct sockaddr_in *)address;

	(void)flags;
	if (!address || length < (socklen_t)sizeof(*in) || address->sa_family != AF_INET)
		return EAI_FAMILY;
	if (host && host_size && !inet_ntop(AF_INET, &in->sin_addr, host, (socklen_t)host_size))
		return EAI_OVERFLOW;
	if (service && service_size)
		snprintf(service, service_size, "%u", ntohs(in->sin_port));
	return 0;
}

const char *ps5_upnp_gai_strerror(int error)
{
	return error == EAI_FAMILY ? "not an IPv4 address" : error == EAI_OVERFLOW ? "no room" : "failed";
}

/* (an IPv6 router's scope; the search here is IPv4's) */
char *ps5_upnp_if_indextoname(unsigned int index, char *name)
{
	(void)index;
	(void)name;
	return NULL;
}

/* ---------- the console's own address */

/* sceNetCtlGetInfo's address as text, or why not, into text */
static void net_ctl_address(int code, char *text, size_t size)
{
	static int initialized;
	static int init_result;
	union net_ctl_info info;
	int result;

	if (!initialized)
	{
		init_result = sceNetCtlInit();
		initialized = 1;
	}
	memset(&info, 0, sizeof(info));
	result = sceNetCtlGetInfo(code, &info);
	if (result < 0)
		snprintf(text, size, "sceNetCtlGetInfo(%d) 0x%08x (sceNetCtlInit 0x%08x)", code, (unsigned)result,
			(unsigned)init_result);
	else
		snprintf(text, size, "%.15s (sceNetCtlGetInfo(%d) %d, sceNetCtlInit 0x%08x)", info.ip_address, code,
			result, (unsigned)init_result);
}

/* ---------- at the title's start */

/* internet play's brokers (network.brokers_file), which the game reads
beside config.toml: the title's copy (port/assets/network/brokers.txt,
tools/ps5_build.py) written there at every start, as Android's app does, so
an update's list replaces the old one */
static void copy_brokers(void)
{
	char text[8192];
	ssize_t length;
	int in = open(PS5_TITLE_FOLDER "/brokers.txt", O_RDONLY), out;

	if (in < 0)
	{
		ps5_log("network: " PS5_TITLE_FOLDER "/brokers.txt: %s", strerror(errno));
		return;
	}
	length = read(in, text, sizeof(text));
	close(in);
	out = open(PS5_DATA_FOLDER "/brokers.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (length <= 0 || out < 0 || write(out, text, (size_t)length) != length)
		ps5_log("network: " PS5_DATA_FOLDER "/brokers.txt cannot be written: %s", strerror(errno));
	if (out >= 0)
	{
		close(out);
		chmod(PS5_DATA_FOLDER "/brokers.txt", 0666);
	}
}

void ps5_net_start(void)
{
	char route[INET_ADDRSTRLEN], ctl[160], hex[17];
	unsigned char fingerprint[32];
	uint64_t start;
	int index, made;

	copy_brokers();
	/* the address the game takes for this machine's (posix_net.c), and the
	console's own word on it */
	ipv4_text(posix_local_ipv4_address(), route);
	net_ctl_address(NET_CTL_INFO_IP_ADDRESS, ctl, sizeof(ctl));
	ps5_log("network: this machine's address %s (posix_local_ipv4_address); the console's: %s", route, ctl);
	/* internet play's certificate for browsers (posix_dtls.c, Mbed TLS on
	the console's random numbers), made now rather than at the first
	invite, so the log shows it works */
	start = ps5_now_ns();
	made = posix_dtls_fingerprint(fingerprint);
	for (index = 0; index < 8; index++)
		snprintf(hex + index * 2, 3, "%02x", fingerprint[index]);
	if (made)
		ps5_log("network: DTLS certificate made in %.1f ms, SHA-256 %s...", (ps5_now_ns() - start) / 1e6, hex);
	else
		ps5_log("network: DTLS certificate NOT made (posix_dtls_fingerprint, %.1f ms): browsers cannot join",
			(ps5_now_ns() - start) / 1e6);
}

/* ---------- Mbed TLS's lookup (net_sockets.c's mbedtls_net_connect)

The platform layer's getaddrinfo refuses every name, so Mbed TLS's
net_sockets.c is compiled with these in place of getaddrinfo and
freeaddrinfo (tools/ps5_build.py): one IPv4 address, a numeric one as it is
and a name through the resolver, the port a number, in one block that
freeaddrinfo frees. */

int ps5_tls_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result);
void ps5_tls_freeaddrinfo(struct addrinfo *list);

int ps5_tls_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result)
{
	struct
	{
		struct addrinfo info;
		struct sockaddr_in address;
	} *block;
	struct in_addr address;
	char *end;
	long port = service ? strtol(service, &end, 10) : 0;

	*result = NULL;
	if (!node || (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET))
		return EAI_FAMILY;
	if (service && (*end || port < 0 || port > 65535))
		return EAI_SERVICE;
	if (inet_pton(AF_INET, node, &address) != 1)
	{
		address.s_addr = ps5_net_resolve_ipv4(node);
		if (!address.s_addr)
			return EAI_NONAME;
	}
	block = calloc(1, sizeof(*block));
	if (!block)
		return EAI_MEMORY;
	block->address.sin_len = sizeof(block->address);
	block->address.sin_family = AF_INET;
	block->address.sin_port = htons((uint16_t)port);
	block->address.sin_addr = address;
	block->info.ai_family = AF_INET;
	block->info.ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
	block->info.ai_protocol = hints ? hints->ai_protocol : 0;
	block->info.ai_addrlen = sizeof(block->address);
	block->info.ai_addr = (struct sockaddr *)&block->address;
	*result = &block->info;
	return 0;
}

void ps5_tls_freeaddrinfo(struct addrinfo *list)
{
	/* (the address is in the same block, after it) */
	free(list);
}

/* ---------- Mbed TLS's entropy (mbedtls_config_ps5.h) */

/* its hardware source's signature (mbedtls/entropy.h, with
MBEDTLS_ENTROPY_HARDWARE_ALT): the console's random numbers, all asked for */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t length, size_t *written);

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t length, size_t *written)
{
	(void)data;
	arc4random_buf(output, length);
	*written = length;
	return 0;
}

/* ---------- the step 1 smoke test (port/ps5/PS5.md, phase 5) */

#define TEST_BROKERS PS5_DATA_FOLDER "/brokers.txt"

static const char *error_text(int error)
{
	static __thread char text[96];

	snprintf(text, sizeof(text), "errno %d (%s)", error, strerror(error));
	return text;
}

static const char *ipv4_text(uint32_t address, char *text)
{
	struct in_addr in;

	in.s_addr = address;
	if (!inet_ntop(AF_INET, &in, text, INET_ADDRSTRLEN))
		snprintf(text, INET_ADDRSTRLEN, "?");
	return text;
}

/* the first broker's host and port of brokers.txt (the game's own list,
port/assets/network/brokers.txt, beside config.toml once the game has
written it), else the shipped list's first */
static void first_broker(char *host, size_t size, int *port)
{
	char text[2048];
	char *line, *next;
	ssize_t length = -1;
	int fd = open(TEST_BROKERS, O_RDONLY);

	snprintf(host, size, "opence.milenko.org");
	*port = 1883;
	if (fd >= 0)
	{
		length = read(fd, text, sizeof(text) - 1);
		close(fd);
	}
	if (length <= 0)
		return;
	text[length] = 0;
	for (line = text; line && *line; line = next)
	{
		char *colon;

		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		line[strcspn(line, "\r#")] = 0;
		line += strspn(line, " \t");
		if (!*line)
			continue;
		colon = strrchr(line, ':');
		if (colon)
		{
			*colon = 0;
			*port = atoi(colon + 1);
		}
		snprintf(host, size, "%s", line);
		return;
	}
}

static void test_socket(int *datagram)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int error = errno;

	if (fd >= 0)
	{
		ps5_log("net test: 1. socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC) without sceNetInit: %d; sceNetInit is "
			"not needed first", fd);
		*datagram = fd;
		return;
	}
	ps5_log("net test: 1. socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC) without sceNetInit: -1, %s", error_text(error));
	ps5_log("net test: 1. sceNetInit: 0x%08x", (unsigned)net_init());
	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	error = errno;
	if (fd >= 0)
		ps5_log("net test: 1. socket after sceNetInit: %d; sceNetInit is needed first", fd);
	else
		ps5_log("net test: 1. socket after sceNetInit: -1, %s; a title cannot open a socket", error_text(error));
	*datagram = fd;
}

static uint32_t test_route(void)
{
	struct sockaddr_in route;
	socklen_t length = sizeof(route);
	char address[INET_ADDRSTRLEN], ctl[160];
	uint32_t result = 0;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
	{
		ps5_log("net test: 2. the route probe's socket: -1, %s", error_text(errno));
	}
	else
	{
		memset(&route, 0, sizeof(route));
		route.sin_family = AF_INET;
		route.sin_port = htons(53);
		route.sin_addr.s_addr = htonl(0x08080808);
		if (connect(fd, (struct sockaddr *)&route, sizeof(route)) != 0)
			ps5_log("net test: 2. the route probe's connect to 8.8.8.8:53: %s", error_text(errno));
		else if (getsockname(fd, (struct sockaddr *)&route, &length) != 0)
			ps5_log("net test: 2. the route probe's getsockname: %s", error_text(errno));
		else
		{
			result = route.sin_addr.s_addr;
			ps5_log("net test: 2. the route probe (UDP connected to 8.8.8.8:53, getsockname): %s, port %u",
				ipv4_text(result, address), ntohs(route.sin_port));
		}
		close(fd);
	}
	net_ctl_address(NET_CTL_INFO_IP_ADDRESS, ctl, sizeof(ctl));
	ps5_log("net test: 2. sceNetCtlGetInfo's address: %s", ctl);
	/* (PS5_Vulkan's diagnostics.cpp asks with code 0) */
	net_ctl_address(0, ctl, sizeof(ctl));
	ps5_log("net test: 2. sceNetCtlGetInfo with code 0: %s", ctl);
	return result;
}

static void test_broadcast(void)
{
	/* the host's game advertisement goes to the clients' port, 5151
	(NETWORK_GAME_CLIENT_PORT, source/networking/network_game_protocol.h) */
	static const char payload[] = "halo net test";
	struct sockaddr_in address, from;
	socklen_t from_length = sizeof(from);
	struct pollfd wait;
	char buffer[64];
	int value = 1, fd, result;
	socklen_t length = sizeof(address);

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
	{
		ps5_log("net test: 3. socket: %s", error_text(errno));
		return;
	}
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	result = bind(fd, (struct sockaddr *)&address, sizeof(address));
	ps5_log("net test: 3. bind to port 0: %d%s%s", result, result ? ", " : "", result ? error_text(errno) : "");
	if (getsockname(fd, (struct sockaddr *)&address, &length) == 0)
		ps5_log("net test: 3. bound to port %u", ntohs(address.sin_port));
	result = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value));
	ps5_log("net test: 3. SO_BROADCAST: %d%s%s", result, result ? ", " : "", result ? error_text(errno) : "");
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(5151);
	address.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	result = (int)sendto(fd, payload, sizeof(payload), MSG_NOSIGNAL, (struct sockaddr *)&address, sizeof(address));
	ps5_log("net test: 3. sendto 255.255.255.255:5151 (%zu bytes): %d%s%s", sizeof(payload), result,
		result < 0 ? ", " : "", result < 0 ? error_text(errno) : "");
	wait.fd = fd;
	wait.events = POLLIN;
	wait.revents = 0;
	result = poll(&wait, 1, 200);
	if (result > 0)
	{
		result = (int)recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr *)&from, &from_length);
		ps5_log("net test: 3. poll 200 ms: readable; recvfrom: %d bytes", result);
	}
	else
		ps5_log("net test: 3. poll 200 ms: %d (nothing answers, as expected)%s%s", result, result < 0 ? ", " : "",
			result < 0 ? error_text(errno) : "");
	close(fd);
}

static void test_ssdp_options(uint32_t local)
{
	struct in_addr interface;
	char address[INET_ADDRSTRLEN];
	int value = 1, result;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
	{
		ps5_log("net test: 4. socket: %s", error_text(errno));
		return;
	}
	result = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
	ps5_log("net test: 4. SO_REUSEADDR: %d%s%s", result, result ? ", " : "", result ? error_text(errno) : "");
	/* (miniupnpc's ssdpDiscoverDevices sets the interface by address) */
	interface.s_addr = local;
	result = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &interface, sizeof(interface));
	ps5_log("net test: 4. IP_MULTICAST_IF %s: %d%s%s", ipv4_text(local, address), result, result ? ", " : "",
		result ? error_text(errno) : "");
	close(fd);
}

static uint32_t test_resolver(char *broker, size_t size, int *port)
{
	char address[INET_ADDRSTRLEN];
	double milliseconds;
	int code;
	uint32_t result;

	first_broker(broker, size, port);
	result = resolve(broker, &code, &milliseconds);
	ps5_log("net test: 5. the pool and resolver (sceNetInit 0x%08x, the pool %d)",
		(unsigned)net_init_result, resolver_pool);
	if (result)
		ps5_log("net test: 5. sceNetResolverStartNtoa(\"%s\"): %s in %.1f ms", broker, ipv4_text(result, address),
			milliseconds);
	else
		ps5_log("net test: 5. sceNetResolverStartNtoa(\"%s\"): 0x%08x after %.1f ms", broker, (unsigned)code,
			milliseconds);
	/* (again, to see what a second lookup of a name costs) */
	result = resolve(broker, &code, &milliseconds);
	ps5_log("net test: 5. again: %s in %.1f ms (code 0x%08x)", ipv4_text(result, address), milliseconds,
		(unsigned)code);
	return result;
}

static void test_tcp(uint32_t broker_address, const char *broker, int port)
{
	struct sockaddr_in address;
	socklen_t length = sizeof(address);
	struct pollfd wait;
	char text[INET_ADDRSTRLEN];
	uint64_t start;
	int fd, flags, result, pending = 0;
	socklen_t pending_length = sizeof(pending);

	if (!broker_address)
	{
		ps5_log("net test: 6. not tried: %s has no address", broker);
		return;
	}
	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
	{
		ps5_log("net test: 6. socket(SOCK_STREAM): %s", error_text(errno));
		return;
	}
	flags = fcntl(fd, F_GETFL);
	result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	ps5_log("net test: 6. fcntl O_NONBLOCK: F_GETFL 0x%x, F_SETFL %d%s%s", flags, result, result ? ", " : "",
		result ? error_text(errno) : "");
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((uint16_t)port);
	address.sin_addr.s_addr = broker_address;
	start = ps5_now_ns();
	result = connect(fd, (struct sockaddr *)&address, sizeof(address));
	ps5_log("net test: 6. connect %s:%d: %d%s%s", ipv4_text(broker_address, text), port, result, result ? ", " : "",
		result ? error_text(errno) : "");
	wait.fd = fd;
	wait.events = POLLOUT;
	wait.revents = 0;
	result = poll(&wait, 1, 5000);
	getsockopt(fd, SOL_SOCKET, SO_ERROR, &pending, &pending_length);
	ps5_log("net test: 6. poll for writable (5 s): %d, revents 0x%x, after %.1f ms; SO_ERROR %d", result,
		wait.revents, (ps5_now_ns() - start) / 1e6, pending);
	if (getpeername(fd, (struct sockaddr *)&address, &length) == 0)
		ps5_log("net test: 6. getpeername: %s:%u", ipv4_text(address.sin_addr.s_addr, text), ntohs(address.sin_port));
	else
		ps5_log("net test: 6. getpeername: %s", error_text(errno));
	result = shutdown(fd, SHUT_RDWR);
	ps5_log("net test: 6. shutdown: %d%s%s", result, result ? ", " : "", result ? error_text(errno) : "");
	result = close(fd);
	ps5_log("net test: 6. close: %d", result);
}

static void test_loopback(void)
{
	struct sockaddr_in address, peer;
	socklen_t length = sizeof(address), peer_length = sizeof(peer);
	char text[INET_ADDRSTRLEN];
	char byte = 'h', got = 0;
	int listener, client = -1, accepted = -1, result, available = -1, flags;

	listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (listener < 0)
	{
		ps5_log("net test: 7. socket: %s", error_text(errno));
		return;
	}
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 4) != 0 ||
		getsockname(listener, (struct sockaddr *)&address, &length) != 0)
	{
		ps5_log("net test: 7. the listener on 127.0.0.1 port 0: %s", error_text(errno));
		close(listener);
		return;
	}
	ps5_log("net test: 7. listening on 127.0.0.1:%u", ntohs(address.sin_port));
	client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	result = client >= 0 ? connect(client, (struct sockaddr *)&address, sizeof(address)) : -1;
	ps5_log("net test: 7. connect to it: %d%s%s", result, result ? ", " : "", result ? error_text(errno) : "");
	accepted = accept(listener, (struct sockaddr *)&peer, &peer_length);
	if (accepted < 0)
	{
		ps5_log("net test: 7. accept: %s", error_text(errno));
	}
	else
	{
		flags = fcntl(accepted, F_GETFL);
		ps5_log("net test: 7. accept: %d, from %s:%u (address length %u), F_GETFL 0x%x (O_NONBLOCK %s)", accepted,
			ipv4_text(peer.sin_addr.s_addr, text), ntohs(peer.sin_port), (unsigned)peer_length, flags,
			flags >= 0 && (flags & O_NONBLOCK) ? "set" : "not set");
		result = (int)send(client, &byte, 1, MSG_NOSIGNAL);
		usleep(20000);
		errno = 0;
		flags = ioctl(accepted, FIONREAD, &available);
		ps5_log("net test: 7. one byte from the client: send %d; FIONREAD on the accepted: ioctl %d, %s, count %d",
			result, flags, error_text(errno), available);
		{
			char peek[16];
			int peeked = (int)recv(accepted, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT);

			ps5_log("net test: 7. recv MSG_PEEK | MSG_DONTWAIT on the accepted: %d%s%s", peeked, peeked < 0 ? ", " : "",
				peeked < 0 ? error_text(errno) : "");
		}
		result = (int)recv(accepted, &got, 1, 0);
		ps5_log("net test: 7. recv on the accepted: %d ('%c')", result, result == 1 ? got : '?');
		byte = 'c';
		got = 0;
		result = (int)send(accepted, &byte, 1, MSG_NOSIGNAL);
		usleep(20000);
		available = -1;
		ioctl(client, FIONREAD, &available);
		ps5_log("net test: 7. one byte back: send %d; FIONREAD on the client %d", result, available);
		result = (int)recv(client, &got, 1, 0);
		ps5_log("net test: 7. recv on the client: %d ('%c'); loopback %s", result, result == 1 ? got : '?',
			result == 1 && got == 'c' ? "works in a title" : "DOES NOT work");
		close(accepted);
	}
	if (client >= 0)
		close(client);
	close(listener);
}

static void test_random(void)
{
	unsigned char bytes[32];
	char text[65];
	int index, nonzero = 0;

	memset(bytes, 0, sizeof(bytes));
	arc4random_buf(bytes, sizeof(bytes));
	for (index = 0; index < (int)sizeof(bytes); index++)
	{
		nonzero += bytes[index] != 0;
		snprintf(text + index * 2, 3, "%02x", bytes[index]);
	}
	ps5_log("net test: 8. arc4random_buf(32): %s (%d bytes not zero)", text, nonzero);
}

/* (not one of the plan's eight: fcntl's O_NONBLOCK is refused, step 6, and
so is FIONBIO; the console's own option, SO_NBIO, through setsockopt and
libSceNet's sceNetSetsockopt, each with a receive timeout so that a socket
left blocking does not hang the test) */
int sceNetSetsockopt(int socket, int level, int name, const void *value, unsigned int length);

static void test_nonblocking_with(const char *how, int name, int through_net)
{
	struct timeval guard = { 0, 300000 };
	struct sockaddr_in address;
	char buffer[16];
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int on = 1, result, error, received;
	uint64_t start;

	if (fd < 0)
		return;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	bind(fd, (struct sockaddr *)&address, sizeof(address));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &guard, sizeof(guard));
	errno = 0;
	result = through_net ? sceNetSetsockopt(fd, SOL_SOCKET, name, &on, sizeof(on)) :
		setsockopt(fd, SOL_SOCKET, name, &on, sizeof(on));
	error = errno;
	start = ps5_now_ns();
	received = (int)recv(fd, buffer, sizeof(buffer), 0);
	ps5_log("net test: 10. %s 0x%x: %d (0x%08x, %s); recv with nothing waiting then: %d, %s, after %.1f ms "
		"(%s)", how, name, result, (unsigned)result, error_text(error), received, error_text(errno),
		(ps5_now_ns() - start) / 1e6, (ps5_now_ns() - start) < 100000000ull ? "NON-BLOCKING" : "blocking");
	close(fd);
}

static void test_nonblocking(void)
{
	test_nonblocking_with("setsockopt SOL_SOCKET", 0x1200, 0);
	test_nonblocking_with("setsockopt SOL_SOCKET", 0x1100, 0);
	test_nonblocking_with("sceNetSetsockopt SOL_SOCKET", 0x1200, 1);
	test_nonblocking_with("sceNetSetsockopt SOL_SOCKET", 0x1100, 1);
}

/* (not one of the plan's eight: the plan has getifaddrs not exported, but
the SDK's libc stub has it, so it is tried, last) */
static void test_interfaces(void)
{
	struct ifaddrs *addresses, *entry;
	char text[INET_ADDRSTRLEN];
	int count = 0;

	if (getifaddrs(&addresses) != 0)
	{
		ps5_log("net test: 9. getifaddrs: %s", error_text(errno));
		return;
	}
	for (entry = addresses; entry; entry = entry->ifa_next)
	{
		count++;
		if (entry->ifa_addr && entry->ifa_addr->sa_family == AF_INET)
			ps5_log("net test: 9. getifaddrs: %s, flags 0x%x, %s", entry->ifa_name ? entry->ifa_name : "?",
				entry->ifa_flags, ipv4_text(((struct sockaddr_in *)entry->ifa_addr)->sin_addr.s_addr, text));
	}
	ps5_log("net test: 9. getifaddrs: %d entries", count);
	freeifaddrs(addresses);
}

void ps5_net_test(void)
{
	char broker[128];
	int port, datagram = -1;
	uint32_t local, broker_address;

	ps5_log("net test: the console's sockets in a title (HALO_PS5_NET_TEST)");
	test_socket(&datagram);
	if (datagram >= 0)
		close(datagram);
	local = test_route();
	test_broadcast();
	test_ssdp_options(local);
	broker_address = test_resolver(broker, sizeof(broker), &port);
	test_tcp(broker_address, broker, port);
	test_loopback();
	test_random();
	test_interfaces();
	test_nonblocking();
	ps5_log("net test: done");
}

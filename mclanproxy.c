/******************************************************************************
 * MinecraftLANProxy
 * © 2015 Ola Liljedahl
 * Proxy server for Minecraft LAN worlds to enable remote access
 *****************************************************************************/
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_IF "eth0" //TODO find automatically?
#define MINECRAFT_ANNOUNCE "224.0.2.60"
#define PUBLIC_PORT 12345 //TODO use option
#define BUFSIZE 8192

static bool verbose;

static uint32_t get_ip_addr(const char *ifname)
{
    int sd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sd < 0)
    {
	perror("socket"), exit(EXIT_FAILURE);
    }

    struct ifreq ifr;
    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    int rc = ioctl(sd, SIOCGIFADDR, &ifr);
    if (rc < 0)
    {
	perror("ioctl"), exit(EXIT_FAILURE);
    }

    rc = close(sd);
    if (rc < 0)
    {
	perror("close"), exit(EXIT_FAILURE);
    }

    static bool done;
    if (verbose && !done)
    {
	printf("Interface %s has IP address %s\n",
	       ifname,
	       inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
	done = true;
    }
    return ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
}

static void set_nonblock(int sd)
{
    int flags = fcntl(sd, F_GETFL, 0);
    if (flags == -1)
    {
	perror("fcntl(F_GETFL)"), exit(EXIT_FAILURE);
    }
    int rc = fcntl(sd, F_SETFL, flags | O_NONBLOCK);
    if (rc != 0)
    {
	perror("fcntl(F_SETFL)"), exit(EXIT_FAILURE);
    }
}

struct buffer
{
    char buf[BUFSIZE];
    unsigned offset, length;
};

static void print_error(const char *peer, int sd0, int sd1)
{
    printf("Poll error or hangup on %s socket, terminating\n", peer);
    (void)close(sd0);
    (void)close(sd1);
    exit(EXIT_SUCCESS);
}

static void read_socket(int sd, struct buffer *buf, const char *peer)
{
    assert(buf->length == 0);
    int rc = read(sd, buf->buf, sizeof buf->buf);
    if (rc < 0)
    {
	if (errno != EAGAIN)
	{
	    perror("read"), exit(EXIT_FAILURE);
	}
    }
    else if (rc != 0)
    {
//	printf("Read %u bytes from %s\n", rc, peer);
	buf->length = rc;
	buf->offset = 0;
    }
    else
    {
	printf("EOF on %s socket\n", peer);
	(void)close(sd);
	//Other socket closed when process terminates
	exit(EXIT_SUCCESS);
    }
}

static void write_socket(struct buffer *buf, int sd, const char *peer)
{
    (void)peer;
    assert(buf->length != 0);
    int rc = write(sd, buf->buf + buf->offset, buf->length);
    if (rc < 0)
    {
	if (errno != EAGAIN)
	{
	    perror("write"), exit(EXIT_FAILURE);
	}
    }
    else if (rc != 0)
    {
//	printf("Wrote %u bytes to %s\n", rc, peer);
	buf->offset += rc;
	buf->length -= rc;
    }
}

static void layer7_splice(int remote_sd, int mclan_sd)
{
    set_nonblock(remote_sd);
    set_nonblock(mclan_sd);

    struct buffer r2m, m2r;
    r2m.offset = 0;
    r2m.length = 0;
    m2r.offset = 0;
    m2r.length = 0;

    for (;;)
    {
	struct pollfd pfd[2];
#define RIDX 0 //Remote index
	pfd[RIDX].fd = remote_sd;
	pfd[RIDX].events = POLLERR;
	pfd[RIDX].revents = 0;
#define MIDX 1 //MCLAN index
	pfd[MIDX].fd = mclan_sd;
	pfd[MIDX].events = POLLERR;
	pfd[MIDX].revents = 0;

	if (r2m.length == 0)
	{
	    //remote-to-minecraft buffer empty, read from remote-socket
	    pfd[RIDX].events |= POLLIN;
	}
	else
	{
	    //remote-to-minecraft buffer not empty, write to minecraft-socket
	    pfd[MIDX].events |= POLLOUT;
	}
	if (m2r.length == 0)
	{
	    //minecraft-to-remote buffer empty, read from minecraft-socket
	    pfd[MIDX].events |= POLLIN;
	}
	else
	{
	    //minecraft-to-remote buffer not empty, write to remote-socket
	    pfd[RIDX].events |= POLLOUT;
	}

	int rc = poll(pfd, 2, -1);
	if (rc < 0)
	{
	    perror("poll"), exit(EXIT_FAILURE);
	}
	if ((pfd[RIDX].revents & (POLLERR | POLLHUP)) != 0)
	{
	    print_error("remote", remote_sd, mclan_sd);
	}
	if ((pfd[MIDX].revents & (POLLERR | POLLHUP)) != 0)
	{
	    print_error("mclan", remote_sd, mclan_sd);
	}
	if ((pfd[RIDX].revents & POLLIN) != 0)
	{
	    //Read from remote-socket into remote-to-minecraft-buffer
	    read_socket(remote_sd, &r2m, "remote");
	}
	if ((pfd[MIDX].revents & POLLIN) != 0)
	{
	    //Read from minecraft-socket into minecraft-to-remote-buffer
	    read_socket(mclan_sd, &m2r, "mclan");
	}
	if ((pfd[RIDX].revents & POLLOUT) != 0)
	{
	    //Write to remote-socket from minecraft-to-remote-buffer
	    write_socket(&m2r, remote_sd, "remote");
	}
	if ((pfd[MIDX].revents & POLLOUT) != 0)
	{
	    //Write to minecraft-socket from remote-to-minecraft-buffer
	    write_socket(&r2m, mclan_sd, "mclan");
	}
    }
}

static pid_t fork_proxy(int remote_sd, int mclan_sd)
{
    pid_t pid;
    printf("Forking child proxy\n");
    pid = fork();
    if (pid == 0)
    {
	//child process
	layer7_splice(remote_sd, mclan_sd);
	exit(EXIT_SUCCESS);
    }
    else if (pid != -1)
    {
	//parent process
	return pid;
    }
    else
    {
	//Print a non-fatal error message
	perror("fork");
	//We continue execution
	return -1;
    }
}

static void proxy(struct in_addr mc_addr, uint16_t mc_port)
{
    printf("Minecraft LAN server at %s:%u\n", inet_ntoa(mc_addr), mc_port);
    int listen_sd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sd < 0)
    {
	perror("socket"), exit(EXIT_FAILURE);
    }

    int reuseaddr = 1;
    if (setsockopt(listen_sd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr,
		   sizeof reuseaddr) < 0)
    {
	perror("setsockopt(SO_REUSEADDR)"), exit(EXIT_FAILURE);
    }

    struct sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(PUBLIC_PORT);
    if (bind(listen_sd, (struct sockaddr *)&local, sizeof local) < 0)
    {
	perror("bind"), exit(EXIT_FAILURE);
    }

    struct in_addr in;
    in.s_addr = get_ip_addr(DEFAULT_IF);
    if (listen(listen_sd, 5) < 0)
    {
	perror("listen"), exit(EXIT_FAILURE);
    }

    for (;;)
    {
	printf("Accepting connections on %s:%u\n", inet_ntoa(in), PUBLIC_PORT);
	struct sockaddr_in sin_rem;
	socklen_t addr_len = sizeof sin_rem;
	int rem_sd = accept(listen_sd, (struct sockaddr *)&sin_rem,
		                &addr_len);
	if (rem_sd < 0)
	{
	    perror("accept"), exit(EXIT_FAILURE);
	}
	printf("Remote: %s:%u\n", inet_ntoa(sin_rem.sin_addr),
				  ntohs(sin_rem.sin_port));

	int mclan_sd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (mclan_sd < 0)
	{
	    perror("socket"), exit(EXIT_FAILURE);
	}

	//Attempt to connect to Minecraft server
	struct sockaddr_in sin_mc;
	memset(&sin_mc, 0, sizeof sin_mc);
	sin_mc.sin_family = AF_INET;
	sin_mc.sin_addr.s_addr = mc_addr.s_addr;
	sin_mc.sin_port = htons(mc_port);
	if (connect(mclan_sd, (struct sockaddr *)&sin_mc, sizeof sin_mc) < 0)
	{
	    perror("connect"), exit(EXIT_FAILURE);
	}
	printf("Connected to Minecraft LAN server\n");

	fork_proxy(rem_sd, mclan_sd);

	if (close(rem_sd) != 0)
	{
	    perror("close"), exit(EXIT_FAILURE);
	}
	if (close(mclan_sd) != 0)
	{
	    perror("close"), exit(EXIT_FAILURE);
	}
    }
}

int main(int argc, char *argv[])
{
    const char *ifname = DEFAULT_IF;
    int c;
    while ((c = getopt(argc, argv, "i:v")) != -1)
    {
	switch (c)
	{
	    case 'i' :
		ifname = optarg;
		break;
	    case 'v' :
		verbose = true;
		break;
	    default :
usage :
		fprintf(stderr, "Usage: mclanproxy <options>\n"
			"-i <ifname>     Interface to use\n");
		exit(EXIT_FAILURE);
	}
    }
    if (optind + 0 > argc)
    {
	goto usage;
    }

    int sd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sd < 0)
    {
	perror("socket"), exit(EXIT_FAILURE);
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(4445);
    if (bind(sd, (struct sockaddr *)&sin, sizeof sin) < 0)
    {
	perror("bind"), exit(EXIT_FAILURE);
    }

    struct ip_mreq multi;
    multi.imr_multiaddr.s_addr = inet_addr(MINECRAFT_ANNOUNCE);
    multi.imr_interface.s_addr = get_ip_addr(ifname);
    if (setsockopt(sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &multi, sizeof multi) < 0)
    {
	perror("setsockopt(IP_ADD_MEMBERSHIP)"), exit(EXIT_FAILURE);
    }

    for (;;)
    {
	char buf[256];
	struct sockaddr_in sin_mc;
	socklen_t addr_len = sizeof sin_mc;
	int rc = recvfrom(sd, buf, sizeof buf, 0, (struct sockaddr *)&sin_mc,
		&addr_len);
	if (rc < 0)
	{
	    perror("recvfrom"), exit(EXIT_FAILURE);
	}
	if (rc > (int)sizeof buf - 1)
	{
	    rc = sizeof buf - 1;
	}
	buf[rc] = 0;
	printf("Announcement: %s\n", buf);
	printf("Sender: %s:%u\n", inet_ntoa(sin_mc.sin_addr),
		ntohs(sin_mc.sin_port));
	//Looking for (address and) port
	char *found = strstr(buf, "[AD]");
	if (found != NULL)
	{
	    //Check for [AD]a.b.c.d:p[/AD]
	    char *found2 = strchr(found + 4, ':');
	    if (found2 != NULL)
	    {
		proxy(sin_mc.sin_addr, atoi(found2 + 1));
	    }
	    else
	    {
		//Assume [AD]p[/AD]
		proxy(sin_mc.sin_addr, atoi(found + 4));
	    }
	}
    }
    return 0;
}

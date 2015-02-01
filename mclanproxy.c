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

//Default interface to listen for announcements on
#define DEFAULT_IF "eth0" //TODO find automatically?
//Multicast address for Minecraft LAN world announcements
#define MINECRAFT_ANNOUNCE "224.0.2.60"
//Default public port of proxy
#define DEFAULT_PORT 12345
//Size of splicing buffer
#define BUFSIZE 8192
//Size of buffer for annoucement
#define ANNOUNCEMENT_BUFSIZE 256

static uint16_t verbose;

//Return interface IP address in network byte order
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

    if (verbose > 1)
    {
	printf("Interface %s has IP address %s\n",
	       ifname,
	       inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
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
    uint16_t offset, length;
    uint32_t accumulated;
};

static void print_error(const char *peer)
{
    if (verbose)
    {
	printf("Poll error on %s socket\n", peer);
    }
}

static bool read_socket(int sd, struct buffer *buf, const char *peer)
{
    assert(buf->length == 0);
    int rc = read(sd, buf->buf, sizeof buf->buf);
    if (rc < 0)
    {
	if (errno != EAGAIN)
	{
	    perror("read"), exit(EXIT_FAILURE);
	}
	return true;
    }
    else if (rc != 0)
    {
//	printf("Read %u bytes from %s\n", rc, peer);
	buf->length = rc;
	buf->offset = 0;
	return true;
    }
    else
    {
	if (verbose)
	{
	    printf("EOF on %s socket\n", peer);
	}
	return false;
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
	buf->accumulated += rc;
    }
}

static void print_stats(struct buffer *buf, const char *direction)
{
    if (verbose)
    {
	printf("%s: %u bytes transferred\n", direction, buf->accumulated);
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

    bool error;
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
	if ((pfd[RIDX].revents & POLLERR) != 0)
	{
	    print_error("remote");
	    error = true;
	    goto done;
	}
	if ((pfd[MIDX].revents & POLLERR) != 0)
	{
	    print_error("mclan");
	    error = true;
	    goto done;
	}
	if ((pfd[RIDX].revents & POLLIN) != 0)
	{
	    //Read from remote-socket into remote-to-minecraft-buffer
	    if (!read_socket(remote_sd, &r2m, "remote"))
	    {
		error = false;
		goto done;
	    }
	}
	if ((pfd[MIDX].revents & POLLIN) != 0)
	{
	    //Read from minecraft-socket into minecraft-to-remote-buffer
	    if (!read_socket(mclan_sd, &m2r, "mclan"))
	    {
		error = false;
		goto done;
	    }
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
done:
    print_stats(&r2m, "remote-to-mclan");
    print_stats(&m2r, "mclan-to-remote");
    (void)close(remote_sd);
    (void)close(mclan_sd);
    if (verbose > 1)
    {
	printf("<%d> terminating\n", getpid());
    }
    exit(error ? EXIT_FAILURE : EXIT_SUCCESS);
}

static pid_t fork_proxy(int remote_sd, int mclan_sd)
{
    pid_t pid;
    pid = fork();
    if (pid == 0)
    {
	//child process
	if (verbose > 1)
	{
	    printf("Proxy process pid %d\n", getpid());
	}
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

static bool read_message(int announce_sd,
	                 struct sockaddr_in *mc_sin)
{
    char msg[ANNOUNCEMENT_BUFSIZE];
    socklen_t addr_len = sizeof *mc_sin;
    int rc = recvfrom(announce_sd, msg, sizeof msg, 0,
	    (struct sockaddr *)mc_sin, &addr_len);
    if (rc < 0)
    {
	perror("recvfrom"), exit(EXIT_FAILURE);
    }
    if (rc > (int)sizeof msg - 1)
    {
	rc = sizeof msg - 1;
    }
    msg[rc] = 0;
    if (verbose > 1)
    {
	printf("Announcement: %s\n", msg);
	printf("Sender: %s:%u\n", inet_ntoa(mc_sin->sin_addr),
		ntohs(mc_sin->sin_port));
    }
    //Looking for (address and) port
    char *found_ad = strstr(msg, "[AD]");
    if (found_ad != NULL)
    {
	//TODO use sscanf() to parse message
	char *found_end = strstr(found_ad + 4, "[/AD]");
	if (found_end != NULL)
	{
	    //Check for [AD]a.b.c.d:p[/AD]
	    char *found_port = strchr(found_ad + 4, ':');
	    int port;
	    if (found_port != NULL)
	    {
		port = atoi(found_port + 1);
	    }
	    else
	    {
		//Assume [AD]p[/AD]
		port = atoi(found_ad + 4);
	    }
	    if (port < 0x10000)
	    {
		//Overwrite the port the message was sent from
		mc_sin->sin_port = htons(port);
		return true;
	    }
	    //Else invalid port number
	}
	//Else message truncated, ignore it
    }
    //Else not an announcement
    return false;
}

static int create_accept_socket(uint16_t public_port)
{
    int accept_sd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (accept_sd < 0)
    {
	perror("socket"), exit(EXIT_FAILURE);
    }

    int reuseaddr = 1;
    if (setsockopt(accept_sd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr,
		   sizeof reuseaddr) < 0)
    {
	perror("setsockopt(SO_REUSEADDR)"), exit(EXIT_FAILURE);
    }

    struct sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(public_port);
    if (bind(accept_sd, (struct sockaddr *)&local, sizeof local) < 0)
    {
	perror("bind"), exit(EXIT_FAILURE);
    }

    if (listen(accept_sd, 5) < 0)
    {
	perror("listen"), exit(EXIT_FAILURE);
    }

    if (verbose)
    {
	printf("Accepting connections on port %u\n", public_port);
    }

    set_nonblock(accept_sd);
    return accept_sd;
}

static void create_proxy(int accept_sd,
			 const struct sockaddr_in *mc_addr)
{

    //Accept the connection from remote host
    struct sockaddr_in sin_rem;
    socklen_t addr_len = sizeof sin_rem;
    int rem_sd = accept(accept_sd, (struct sockaddr *)&sin_rem, &addr_len);
    if (rem_sd < 0)
    {
	perror("accept"), exit(EXIT_FAILURE);
    }
    if (verbose)
    {
	printf("Remote: %s:%u\n", inet_ntoa(sin_rem.sin_addr),
		ntohs(sin_rem.sin_port));
    }

    //Create a socket for connecting to Minecraft server
    int mclan_sd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (mclan_sd < 0)
    {
	perror("socket"), exit(EXIT_FAILURE);
    }

    //Connect to Minecraft server
    if (connect(mclan_sd, (struct sockaddr *)mc_addr, sizeof *mc_addr) < 0)
    {
	//Ignore a number of non-fatal error codes
	if (errno == ETIMEDOUT ||
	    errno == ECONNRESET ||
	    errno == ECONNREFUSED ||
	    errno == EHOSTDOWN ||
	    errno == EHOSTUNREACH ||
	    errno == ENETUNREACH)
	{
	    //Non-fatal error
	    goto cleanup;
	}
	//Treat remaining errors as fatal
	perror("connect"), exit(EXIT_FAILURE);
    }
    if (verbose)
    {
	printf("Connected to Minecraft LAN server\n");
    }

    fork_proxy(rem_sd, mclan_sd);

cleanup:
    if (close(rem_sd) != 0)
    {
	perror("close"), exit(EXIT_FAILURE);
    }
    if (close(mclan_sd) != 0)
    {
	perror("close"), exit(EXIT_FAILURE);
    }
}

static void listen_for_announcement(const char *ifname, uint16_t public_port)
{
    int announce_sd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (announce_sd < 0)
    {
	perror("socket"), exit(EXIT_FAILURE);
    }

    //Bind socket to Minecraft announcement port
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(4445);
    if (bind(announce_sd, (struct sockaddr *)&sin, sizeof sin) < 0)
    {
	perror("bind"), exit(EXIT_FAILURE);
    }

    //Listen on the Minecraft announcement multicast IP address
    //TODO do this before binding to the port above?
    struct ip_mreq multi;
    multi.imr_multiaddr.s_addr = inet_addr(MINECRAFT_ANNOUNCE);
    multi.imr_interface.s_addr = get_ip_addr(ifname);
    if (setsockopt(announce_sd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
		   &multi, sizeof multi) < 0)
    {
	perror("setsockopt(IP_ADD_MEMBERSHIP)"), exit(EXIT_FAILURE);
    }

    int accept_sd = -1;//Invalid descriptor
    struct sockaddr_in mc_new, mc_cur;
    mc_new.sin_addr.s_addr = 0;//Invalid IP address
    mc_new.sin_port = 0;
    mc_cur.sin_addr.s_addr = 0;//Invalid IP address
    mc_cur.sin_port = 0;
    for (;;)
    {
	struct pollfd pfd[2];
	pfd[0].fd = announce_sd;
	pfd[0].events = POLLIN;
	pfd[0].revents = 0;
	pfd[1].fd = accept_sd;
	pfd[1].events = POLLIN;
	pfd[1].revents = 0;
	//Poll announcement socket, poll accept socket if valid
	//TODO add timeout
	int rc = poll(pfd, accept_sd != -1 ? 2 : 1, -1);
	if (rc < 0)
	{
	    perror("poll"), exit(EXIT_FAILURE);
	}
	//Check announcement socket
	if ((pfd[0].revents & POLLIN) != 0)
	{
	    //Data on announcement socket
	    if (read_message(announce_sd, &mc_new))
	    {
		if (mc_new.sin_addr.s_addr != mc_cur.sin_addr.s_addr ||
		    mc_new.sin_port != mc_cur.sin_port)
		{
		    if (verbose)
		    {
			printf("Minecraft LAN server at %s:%u\n",
				inet_ntoa(mc_new.sin_addr),
				ntohs(mc_new.sin_port));
		    }
		    if (accept_sd != -1)
		    {
			if (close(accept_sd) < 0)
			{
			    perror("close"), exit(EXIT_FAILURE);
			}
		    }
		    accept_sd = create_accept_socket(public_port);
		    if (accept_sd != -1)
		    {
			//Make new LAN server current
			mc_cur = mc_new;
		    }
		}
		//Else same address as previous
	    }
	    //Else not a proper announcement
	}
	//Check accept socket, data to read => connection waiting
	if ((pfd[1].revents & POLLIN) != 0)
	{
	    //Connection on accept socket
	    create_proxy(accept_sd, &mc_cur);
	}
    }
}

int main(int argc, char *argv[])
{
    const char *ifname = DEFAULT_IF;
    uint16_t public_port = DEFAULT_PORT;
    int c;
    while ((c = getopt(argc, argv, "i:p:vV")) != -1)
    {
	switch (c)
	{
	    case 'i' :
		ifname = optarg;
		break;
	    case 'p' :
		public_port = (uint16_t)atoi(optarg);
		break;
	    case 'v' :
		verbose = 1;
		break;
	    case 'V' :
		verbose = 2;
		break;
	    default :
usage :
		fprintf(stderr, "Usage: mclanproxy <options>\n"
			"-i <ifname>     Interface to use\n"
			"-p <port>       Public port\n"
			"-v              Verbose\n"
			"-V              Extra verbose\n");
		exit(EXIT_FAILURE);
	}
    }
    if (optind + 0 > argc)
    {
	goto usage;
    }

    listen_for_announcement(ifname, public_port);
    return 0;
}

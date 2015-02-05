/******************************************************************************
 * MinecraftLANProxy
 * Proxy server to enable remote access to Minecraft LAN worlds
 * Copyright 2015 Ola Liljedahl
 *****************************************************************************/
#define _GNU_SOURCE //for ip_mreq and getopt
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

//Multicast address for Minecraft LAN world announcements
#define ANNOUNCE_ADDR "224.0.2.60"

//UDP port number for Minecraft LAN world announcements
#define ANNOUNCE_PORT 4445

//Default public port for remote connections
#define PUBLIC_PORT 4446

//Size of splicing buffer
#define BUFSIZE 8192

//Announcement message buffer size (message contains name of LAN world)
#define ANNOUNCEMENT_BUFSIZE 256

//Timeout (in seconds) for LAN server supervision
#define MCLAN_TIMEOUT 5

static int verbose;

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
    uint64_t accumulated;
    uint16_t offset, length;
    char buf[BUFSIZE];
};

static void print_error(const char *peer)
{
    if (verbose)
    {
	printf("%d: error on %s socket\n", getpid(), peer);
    }
}

static bool read_socket(int sd, struct buffer *buf, const char *peer)
{
    assert(buf->length == 0);
    int rc = read(sd, buf->buf, sizeof buf->buf);
    if (rc < 0)
    {
	if (errno == EAGAIN)
	{
	    return true;//not EOF
	}
	perror("read"), exit(EXIT_FAILURE);
    }
    else if (rc != 0)
    {
	buf->length = rc;
	buf->offset = 0;
	return true;//not EOF
    }
    else//rc == 0
    {
	if (verbose)
	{
	    printf("%d: EOF on %s socket\n", getpid(), peer);
	}
	return false;//EOF
    }
}

static void write_socket(struct buffer *buf, int sd)
{
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
	buf->offset += rc;
	buf->length -= rc;
	buf->accumulated += rc;
    }
}

static void print_stats(struct buffer *buf,
			unsigned secs,
			const char *direction)
{
    if (verbose)
    {
	pid_t pid = getpid();
	if (secs != 0)
	{
	    const char *metric = "";
	    unsigned rate = buf->accumulated / secs;
	    if (rate > 10000)
	    {
		rate /= 1000;
		metric = "K";
	    }
	    printf("%d: %s: %"PRIu64" bytes transferred, %u %sbytes/s\n",
		    pid, direction, buf->accumulated, rate, metric);
	}
	else
	{
	    printf("%d: %s: %"PRIu64" bytes transferred\n",
		    pid, direction, buf->accumulated);
	}
    }
}

static void layer7_splice(int remote_sd, int mclan_sd)
{
    time_t start = time(NULL);

    set_nonblock(remote_sd);
    set_nonblock(mclan_sd);

    struct buffer r2m, m2r;
    memset(&r2m, 0, sizeof r2m);
    memset(&m2r, 0, sizeof m2r);

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
	    if (errno == EINTR || errno == ERESTART)
	    {
		continue;
	    }
	    perror("poll"), exit(EXIT_FAILURE);
	}
	if ((pfd[RIDX].revents & POLLERR) != 0)
	{
	    print_error("remote");
	    error = true;
	    goto cleanup;
	}
	if ((pfd[MIDX].revents & POLLERR) != 0)
	{
	    print_error("mclan");
	    error = true;
	    goto cleanup;
	}
	if ((pfd[RIDX].revents & POLLIN) != 0)
	{
	    //Read from remote-socket into remote-to-minecraft-buffer
	    if (!read_socket(remote_sd, &r2m, "remote"))
	    {
		error = false;
		goto cleanup;
	    }
	}
	if ((pfd[MIDX].revents & POLLIN) != 0)
	{
	    //Read from minecraft-socket into minecraft-to-remote-buffer
	    if (!read_socket(mclan_sd, &m2r, "mclan"))
	    {
		error = false;
		goto cleanup;
	    }
	}
	if ((pfd[RIDX].revents & POLLOUT) != 0)
	{
	    //Write to remote-socket from minecraft-to-remote-buffer
	    write_socket(&m2r, remote_sd);
	}
	if ((pfd[MIDX].revents & POLLOUT) != 0)
	{
	    //Write to minecraft-socket from remote-to-minecraft-buffer
	    write_socket(&r2m, mclan_sd);
	}
    }
cleanup: (void)0;//Need a statement to make compiler happy
    unsigned secs = time(NULL) - start;
    pid_t pid = getpid();
    if (verbose)
    {
	unsigned hours = secs / 3600;
	unsigned mins = (secs / 60) % 60;
	printf("%d: Session duration %02u:%02u:%02u h:m:s\n",
		pid, hours, mins, secs % 60);
    }
    print_stats(&r2m, secs, "remote-to-mclan");
    print_stats(&m2r, secs , "mclan-to-remote");
    (void)close(remote_sd);
    (void)close(mclan_sd);
    exit(error ? EXIT_FAILURE : EXIT_SUCCESS);
}

static void fork_proxy(const struct sockaddr_in *sin_rem,
		       int remote_sd,
		       const struct sockaddr_in *mc_addr)
{
    pid_t pid;
    pid = fork();
    if (pid == -1)
    {
	//Fork failed
	if (verbose)
	{
	    printf("Failed to fork proxy (errno %d) for remote %s:%u\n",
		    errno,
		    inet_ntoa(sin_rem->sin_addr), ntohs(sin_rem->sin_port));
	}
	//Continue execution, better luck next time
    }
    else if (pid == 0)
    {
	//child process
	if (verbose)
	{
	    printf("%d: Proxy forked for remote %s:%u\n",
		    getpid(),
		    inet_ntoa(sin_rem->sin_addr), ntohs(sin_rem->sin_port));
	}

	//Close all unused descriptors
	//Ignore stdin/stdout/stderr and remote_sd
	int desc;
	for (desc = 3; desc < remote_sd; desc++)
	{
	    (void)close(desc);
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
		if (verbose)
		{
		    printf("%d: Failed to connect to %s:%u\n",
			    getpid(),
			    inet_ntoa(mc_addr->sin_addr),
			    ntohs(mc_addr->sin_port));
		}
		exit(EXIT_FAILURE);
	    }
	    perror("connect"), exit(EXIT_FAILURE);
	}
	if (verbose)
	{
	    printf("%d: Connected to Minecraft LAN server\n", getpid());
	    fflush(stdout);
	}

	layer7_splice(remote_sd, mclan_sd);
	exit(EXIT_SUCCESS);
    }
    //else parent process
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
    //Else not a Minecraft LAN world announcement
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

    //Set max number of enqueued connection requests
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

    fork_proxy(&sin_rem, rem_sd, mc_addr);

    if (close(rem_sd) != 0)
    {
	perror("close"), exit(EXIT_FAILURE);
    }
}

static void sigchld_handler(int signum)
{
    (void)signum;
    int status;
    pid_t child_pid = waitpid(-1, &status, WNOHANG);
    printf("Proxy %d terminated\n", child_pid);
    fflush(stdout);
}

static void listen_for_announcement(uint16_t public_port)
{
    //Install signal handler for child termination
    signal(SIGCHLD, sigchld_handler);

    int announce_sd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (announce_sd < 0)
    {
	perror("socket"), exit(EXIT_FAILURE);
    }

    int reuseaddr = 1;
    if (setsockopt(announce_sd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr,
		   sizeof reuseaddr) < 0)
    {
	perror("setsockopt(SO_REUSEADDR)"), exit(EXIT_FAILURE);
    }

    //Join the Minecraft announcement IP multicast group
    struct ip_mreq multi;
    multi.imr_multiaddr.s_addr = inet_addr(ANNOUNCE_ADDR);
    multi.imr_interface.s_addr = INADDR_ANY;//Receive on any interface?
    if (setsockopt(announce_sd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
		   &multi, sizeof multi) < 0)
    {
	perror("setsockopt(IP_ADD_MEMBERSHIP)"), exit(EXIT_FAILURE);
    }

    //Bind socket to Minecraft announcement address and port
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(ANNOUNCE_ADDR);
    sin.sin_port = htons(ANNOUNCE_PORT);
    if (bind(announce_sd, (struct sockaddr *)&sin, sizeof sin) < 0)
    {
	perror("bind"), exit(EXIT_FAILURE);
    }

    //Socket to listen for remote connections on
    int accept_sd = -1;//Invalid descriptor
    //Last time we heard from LAN server
    time_t last = (time_t)-1;
    //Address of LAN server
    struct sockaddr_in mc_cur;
    memset(&mc_cur, 0, sizeof mc_cur);//Invalid address
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
	int rc;
	if (accept_sd == -1)
	{
	    //No LAN server known, block until we receive an announcement
	    rc = poll(pfd, 1, -1/*No timeout*/);
	}
	else
	{
	    //LAN server known, block with timeout so we can supervise it
	    assert(last != (time_t)-1);
	    rc = poll(pfd, 2, 2000/*timeout in milliseconds*/);
	}
	if (rc < 0)
	{
	    if (errno == EINTR || errno == ERESTART)
	    {
		continue;
	    }
	    perror("poll"), exit(EXIT_FAILURE);
	}
	else if (rc == 0)
	{
	    //Timeout
	    assert(last != (time_t)-1);
	    assert(accept_sd != -1);
	    time_t now = time(NULL);
	    if (now - last >= MCLAN_TIMEOUT)
	    {
		//No announcement for N seconds
		if (verbose)
		{
		    printf("Lost contact with Minecraft LAN server\n");
		    fflush(stdout);
		}
		//Stop accepting connections
		if (close(accept_sd) < 0)
		{
		    perror("close"), exit(EXIT_FAILURE);
		}
		accept_sd = -1;
		//Invalidate current address
		memset(&mc_cur, 0, sizeof mc_cur);
	    }
	    //Skip checking revents
	    continue;
	}
	//Check announcement socket
	if ((pfd[0].revents & POLLIN) != 0)
	{
	    struct sockaddr_in mc_new;
	    //Data on announcement socket
	    if (read_message(announce_sd, &mc_new))
	    {
		//Found a valid announcement
		if (mc_new.sin_addr.s_addr != mc_cur.sin_addr.s_addr ||
		    mc_new.sin_port != mc_cur.sin_port)
		{
		    if (verbose)
		    {
			printf("Found Minecraft LAN server at %s:%u\n",
				inet_ntoa(mc_new.sin_addr),
				ntohs(mc_new.sin_port));
			fflush(stdout);
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
		//Else we already have this address
		//Save last time we saw it
		last = time(NULL);
	    }
	    //Else not a proper announcement
	}
	//Check accept socket
	if ((pfd[1].revents & POLLIN) != 0)
	{
	    //Data to read => connection waiting on accept socket
	    create_proxy(accept_sd, &mc_cur);
	}
    }
}

int main(int argc, char *argv[])
{
    uint16_t public_port = PUBLIC_PORT;
    int c;
    while ((c = getopt(argc, argv, "p:vV")) != -1)
    {
	switch (c)
	{
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

    listen_for_announcement(public_port);
    return 0;
}

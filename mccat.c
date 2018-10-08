/* Multicast cat
   Copyright (c) 2004-2013 Wouter Cloetens <wouter@e2big.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/


#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <netdb.h>
#include <time.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <poll.h>
#include <syslog.h>

#define BUFSIZE 4096

static struct sockaddr_in addr, my_addr;
static unsigned int if_index;
static char *if_name;
static int use_multicast;
static int terminate;
static int show_status;
static int epoch;

static int ts_hexdump;
static int rtp_check;
static int rtp_ival;

static uint64_t u64_pkt_ts;
static uint64_t u64_prev_pkt_ts;
//static uint64_t u64_pkt_ival;
static int rtp_latency;

static struct in_addr local_addr;

static int stats;
static int stats_rx_pkts;
static int stats_rx_bytes;
static int stats_rtp_lost;
static double stats_rtp_lost_ratio;
static double stats_rtp_bw;
static int stats_rtp_latency;


static uint64_t total_rx_pkts;
static uint64_t total_rx_bytes;
static uint64_t total_rtp_lost;
static double total_rtp_lost_ratio;
//static double total_rtp_bw;
static uint64_t total_rtp_latency;

static struct timeval pkt_ts;
//static struct timeval prev_pkt_ts;
//static struct timeval pkt_ival;
static int quiet;

static void show_usage(const char *name);
static int parse_args(int argc, char *argv[]);
static void mainloop(int s);
static void hexdump(const unsigned char *buffer, size_t len);
static int ts_fprintf(FILE *f, struct timeval *tv, const char *format, ...);
static void sighandler(int signum);

int main(int argc, char *argv[])
{
    int s;
    struct sigaction newsig = {.sa_handler=sighandler};
    struct sigaction oldsig;
    struct sockaddr_in sin;
/*    struct ip_mreqn mreq; */
    struct ip_mreq fdd_mreq;

    if (parse_args(argc, argv))
    {
	show_usage(argv[0]);
	return EXIT_FAILURE;
    }

    openlog(NULL,LOG_PID|LOG_CONS|LOG_NOWAIT|LOG_PERROR,LOG_USER);
    s = socket(addr.sin_family == AF_INET ? PF_INET : PF_INET6, SOCK_DGRAM, 0);
    if (s < 0)
    {
	fprintf(stderr, "Error opening socket: %s\n", strerror(errno));
	return EXIT_FAILURE;
    }
    sin = addr;

    sin.sin_addr.s_addr = local_addr.s_addr;

    if (use_multicast)
    {
        int optval = 1;
	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
	{
	    fprintf(stderr, "Warning: could not set SO_REUSEADDR for multicast group %s: %m\n",
		    /*inet_ntoa(mreq.imr_multiaddr)*/ "toto");
	}
    }

	fprintf(stderr, "Binding to port %hu of %s\n",
		ntohs(sin.sin_port), inet_ntoa(sin.sin_addr));

    if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0)
    {
        close(s);
	fprintf(stderr, "Error binding to port %hu of %s: %s\n",
		ntohs(sin.sin_port), inet_ntoa(sin.sin_addr),
		strerror(errno));
	closelog();
	return EXIT_FAILURE;
    }

    if (use_multicast)
    {
	/*
	mreq.imr_multiaddr = addr.sin_addr;
	mreq.imr_address = my_addr.sin_addr;
	mreq.imr_ifindex = if_index;
	*/

	fdd_mreq.imr_multiaddr = addr.sin_addr;
	fdd_mreq.imr_interface = /*my_addr.sin_addr*/ local_addr;

	fprintf(stderr, "Joining group %s:%hu\n",
		inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

	if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &fdd_mreq, sizeof(fdd_mreq)) < 0)
	{
            close(s);
	    fprintf(stderr, "Error joining multicast group: %s\n", strerror(errno));
	    closelog();
	    return EXIT_FAILURE;
	}
    }

    sigaction(SIGINT, &newsig, &oldsig);
    sigaction(SIGUSR1, &newsig, NULL);
    mainloop(s);
    sigaction(SIGINT, &oldsig, NULL);

    if (use_multicast)
    {
	/*
	mreq.imr_multiaddr = addr.sin_addr;
	mreq.imr_address = my_addr.sin_addr;
	mreq.imr_ifindex = if_index;
	*/

	fdd_mreq.imr_multiaddr = addr.sin_addr;
	fdd_mreq.imr_interface = /*my_addr.sin_addr*/ local_addr;

	if (setsockopt(s, IPPROTO_IP, IP_DROP_MEMBERSHIP, &fdd_mreq, sizeof(fdd_mreq)) < 0)
	{
	    fprintf(stderr, "Error leaving multicast group: %s\n", strerror(errno));
	}
    }
    close(s);
    closelog();
    return EXIT_SUCCESS;
}

static void sighandler(int signum)
{
    switch (signum)
    {
    case SIGINT:
    case SIGTERM:
        terminate = 1;
        break;
    case SIGUSR1:
        show_status = 1;
        break;
    }
}

static int ts_fprintf(FILE *f, struct timeval *tv, const char *format, ...)
{
    va_list ap;
    struct tm *dt;
    int offset;
    struct timeval now;

    if (tv == NULL)
    {
	gettimeofday(&now, NULL);
        tv = &now;
    }
    if (epoch)
        offset = fprintf(f, "%010d.%06lu ", (int)tv->tv_sec, tv->tv_usec);
    else
    {
/*        dt = gmtime(&tv->tv_sec);*/
	dt = localtime(&tv->tv_sec);
        offset = fprintf(f, "%4d/%02d/%02d %02d:%02d:%02d.%06lu ",
                         dt->tm_year + 1900, dt->tm_mon + 1, dt->tm_mday,
                         dt->tm_hour, dt->tm_min, dt->tm_sec, tv->tv_usec);
    }
    va_start(ap, format);
    offset += vfprintf(f, format, ap);
    va_end(ap);

    return offset;
}

static void mainloop(int s)
{
    unsigned char buf[BUFSIZE];
    ssize_t n;
    ssize_t rtp_seq = -1;
    uint32_t ssrc = 0;
    uint32_t rtp_timestamp = 0;
    uint64_t u64_pkt_delta_us = 0;
    uint64_t stats_next_report = 0;
    uint64_t stats_last_report = 0;
    struct pollfd fds[2];

    gettimeofday(&pkt_ts, NULL);
    stats_next_report = (uint64_t)pkt_ts.tv_sec * (uint64_t)1000000 + (uint64_t)pkt_ts.tv_usec + (uint64_t)1000000 * (uint64_t)stats;

    while (!terminate)
    {
        fds[0].fd = s;
        fds[0].events = POLLIN | POLLERR | POLLPRI;
        fds[1].fd = STDIN_FILENO;
        fds[1].events = POLLERR;
/*        fprintf(stderr, "[FDD] Entering poll\n");*/
        if (poll(fds, 2, 100) < 0)
        {
            if (errno != EINTR)
            {
                fprintf(stderr, "poll(): %m\n");
                break;
            }
            if (terminate)
            {
                fprintf(stderr, "\n");
                break;
            }
        }
       	gettimeofday(&pkt_ts, NULL);
	u64_pkt_ts = (uint64_t)pkt_ts.tv_sec * (uint64_t)1000000 + (uint64_t)pkt_ts.tv_usec;
	u64_pkt_delta_us = u64_pkt_ts - u64_prev_pkt_ts;

	if (stats && u64_pkt_ts >= stats_next_report)
	{
	    total_rx_pkts += (uint64_t)stats_rx_pkts;
	    total_rx_bytes += (uint64_t)stats_rx_bytes;
	    total_rtp_lost += (uint64_t)stats_rtp_lost;
	    total_rtp_latency += (uint64_t)stats_rtp_latency;
		/* Compute rtp lost ratio on basis of estimated rx packets = actual rx packets + lost packets */
	    if (stats_rx_pkts)
		stats_rtp_lost_ratio = (double)stats_rtp_lost * 100.0 / ((double)stats_rx_pkts + (double)stats_rtp_lost);
	    if (total_rx_pkts)
		total_rtp_lost_ratio = (double)total_rtp_lost * 100.0/ ((double)total_rx_pkts + (double)total_rtp_lost);
	    stats_rtp_bw = (double)stats_rx_bytes * 8.0 * 1000000.0 /(((double)u64_pkt_ts - (double)stats_last_report) * 1000.0);
/*	    ts_fprintf(stdout, NULL, "typ=REPORT,RXPKTS=%d,RXBYTES=%d,LOST=%d,LATENCY=%d,LOSTRATIO[%%]=%lf,TOTLOSTRATIO[%%]=%lf,BW=%lf\n", stats_rx_pkts, stats_rx_bytes, stats_rtp_lost, stats_rtp_latency, stats_rtp_lost_ratio, total_rtp_lost_ratio, stats_rtp_bw);*/
/*	    ts_fprintf(stdout, NULL, "REPORT %d %d %d %d %lf %lf %lf\n", stats_rx_pkts, stats_rx_bytes, stats_rtp_lost, stats_rtp_latency, stats_rtp_lost_ratio, total_rtp_lost_ratio, stats_rtp_bw);*/
	    syslog(LOG_USER|LOG_NOTICE, "REPORT %d %d %d %d %lf %lf %lf\n", stats_rx_pkts, stats_rx_bytes, stats_rtp_lost, stats_rtp_latency, stats_rtp_lost_ratio, total_rtp_lost_ratio, stats_rtp_bw);
	    fflush(stdout);
	    stats_rx_pkts = 0;
	    stats_rx_bytes = 0;
	    stats_rtp_lost = 0;
	    stats_rtp_latency = 0;
	    stats_last_report = u64_pkt_ts;
	    stats_next_report = u64_pkt_ts + (uint64_t)1000000 * (uint64_t)stats;
	}
	if (!fds[0].revents & POLLIN) {
//            fprintf(stderr, "[FDD] POLLIN not set [revents=0x%08X]\n", fds[0].revents);
            continue;
        }

	n = recv(s, buf, sizeof(buf), MSG_DONTWAIT);

	if (n < 0)
	{
//	    fprintf(stderr, "Error in recv() from (%s:%hu): %m\n",
//		    inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
//            break;
            continue;
	}
        if (n == 0) {
	    fprintf(stderr, "[FDD] Recv 0 bytes revents=0x%08X\n", fds[0].revents);
	    break;
        }
	stats_rx_pkts++;
	stats_rx_bytes += n;
	if (rtp_check)
        {
            ssize_t new_rtp_seq;
            uint32_t new_ssrc, csrc, new_rtp_timestamp, ts_delta, ts_delta_us;
            unsigned int pt, cc;
	    int latency_factor = rtp_latency == 0 ? 2 : rtp_latency;

	    if (n < 12)
                ts_fprintf(stderr, NULL, "read %d bytes: too short for RTP header\n", n);
            else
            {
                new_rtp_seq = (buf[2] << 8) + buf[3];
                new_ssrc = (((((buf[8] << 8) | buf[9]) << 8) | buf[10]) << 8) | buf[11];
                new_rtp_timestamp = (((((buf[4] << 8) | buf[5]) << 8) | buf[6]) << 8) | buf[7];
		ts_delta = new_rtp_timestamp - rtp_timestamp;
		/* RTP TS clock is at 90KHz */
		ts_delta_us = ts_delta * 1000 / 90;
                pt = buf[1] & 0x7f;
                cc = buf[0] & 0x0f;
                if (rtp_seq < 0 || new_ssrc != ssrc)
                {
                    unsigned int i;

                    ts_fprintf(stderr, NULL, "#%05d SSRC=0x%08x CC=%-2u PT=%-3u start\n", new_rtp_seq, new_ssrc, cc, pt);
                    /* FIXME: assuming no profile-specific extension headers */
                    if ((unsigned int)n < 12 + 4 * cc)
                        ts_fprintf(stderr, NULL, "read %d bytes: too short for RTP header with CC=%u\n", n, cc);
                    else for (i = 12; i < 12 + 4 * cc; i += 4)
                    {
                        csrc = (((((buf[i] << 8) | buf[i + 1]) << 8) | buf[i + 2]) << 8) | buf[i + 3];
                        ts_fprintf(stderr, NULL, "       CSRC=0x%08x%s\n", csrc, csrc == new_ssrc ? " *" : "");
                    }


		    /* Print and close prev SSRC stats */
		    total_rx_pkts += (uint64_t)stats_rx_pkts;
		    total_rx_bytes += (uint64_t)stats_rx_bytes;
		    total_rtp_lost += (uint64_t)stats_rtp_lost;
		    total_rtp_latency += (uint64_t)stats_rtp_latency;
			/* Compute rtp lost ratio on basis of estimated rx packets = actual rx packets + lost packets */
		    if (stats_rx_pkts)
			stats_rtp_lost_ratio = (double)stats_rtp_lost * 100.0 /((double)stats_rx_pkts + (double)stats_rtp_lost);
		    if (total_rx_pkts)
			total_rtp_lost_ratio = (double)total_rtp_lost * 100.0/ ((double)total_rx_pkts + (double)total_rtp_lost);
		    stats_rtp_bw = (double)stats_rx_bytes * 8.0 * 1000000.0 /(((double)u64_pkt_ts - (double)stats_last_report) * 1000.0);
		    syslog(LOG_USER|LOG_NOTICE, "REPORT %d %d %d %d %lf %lf %lf\n", stats_rx_pkts, stats_rx_bytes, stats_rtp_lost, stats_rtp_latency, stats_rtp_lost_ratio, total_rtp_lost_ratio, stats_rtp_bw);
/*		    ts_fprintf(stdout, NULL, "typ=REPORT,RXPKTS=%d,RXBYTES=%d,LOST=%d,LATENCY=%d,LOSTRATIO[%%]=%lf,TOTLOSTRATIO[%%]=%lf,BW=%lf\n", stats_rx_pkts, stats_rx_bytes, stats_rtp_lost, stats_rtp_latency, stats_rtp_lost_ratio, total_rtp_lost_ratio, stats_rtp_bw);*/
		    fflush(stdout);

		    /* Init new SSRC stats */
		    stats_rx_pkts = 0;
		    stats_rx_bytes = 0;
    		    stats_rtp_lost = 0;
		    stats_rtp_latency = 0;
		    stats_last_report = u64_pkt_ts;
		    stats_next_report = u64_pkt_ts + (uint64_t)1000000 * (uint64_t)stats;
	         }
                else if (!(((new_rtp_seq == 0) && (rtp_seq == 65535)) ||
                        new_rtp_seq == rtp_seq + 1))
		{
			ssize_t lost;

			if (new_rtp_seq < rtp_seq) {
			    lost = (65535 - rtp_seq) + new_rtp_seq - 1;
           		    ts_fprintf(stderr, &pkt_ts, "#%05d SSRC=0x%08x discontinuity/reorder, last seen #%05d SSRC=0x%08x, lost: %5d\n",
                                   new_rtp_seq, new_ssrc, rtp_seq, ssrc, lost);
			} else {
			    lost = new_rtp_seq - rtp_seq - 1;
           		    ts_fprintf(stderr, &pkt_ts, "#%05d SSRC=0x%08x discontinuity, last seen #%05d SSRC=0x%08x, lost: %5d\n",
                                   new_rtp_seq, new_ssrc, rtp_seq, ssrc, lost);
			}
			stats_rtp_lost += lost;
		}
		if (rtp_seq >= 0 && u64_pkt_delta_us > (latency_factor * (uint64_t)ts_delta_us))
		{
		    ts_fprintf(stderr, &pkt_ts, "#%05d latency warning, RTP-TS=%013u RTP-TS-DELTA=%05u RTP_TS_DELTA_US=%013u PKT_TS_DELTA=%06u\n", new_rtp_seq, new_rtp_timestamp, ts_delta, ts_delta_us, u64_pkt_delta_us);
		    stats_rtp_latency++;
		}
		if (rtp_ival)
		{
			if (new_rtp_timestamp > rtp_timestamp)
			{
                    		ts_fprintf(stderr, &pkt_ts, "#%05d RTP-TS=%013u RTP-TS-DELTA=%05u RTP_TS_DELTA_US=%013u PKT_TS_DELTA=%06u\n", new_rtp_seq, new_rtp_timestamp, ts_delta, ts_delta_us, u64_pkt_delta_us);
			}
			else
			{
                    		ts_fprintf(stderr, &pkt_ts, "#%05d RTP-TS=%013u PREV-RTP-TS=%013u PKT_TS_DELTA=%06u\n", new_rtp_seq, new_rtp_timestamp, rtp_timestamp, u64_pkt_ts - u64_prev_pkt_ts);
			}
		}
                rtp_seq = new_rtp_seq;
		rtp_timestamp = new_rtp_timestamp;
		u64_prev_pkt_ts = u64_pkt_ts;
		/*
		prev_pkt_ts.tv_sec = pkt_ts.tv_sec;
		prev_pkt_ts.tv_usec = pkt_ts.tv_usec;
		*/
                ssrc = new_ssrc;
                if (show_status)
                {
                    ts_fprintf(stderr, NULL, "#%05d TS=%013u SSRC=0x%08x\n", rtp_seq, rtp_timestamp, ssrc);
                    show_status = 0;
                }
            }
        }
       if (!quiet)
        {
            if (ts_hexdump)
	        hexdump(buf, n);
	    else
            {
                fwrite(buf, n, 1, stdout);
                fflush(stdout);
            }
        }
    }
}

static int parse_args(int argc, char *argv[])
{
    static const struct option options[] = {
        { "hex",     no_argument,       &ts_hexdump, 'x'},
        { "quiet",   no_argument,       &quiet,      'q'},
        { "ival",    no_argument,       &rtp_ival,   'a'},
        { "rtp",     no_argument,       &rtp_check,  'r'},
        { "stats",   required_argument, NULL,        's'},
        { "epoch",   no_argument,       &epoch,      'e'},
        { "latency", required_argument, NULL,        'l'},
        { "if",      required_argument, NULL,        'i'},
        { "local",   required_argument, NULL,        'L'},
        { "help",    no_argument,       NULL,        '?'},
        { 0, 0, 0, 0 }
    };
    int c;
    struct hostent *he;
    int s;
    struct ifreq ifr;
    char *address, *port;

    while ((c = getopt_long(argc, argv, "W;xqeari:?l:s:L:", options, NULL)) != -1)
    {
        switch (c)
        {
        case 'x':
            ts_hexdump = c;
            break;
        case 'q':
            quiet = c;
            break;
        case 'e':
            epoch = c;
            break;
        case 'a':
            rtp_ival = c;
            break;
         case 'r':
            rtp_check = c;
            break;
         case 's':
            stats = atoi(optarg);
            break;
        case 0:
            break;
        case 'i':
            if_name = optarg;
            break;
        case 'L':
            if (inet_aton(optarg, &local_addr) == 0)
		    local_addr.s_addr = INADDR_ANY;
            break;
	case 'l':
	    rtp_latency = atoi(optarg);
	    break;
        case '?':
        default:
            return 1;
        }
        //printf("optind = %d\n", optind);
    }

    if (optind + 2 > argc)
        return 1;
    address = argv[optind++];
    port = argv[optind++];

    he = gethostbyname(address);
    if (he == NULL)
    {
	fprintf(stderr, "Failed to resolve \"%s\": %s\n", argv[1], strerror(h_errno));
	return 1;
    }
    addr.sin_family = he->h_addrtype;
    addr.sin_port = htons(atoi(port));
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if ((addr.sin_family == AF_INET) && IN_MULTICAST(ntohl(addr.sin_addr.s_addr)))
    {
        use_multicast = 1;

	if (if_name)
	{
	    if_index = if_nametoindex(if_name);

	    if (if_index == 0)
	    {
		fprintf(stderr, "No such interface: %s\n", if_name);
		return 1;
	    }
	}
	else
	{
	    struct if_nameindex *nidx;

	    nidx = if_nameindex();
	    if (nidx[0].if_index == 0)
	    {
		fprintf(stderr, "Failed to find any network interfaces.\n");
		return 1;
	    }
	    if_index = nidx[0].if_index;
	    if_name = nidx[0].if_name;
	}

	if ((s = socket(PF_INET, SOCK_DGRAM, 0)) < 0)
	{
	    perror("socket");
	    return 1;
	}
	memset(&ifr, 0, sizeof(struct ifreq));
	strcpy(ifr.ifr_name, if_name);
	if (ioctl(s, SIOCGIFDSTADDR, &ifr) < 0)
	{
	    fprintf(stderr, "Failed to find IP address for interface %s: %m\n", if_name);
	    close(s);
	    return 1;
	}
	close(s);

	my_addr = *(struct sockaddr_in *)&ifr.ifr_addr;
	if (my_addr.sin_family == 0xFFFF || my_addr.sin_family == 0)
	{
	    fprintf(stderr, "Failed to find IP address for interface %s: unknown or invalid address family\n",
		    if_name);
	    return 1;
	}
    }
    else
    {
	my_addr.sin_family = AF_INET;
	my_addr.sin_addr.s_addr = INADDR_ANY;
    }

    return 0;
}

static void show_usage(const char *name)
{
    char *cmd = basename(name);

    fprintf(stderr, "Usage: %s [options] <ip address> <port>\n", cmd);
    fprintf(stderr, "  -i|--if              network interface name\n");
    fprintf(stderr, "  -L|--local           local network interface address\n");
    fprintf(stderr, "  -r|--rtp             follow and check RTP sequence numbers\n");
    fprintf(stderr, "  -q|--quiet           do not dump stream data on stdout\n");
    fprintf(stderr, "  -x|--hexdump         dump stream data on stdout as a hex/ASCCI dump\n");
    fprintf(stderr, "  -e|--epoch           show timestamps as seconds since epoch\n");
    fprintf(stderr, "  -a|--ival            show timestamp interval between RTP packets\n");
    fprintf(stderr, "  -l|--latency         set latency factor for warning (between rcv pkts - default 2)\n");
    fprintf(stderr, "  -s|--stats           enable stat reports (every x sec)\n");
    fprintf(stderr, "  -?|--help            print this message and exit\n");
    fprintf(stderr, "Signals:\n");
    fprintf(stderr, "  SIGUSR1              in RTP mode, print next packet's info\n");
}

static void hexdump(const unsigned char *buffer, size_t len)
{
    unsigned int i;
    int j, k;

    printf("data length: %d = 0x%x\n", (int)len, (int)len);

    for (i = 0, j = 0; i < len; i++, j++)
    {
        if (i % 16 == 0)
            printf("%04x  ", i);
        printf("%02x ", ((int)buffer[i] & 0xff));
        if (j >= 15)
        {
            for ( ; j >= 0; j--)
                printf("%c",
                       ((buffer[i - j] >= ' ') && (buffer[i - j] <= '~')) ?
                       buffer[i - j] : '.');
            printf ("\n");
        }
    }
    if (i % 16 != 0)
    {
        for (k = j; k <= 15; k++)
            printf("   ");
        for (k = j ; k > 0; k--)
            printf("%c",
                   ((buffer[i - k] >= ' ') && (buffer[i - k] <= '~')) ?
                   buffer[i - k] : '.');
        for (k = j ; k <= 15; k++)
            printf(" ");
    }
    printf("\n");
    fflush(stdout);
}

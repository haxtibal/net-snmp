/*
 * snmpd.c - rrespond to SNMP queries from management stations
 *
 */
/***********************************************************
	Copyright 1988, 1989 by Carnegie Mellon University

                      All Rights Reserved

Permission to use, copy, modify, and distribute this software and its 
documentation for any purpose and without fee is hereby granted, 
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in 
supporting documentation, and that the name of CMU not be
used in advertising or publicity pertaining to distribution of the
software without specific, written prior permission.  

CMU DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
CMU BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
SOFTWARE.
******************************************************************/
#include <config.h>

#include <stdio.h>
#include <errno.h>
#if HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/types.h>
#if HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#if HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#if HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#if HAVE_NET_IF_H
#include <net/if.h>
#endif
#if HAVE_INET_MIB2_H
#include <inet/mib2.h>
#endif
#if HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#if HAVE_SYS_FILE_H
#include <sys/file.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <sys/wait.h>
#include <signal.h>

#ifndef FD_SET
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
typedef long    fd_mask;
#define NFDBITS (sizeof(fd_mask) * NBBY)        /* bits per mask */
#define FD_SET(n, p)    ((p)->fds_bits[(n)/NFDBITS] |= (1 << ((n) % NFDBITS)))
#define FD_CLR(n, p)    ((p)->fds_bits[(n)/NFDBITS] &= ~(1 << ((n) % NFDBITS)))
#define FD_ISSET(n, p)  ((p)->fds_bits[(n)/NFDBITS] & (1 << ((n) % NFDBITS)))
#define FD_ZERO(p)      memset((p), 0, sizeof(*(p)))
#endif

#include "asn1.h"
#include "snmp_api.h"
#include "snmp_impl.h"
#include "system.h"
#include "read_config.h"
#include "snmp.h"
#include "mib.h"
#include "m2m.h"
#include "snmp_vars.h"
#include "agent_read_config.h"
#include "snmpv3.h"
#include "callback.h"
#include "snmp_alarm.h"
#include "default_store.h"
#include "mib_module_config.h"

#include "snmp_client.h"
#include "snmpd.h"
#include "var_struct.h"
#include "mibgroup/struct.h"
#include "mibgroup/util_funcs.h"
#include "snmp_debug.h"

#include "snmpusm.h"
#include "tools.h"
#include "lcd_time.h"

#include "transform_oids.h"

#include "snmp_agent.h"
#include "agent_trap.h"
#include "ds_agent.h"
#include "agent_read_config.h"
#include "snmp_logging.h"

#include "version.h"

#include "mib_module_includes.h"

/*
 * Globals.
 */
#ifdef USE_LIBWRAP
#include <tcpd.h>

int allow_severity	 = LOG_INFO;
int deny_severity	 = LOG_WARNING;
#endif  /* USE_LIBWRAP */

#define TIMETICK         500000L
#define ONE_SEC         1000000L

int 		log_addresses	 = 0;
int 		snmp_dump_packet;
int             running          = 1;
int		reconfig	 = 0;

struct addrCache {
    in_addr_t	addr;
    int		status;
#define UNUSED	0
#define USED	1
#define OLD	2
};

#define ADDRCACHE 10

static struct addrCache	addrCache[ADDRCACHE];
static int		lastAddrAge = 0;

extern char **argvrestartp;
extern char  *argvrestart;
extern char  *argvrestartname;

#define NUM_SOCKETS	32

#ifdef USING_SD_HANDLERS
static int	  sdlist[NUM_SOCKETS],
		  sdlen = 0;
int		(*sd_handlers[NUM_SOCKETS]) (int);
#endif

/*
 * Prototypes.
 */
int snmp_read_packet (int);
int snmp_input (int, struct snmp_session *, int, struct snmp_pdu *, void *);
static void usage (char *);
int main (int, char **);
static void SnmpTrapNodeDown (void);
static int receive(void);
int snmp_check_packet(struct snmp_session*, snmp_ipaddr);
int snmp_check_parse(struct snmp_session*, struct snmp_pdu*, int);

static void usage(char *prog)
{
	printf("\nUsage:  %s [-h] [-v] [-f] [-a] [-d] [-V] [-P PIDFILE] [-q] [-D] [-p NUM] [-L] [-l LOGFILE]",prog);
#if HAVE_UNISTD_H
	printf(" [-u uid] [-g gid]");
#endif
	printf("\n");
	printf("\n\tVersion:  %s\n",VersionInfo);
	printf("\tAuthor:   Wes Hardaker\n");
	printf("\tEmail:    ucd-snmp-coders@ucd-snmp.ucdavis.edu\n");
	printf("\n-h\t\tThis usage message.\n");
	printf("-H\t\tDisplay configuration file directives understood.\n");
	printf("-v\t\tVersion information.\n");
	printf("-f\t\tDon't fork from the shell.\n");
	printf("-a\t\tLog addresses.\n");
	printf("-d\t\tDump sent and received UDP SNMP packets\n");
	printf("-V\t\tVerbose display\n");
	printf("-P PIDFILE\tUse PIDFILE to store process id\n");
	printf("-q\t\tPrint information in a more parsable format (quick-print)\n");
	printf("-D\t\tTurn on debugging output\n");
	printf("-p NUM\t\tRun on port NUM instead of the default:  161\n");
	printf("-c CONFFILE\tRead CONFFILE as a configuration file.\n");
	printf("-C\t\tDon't read the default configuration files.\n");
	printf("-L\t\tPrint warnings/messages to stdout/err\n");
	printf("-s\t\tLog warnings/messages to syslog\n");
	printf("-A\t\tAppend to the logfile rather than truncating it.\n");
	printf("-l LOGFILE\tPrint warnings/messages to LOGFILE\n");
	printf("\t\t(By default LOGFILE=%s)\n",
#ifdef LOGFILE
			LOGFILE
#else
			"none"
#endif
	      );
#if HAVE_UNISTD_H
	printf("-g \t\tChange to this gid after opening port\n");
	printf("-u \t\tChange to this uid after opening port\n");
#endif
	printf("\n");
	exit(1);
}

	RETSIGTYPE
SnmpdShutDown(int a)
{
	running = 0;
}

#ifdef SIGHUP
	RETSIGTYPE
SnmpdReconfig(int a)
{
	reconfig = 1;
	signal(SIGHUP, SnmpdReconfig);
}
#endif

	static void
SnmpTrapNodeDown(void)
{
    send_easy_trap (SNMP_TRAP_ENTERPRISESPECIFIC, 2);
    /* XXX  2 - Node Down #define it as NODE_DOWN_TRAP */
}

/*******************************************************************-o-******
 * main
 *
 * Parameters:
 *	 argc
 *	*argv[]
 *      
 * Returns:
 *	0	Always succeeds.  (?)
 *
 *
 * Setup and start the agent daemon.
 *
 * Also successfully EXITs with zero for some options.
 */
	int
main(int argc, char *argv[])
{
	int             arg, i;
	int             ret;
	u_short         dest_port = SNMP_PORT;
	int             dont_fork = 0;
	char            logfile[SNMP_MAXBUF_SMALL];
	char           *cptr, **argvptr;
	struct usmUser *user, *userListPtr;
	char           *pid_file = NULL;
	FILE           *PID;
	int             dont_zero_log = 0;
	int             stderr_log=0, syslog_log=0;
	int             uid=0, gid=0;

	logfile[0]		= 0;

#ifdef LOGFILE
	strcpy(logfile, LOGFILE);
#endif


	/*
	 * usage: snmpd
	 */
	for (arg = 1; arg < argc; arg++)
          {
            if (argv[arg][0] == '-') {
              switch (argv[arg][1]) {

                case 'c':
                  if (++arg == argc)
                    usage(argv[0]);
                  ds_set_string(DS_LIBRARY_ID, DS_LIB_OPTIONALCONFIG,
                                 argv[arg]);
                  break;

                case 'C':
                    ds_set_boolean(DS_LIBRARY_ID, DS_LIB_DONT_READ_CONFIGS, 1);
                    break;

		case 'd':
                    snmp_set_dump_packet(++snmp_dump_packet);
		    ds_set_boolean(DS_APPLICATION_ID, DS_AGENT_VERBOSE, 1);
		    break;

		case 'q':
		    snmp_set_quick_print(1);
		    break;

		case 'D':
                    debug_register_tokens(&argv[arg][2]);
		    snmp_set_do_debugging(1);
		    break;

                case 'p':
                  if (++arg == argc)
                    usage(argv[0]);
                  dest_port = atoi(argv[arg]);
                  if (dest_port <= 0)
                    usage(argv[0]);
                  break;

                case 'P':
                  if (++arg == argc)
                    usage(argv[0]);
                  pid_file = argv[arg];

                case 'a':
                      log_addresses++;
                  break;

                case 'V':
                  ds_set_boolean(DS_APPLICATION_ID, DS_AGENT_VERBOSE, 1);
                  break;

                case 'f':
                  dont_fork = 1;
                  break;

                case 'l':
                  if (++arg == argc)
                    usage(argv[0]);
                  strcpy(logfile, argv[arg]);
                  break;

                case 'L':
		    stderr_log=1;
                    break;
		case 's':
		    syslog_log=1;
		    break;
                case 'A':
                    dont_zero_log = 1;
                    break;
#if HAVE_UNISTD_H
                case 'u':
                  if (++arg == argc) usage(argv[0]);
                  uid = atoi(argv[arg]);
                  break;
                case 'g':
                  if (++arg == argc) usage(argv[0]);
                  gid = atoi(argv[arg]);
                  break;
#endif
                case 'h':
                  usage(argv[0]);
                  break;
                case 'H':
                  init_snmpv3("snmpd");
                  init_agent();            /* register our .conf handlers */
                  register_mib_handlers(); /* snmplib .conf handlers */
                  fprintf(stderr, "Configuration directives understood:\n");
                  read_config_print_usage("  ");
                  exit(0);
                case 'v':
                  printf("\nUCD-snmp version:  %s\n",VersionInfo);
                  printf("Author:            Wes Hardaker\n");
                  printf("Email:             ucd-snmp-coders@ucd-snmp.ucdavis.edu\n\n");
                  exit (0);
                case '-':
                  switch(argv[arg][2]){
                    case 'v': 
                      printf("\nUCD-snmp version:  %s\n",VersionInfo);
                      printf("Author:            Wes Hardaker\n");
                      printf("Email:             ucd-snmp-coders@ucd-snmp.ucdavis.edu\n\n");
                      exit (0);
                    case 'h':
                      usage(argv[0]);
                      exit(0);
                  }

                default:
                  printf("invalid option: %s\n", argv[arg]);
                  usage(argv[0]);
                  break;
              }
              continue;
            }
	}  /* end-for */


	/* 
	 * Initialize a argv set to the current for restarting the agent.
	 */
	argvrestartp = (char **) malloc((argc + 2) * sizeof(char *));
	argvptr = argvrestartp;
	for (i = 0, ret = 1; i < argc; i++) {
		ret += strlen(argv[i]) + 1;
	}
	argvrestart = (char *) malloc(ret);
	argvrestartname = (char *) malloc(strlen(argv[0]) + 1);
	strcpy(argvrestartname, argv[0]);
	if ( strstr(argvrestartname, "agentxd") != NULL)
          ds_set_boolean(DS_APPLICATION_ID, DS_AGENT_ROLE, SUB_AGENT);
	else
          ds_set_boolean(DS_APPLICATION_ID, DS_AGENT_ROLE, MASTER_AGENT);
	for (cptr = argvrestart, i = 0; i < argc; i++) {
		strcpy(cptr, argv[i]);
		*(argvptr++) = cptr;
		cptr += strlen(argv[i]) + 1;
	}
	*cptr = 0;
	*argvptr = NULL;


	/* 
	 * Open the logfile if necessary.
	 */

	/* Should open logfile and/or syslog based on arguments */
	if (logfile[0])
		snmp_enable_filelog(logfile, dont_zero_log);
	if (syslog_log)
		snmp_enable_syslog(); 
#ifdef BUFSIZ
	setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
#endif
    /* 
     * Initialize the world.  Detach from the shell.
     * Create initial user.
     */
    if (!dont_fork && fork() != 0) {
      exit(0);
    }

    if (pid_file != NULL) {
      if ((PID = fopen(pid_file, "w")) == NULL) {
        snmp_log_perror("fopen");
        exit(1);
      }
      fprintf(PID, "%d\n", (int)getpid());
      fclose(PID);
    }

    init_agent();		/* do what we need to do first. */

    /* start library */
    init_snmp("snmpd");

    init_master_agent( dest_port,
                       snmp_check_packet,
                       snmp_check_parse );

#ifdef SIGTERM
    signal(SIGTERM, SnmpdShutDown);
#endif
#ifdef SIGINT
    signal(SIGINT, SnmpdShutDown);
#endif
#ifdef SIGHUP
    signal(SIGHUP, SnmpdReconfig);
#endif

    /* send coldstart trap via snmptrap(1) if possible */
    send_easy_trap (0, 0);
        
#if HAVE_UNISTD_H
	if (gid) {
		DEBUGMSGTL(("snmpd", "Changing gid to %d.\n", gid));
		if (setgid(gid)==-1) {
			snmp_log_perror("setgid failed");
			exit(1);
		}
	}
	if (uid) {
		DEBUGMSGTL(("snmpd", "Changing uid to %d.\n", uid));
		if(setuid(uid)==-1) {
			snmp_log_perror("setuid failed");
			exit(1);
		}
	}
#endif

	/* honor selection of standard error output */
	if (!stderr_log)
		snmp_disable_stderrlog();

	/* we're up, log our version number */
	snmp_log(LOG_INFO, "UCD-SNMP version %s\n", VersionInfo);

	memset(addrCache, 0, sizeof(addrCache));
	/* 
	 * Forever monitor the dest_port for incoming PDUs.
	 */
	DEBUGMSGTL(("snmpd", "We're up.  Starting to process data.\n"));
	receive();
#include "mib_module_shutdown.h"
	DEBUGMSGTL(("snmpd", "sending shutdown trap\n"));
	SnmpTrapNodeDown();
	DEBUGMSGTL(("snmpd", "Bye...\n"));
	snmp_shutdown("snmpd");
	return 0;

}  /* End main() -- snmpd */

/*******************************************************************-o-******
 * receive
 *
 * Parameters:
 *      
 * Returns:
 *	0	On success.
 *	-1	System error.
 *
 * Infinite while-loop which monitors incoming messges for the agent.
 * Invoke the established message handlers for incoming messages on a per
 * port basis.  Handle timeouts.
 */
	static int
receive(void)
{
    int numfds;
    fd_set fdset;
    struct timeval	timeout, *tvp = &timeout;
    struct timeval	sched,   *svp = &sched,
			now,     *nvp = &now;
    int count, block;


    /*
     * Set the 'sched'uled timeout to the current time + one TIMETICK.
     */
    gettimeofday(nvp, (struct timezone *) NULL);
    svp->tv_usec = nvp->tv_usec + TIMETICK;
    svp->tv_sec = nvp->tv_sec;
    
    while (svp->tv_usec >= ONE_SEC){
	svp->tv_usec -= ONE_SEC;
	svp->tv_sec++;
    }

    /*
     * Loop-forever: execute message handlers for sockets with data,
     * reset the 'sched'uler.
     */
    while (running) {
        if (reconfig) {
          reconfig = 0;
          snmp_log(LOG_INFO, "Reconfiguring daemon\n");
          update_config();
        }
	tvp =  &timeout;
	tvp->tv_sec = 0;
	tvp->tv_usec = TIMETICK;

	numfds = 0;
	FD_ZERO(&fdset);
        block = 0;
        snmp_select_info(&numfds, &fdset, tvp, &block);
        if (block == 1)
            tvp = NULL; /* block without timeout */
	count = select(numfds, &fdset, 0, 0, tvp);

	if (count > 0){
	    snmp_read(&fdset);
	} else switch(count){
	    case 0:
                snmp_timeout();
                break;
	    case -1:
		if (errno == EINTR){
		    continue;
		} else {
                    snmp_log_perror("select");
		}
		return -1;
	    default:
		snmp_log(LOG_ERR, "select returned %d\n", count);
		return -1;
	}  /* endif -- count>0 */


        /*
         * If the time 'now' is greater than the 'sched'uled time, then:
         *
         *    Check alarm and event timers if v2p is configured.
         *    Reset the 'sched'uled time to current time + one TIMETICK.
         *    Age the cache network addresses (from whom messges have
         *        been received).
         */
        gettimeofday(nvp, (struct timezone *) NULL);

        if (nvp->tv_sec > svp->tv_sec
                || (nvp->tv_sec == svp->tv_sec && nvp->tv_usec > svp->tv_usec)){
            svp->tv_usec = nvp->tv_usec + TIMETICK;
            svp->tv_sec = nvp->tv_sec;

            while (svp->tv_usec >= ONE_SEC){
                svp->tv_usec -= ONE_SEC;
                svp->tv_sec++;
            }
            if (log_addresses && lastAddrAge++ > 600){

                lastAddrAge = 0;
                for(count = 0; count < ADDRCACHE; count++){
                    if (addrCache[count].status == OLD)
                        addrCache[count].status = UNUSED;
                    if (addrCache[count].status == USED)
                        addrCache[count].status = OLD;
                }
            }
        }  /* endif -- now>sched */
    }  /* endwhile */

    snmp_log(LOG_INFO, "Received TERM or STOP signal...  shutting down...\n");
    return 0;

}  /* end receive() */




/*******************************************************************-o-******
 * snmp_check_packet
 *
 * Parameters:
 *	session, from
 *      
 * Returns:
 *	1	On success.
 *	0	On error.
 *
 * Handler for all incoming messages (a.k.a. packets) for the agent.  If using
 * the libwrap utility, log the connection and deny/allow the access. Print
 * output when appropriate, and increment the incoming counter.
 *
 */
int
snmp_check_packet(struct snmp_session *session,
  snmp_ipaddr from)
{
    struct sockaddr_in *fromIp = (struct sockaddr_in *)&from;

#ifdef USE_LIBWRAP
    char *addr_string;
    /*
     * Log the message and/or dump the message.
     * Optionally cache the network address of the sender.
     */
    addr_string = inet_ntoa(fromIp->sin_addr);

    if(!addr_string) {
      addr_string = STRING_UNKNOWN;
    }
    if(hosts_ctl("snmpd", addr_string, addr_string, STRING_UNKNOWN)) {
      snmp_log(allow_severity, "Connection from %s", addr_string);
    } else {
      snmp_log(deny_severity, "Connection from %s refused", addr_string);
      return(0);
    }
#endif	/* USE_LIBWRAP */

    snmp_increment_statistic(STAT_SNMPINPKTS);

    if (log_addresses || ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_VERBOSE)){
	int count;
	
	for(count = 0; count < ADDRCACHE; count++){
	    if (addrCache[count].status > UNUSED /* used or old */
		&& fromIp->sin_addr.s_addr == addrCache[count].addr)
		break;
	}

	if (count >= ADDRCACHE ||
            ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_VERBOSE)){
	    DEBUGMSGTL(("snmpd", "Received SNMP packet(s) from %s\n",
                        inet_ntoa(fromIp->sin_addr)));
	    for(count = 0; count < ADDRCACHE; count++){
		if (addrCache[count].status == UNUSED){
		    addrCache[count].addr = fromIp->sin_addr.s_addr;
		    addrCache[count].status = USED;
		    break;
		}
	    }
	} else {
	    addrCache[count].status = USED;
	}
    }

    return ( 1 );
}


int
snmp_check_parse( struct snmp_session *session,
    struct snmp_pdu     *pdu,
    int    result)
{
    if ( result == 0 ) {
        if ( ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_VERBOSE)) {
             char buf [256];
	     struct variable_list *var_ptr;
	    
	    switch (pdu->command) {
	    case SNMP_MSG_GET:
	    	snmp_log(LOG_DEBUG, "  GET message\n"); break;
	    case SNMP_MSG_GETNEXT:
	    	snmp_log(LOG_DEBUG, "  GETNEXT message\n"); break;
	    case SNMP_MSG_RESPONSE:
	    	snmp_log(LOG_DEBUG, "  RESPONSE message\n"); break;
	    case SNMP_MSG_SET:
	    	snmp_log(LOG_DEBUG, "  SET message\n"); break;
	    case SNMP_MSG_TRAP:
	    	snmp_log(LOG_DEBUG, "  TRAP message\n"); break;
	    case SNMP_MSG_GETBULK:
	    	snmp_log(LOG_DEBUG, "  GETBULK message, non-rep=%d, max_rep=%d\n",
			pdu->errstat, pdu->errindex); break;
	    case SNMP_MSG_INFORM:
	    	snmp_log(LOG_DEBUG, "  INFORM message\n"); break;
	    case SNMP_MSG_TRAP2:
	    	snmp_log(LOG_DEBUG, "  TRAP2 message\n"); break;
	    case SNMP_MSG_REPORT:
	    	snmp_log(LOG_DEBUG, "  REPORT message\n"); break;
	    }
	     
	    for ( var_ptr = pdu->variables ;
	        var_ptr != NULL ; var_ptr=var_ptr->next_variable ) {
                sprint_objid (buf, var_ptr->name, var_ptr->name_length);
                snmp_log(LOG_DEBUG, "    -- %s\n", buf);
	    }
	}
    	return 1;
    }
    return 0; /* XXX: does it matter what the return value is? */
}

/*******************************************************************-o-******
 * snmp_input
 *
 * Parameters:
 *	 op
 *	*session
 *	 requid
 *	*pdu
 *	*magic
 *      
 * Returns:
 *	1		On success	-OR-
 *	Passes through	Return from alarmGetResponse() when 
 *	  		  USING_V2PARTY_ALARM_MODULE is defined.
 *
 * Call-back function to manage responses to traps (informs) and alarms.
 * Not used by the agent to process other Response PDUs.
 */
int
snmp_input(int op,
	   struct snmp_session *session,
	   int reqid,
	   struct snmp_pdu *pdu,
	   void *magic)
{
    struct get_req_state *state = (struct get_req_state *)magic;
    
    if (op == RECEIVED_MESSAGE) {
	if (pdu->command == SNMP_MSG_GET) {
	    if (state->type == EVENT_GET_REQ) {
		/* this is just the ack to our inform pdu */
		return 1;
	    }
	}
    }
    else if (op == TIMED_OUT) {
	if (state->type == ALARM_GET_REQ) {
		/* Need a mechanism to replace obsolete SNMPv2p alarm */
	}
    }
    return 1;

}  /* end snmp_input() */

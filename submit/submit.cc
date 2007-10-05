/*
 * submit -- command-line submit program for solutions.
 *
 * Based on submit.pl by Eelco Dolstra.
 *  
 * $Id$
 *
 * Part of the DOMjudge Programming Contest Jury System and licenced
 * under the GNU GPL. See README and COPYING for details.
 *
 */

/* System/site specific config */
#include "../etc/config.h"

/* Check whether default submission method is available; bail out if not */
#if ( SUBMITCLIENT_METHOD == 1 ) && ( ENABLE_CMDSUBMIT_SERVER != 1 )
#error "Commandline default submission requested, but server not enabled."
#endif
#if ( SUBMITCLIENT_METHOD == 2 ) && ( ENABLE_WEBSUBMIT_SERVER != 1 )
#error "Webinterface default submission requested, but server not enabled."
#endif
#if ( SUBMITCLIENT_METHOD == 2 ) && ! defined( LIBCURL )
#error "Webinterface default submission requested, but libcURL not available."
#endif
#if ( SUBMITCLIENT_METHOD == 0 )
#warning "Commandline submit client disabled."
#endif
#if ( SUBMITCLIENT_METHOD < 0 ) || ( SUBMITCLIENT_METHOD > 2 )
#error "Unknown submission method requested."
#endif

/* Define {CMD,WEB}SUBMIT as available */
#if ( ENABLE_CMDSUBMIT_SERVER == 1 )
#define CMDSUBMIT 1
#endif
#if ( ENABLE_WEBSUBMIT_SERVER == 1 && defined( LIBCURL ) )
#define WEBSUBMIT 1
#endif

/* Standard include headers */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <getopt.h>
#include <termios.h>
#ifdef CMDSUBMIT
#include <netinet/in.h>
#include <netdb.h>
#endif
#ifdef WEBSUBMIT
#include <curl/curl.h>
#include <curl/easy.h>
#endif

/* C++ includes for easy string handling */
using namespace std;
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

/* Logging and error functions */
#include "lib.error.h"

/* Misc. other functions */
#include "lib.misc.h"

/* Include some functions, which are not always available */
#include "mkstemps.h"
#include "basename.h"

/* Common send/receive functions */
#include "submitcommon.h"

/* These defines are needed in 'version' */
#define DOMJUDGE_PROGRAM "DOMjudge/" DOMJUDGE_VERSION
#define PROGRAM "submit"

const int timeout_secs = 60; /* seconds before send/receive timeouts with an error */

extern int errno;

/* Variables defining logmessages verbosity to stderr/logfile */
extern int verbose;
extern int loglevel;

char *logfile;

char *progname;

int port = SUBMITPORT;

int quiet;
int use_websubmit;
int show_help;
int show_version;

struct option const long_opts[] = {
	{"problem",  required_argument, NULL,         'p'},
	{"language", required_argument, NULL,         'l'},
	{"server",   required_argument, NULL,         's'},
	{"team",     required_argument, NULL,         't'},
	{"port",     required_argument, NULL,         'P'},
	{"web",      optional_argument, NULL,         'w'},
	{"verbose",  optional_argument, NULL,         'v'},
	{"quiet",    no_argument,       NULL,         'q'},
	{"help",     no_argument,       &show_help,    1 },
	{"version",  no_argument,       &show_version, 1 },
	{ NULL,      0,                 NULL,          0 }
};

void version();
void usage();
void usage2(int , char *, ...);
void warnuser(char *);
char readanswer(char *answers);

#ifdef CMDSUBMIT
int  cmdsubmit();
#endif
#ifdef WEBSUBMIT
int  websubmit();
#endif

#ifdef CMDSUBMIT
int socket_fd; /* filedescriptor of the connection to server socket */

struct addrinfo *server_ais, *server_ai; /* server adress information */
char server_addr[NI_MAXHOST];            /* server IP address string  */
#endif

int nwarnings;

/* Submission information */
string problem, language, extension, server, team;
char *filename, *submitdir, *tempfile;
int temp_fd;

/* Language extensions */
vector<vector<string> > languages;

int main(int argc, char **argv)
{
	size_t i,j;
	int c;
	char *ptr;
	char *homedir;
	struct stat fstats;
	string filebase, fileext;
	time_t fileage;
	char *lang_exts;
	char *lang, *ext;
	char *lang_ptr, *ext_ptr;

	progname = argv[0];
	stdlog = NULL;

#if ( SUBMITCLIENT_METHOD == 0 )
	printf("Submit client is disabled at compiletime.\n");
	return -1;
#endif

	/* Parse LANGEXTS define into separate strings */
	lang_exts = strdup(LANG_EXTS);
	for(lang=strtok_r(lang_exts," ",&lang_ptr); lang!=NULL;
		lang=strtok_r(NULL," ",&lang_ptr)) {

		languages.push_back(vector<string>());
		
		/* First read the language */
		ext=strtok_r(lang,",",&ext_ptr);
		languages[languages.size()-1].push_back(string(ext));
		
		/* Then all valid extensions for that language */
		for(ext=strtok_r(NULL,",",&ext_ptr); ext!=NULL;
			ext=strtok_r(NULL,",",&ext_ptr)) {
			languages[languages.size()-1].push_back(stringtolower(ext));
		}
	}

	if ( getenv("HOME")==NULL ) error(0,"environment variable `HOME' not set");
	homedir = getenv("HOME");
	
	/* Check for USERSUBMITDIR and create it if nessary */
	submitdir = allocstr("%s/%s",homedir,USERSUBMITDIR);
	if ( stat(submitdir,&fstats)!=0 ) {
		if ( mkdir(submitdir,USERPERMDIR)!=0 ) {
			error(errno,"creating directory `%s'",submitdir);
		}
	} else {
		if ( ! S_ISDIR(fstats.st_mode) ) {
			error(0,"`%s' is not a directory",submitdir);
		}
		if ( chmod(submitdir,USERPERMDIR)!=0 ) {
			error(errno,"setting permissions on `%s'",submitdir);
		}
	}
	
	/* Set logging levels & open logfile */
	verbose  = LOG_NOTICE;
	loglevel = LOG_DEBUG;

	logfile = allocstr("%s/submit.log",submitdir);
	stdlog = fopen(logfile,"a");
	if ( stdlog==NULL ) error(errno,"cannot open logfile `%s'",logfile);

	logmsg(LOG_INFO,"started");
	
	/* Set defaults for server and team */
#ifdef SUBMITSERVER
	server = string(SUBMITSERVER);
#endif
	if ( server.empty() && getenv("SUBMITSERVER")!=NULL ) {
		server = string(getenv("SUBMITSERVER"));
	}
	if ( server.empty() ) server = string("localhost");

	if ( team.empty() && getenv("TEAM")!=NULL ) team = string(getenv("TEAM"));
	if ( team.empty() && getenv("USER")!=NULL ) team = string(getenv("USER"));
	if ( team.empty() && getenv("USERNAME")!=NULL ) {
		team = string(getenv("USERNAME"));
	}

	/* Parse command-line options */
#if ( SUBMITCLIENT_METHOD == 1 )
	use_websubmit = 0;
#else
	use_websubmit = 1;
#endif
	quiet =	show_help = show_version = 0;
	opterr = 0;
	while ( (c = getopt_long(argc,argv,"p:l:s:t:P:w::v::q",long_opts,NULL))!=-1 ) {
		switch ( c ) {
		case 0:   /* long-only option */
			break;
			
		case 'p': problem   = string(optarg); break;
		case 'l': extension = string(optarg); break;
		case 's': server    = string(optarg); break;
		case 't': team      = string(optarg); break;
			
		case 'P': /* port option */
			port = strtol(optarg,&ptr,10);
			if ( *ptr!=0 || port<0 || port>65535 ) {
				usage2(0,"invalid tcp port specified: `%s'",optarg);
			}
			break;
		case 'w': /* websubmit option */
			if ( optarg!=NULL ) {
				use_websubmit = strtol(optarg,&ptr,10);
				if ( *ptr!=0 ) usage2(0,"invalid value specified: `%s'",optarg);
			} else {
				use_websubmit = 1;
			}
			break;
		case 'v': /* verbose option */
			if ( optarg!=NULL ) {
				verbose = strtol(optarg,&ptr,10);
				if ( *ptr!=0 || verbose<0 ) {
					usage2(0,"invalid verbosity specified: `%s'",optarg);
				}
			} else {
				verbose++;
			}
			break;
		case 'q': /* quiet option */
			verbose = LOG_ERR;
			quiet = 1;
			break;
		case ':': /* getopt error */
		case '?':
			usage2(0,"unknown option or missing argument `%c'",optopt);
			break;
		default:
			error(0,"getopt returned character code `%c' ??",c);
		}
	}

	if ( show_help ) usage();
	if ( show_version ) version();
	
	if ( argc<=optind   ) usage2(0,"no filename specified");
	if ( argc> optind+1 ) usage2(0,"multiple filenames specified");
	filename = argv[optind];

	/* Stat file and do some sanity checks */
	if ( stat(filename,&fstats)!=0 ) usage2(errno,"cannot find `%s'",filename);
	logmsg(LOG_DEBUG,"submission file is %s",filename);

	nwarnings = 0;

	if ( ! (fstats.st_mode & S_IFREG) ) warnuser("file is not a regular file");
	if ( ! (fstats.st_mode & S_IRUSR) ) warnuser("file is not readable");
	if ( fstats.st_size==0 )            warnuser("file is empty");
	if ( fstats.st_size>=SOURCESIZE*1024 ) {
		ptr = allocstr("file is larger than %d kB",SOURCESIZE);
		warnuser(ptr);
		free(ptr);
	}

	if ( (fileage=(time(NULL)-fstats.st_mtime)/60)>WARN_MTIME ) {
		ptr = allocstr("file has not been modified for %d minutes",fileage);
		warnuser(ptr);
		free(ptr);
	}
	
	/* Try to parse problem and language from filename */
	filebase = string(gnu_basename(filename));
	if ( filebase.find('.')!=string::npos ) {
		fileext = filebase.substr(filebase.rfind('.')+1);
		filebase.erase(filebase.find('.'));

		/* Check for only alphanumeric characters in problem */
		for(i=0; i<filebase.length(); i++) {
			if ( ! isalnum(filebase[i]) ) break;
		}
		if ( i>=filebase.length() && filebase.length()>0 && problem.empty() ) {
			problem = filebase;
		}

		if ( extension.empty() ) extension = fileext;
	}
	
	/* Check for languages matching file extension */
	extension = stringtolower(extension);
	for(i=0; i<languages.size(); i++) {
		for(j=1; j<languages[i].size(); j++) {
			if ( languages[i][j]==extension ) {
				language  = languages[i][0];
				extension = languages[i][1];
			}
		}
	}
	
	if ( language.empty() ) {
		ptr = allocstr("language `%s' not recognised",extension.c_str());
		warnuser(ptr);
		free(ptr);
		language = extension;
	}
	
	if ( problem.empty()  ) usage2(0,"no problem specified");
	if ( language.empty() ) usage2(0,"no language specified");
	if ( team.empty()     ) usage2(0,"no team specified");
	if ( server.empty()   ) usage2(0,"no server specified");
	
	logmsg(LOG_DEBUG,"problem is `%s'",problem.c_str());
	logmsg(LOG_DEBUG,"language is `%s'",language.c_str());
	logmsg(LOG_DEBUG,"team is `%s'",team.c_str());
	logmsg(LOG_DEBUG,"server is `%s'",server.c_str());

	/* Ask user for confirmation */
	if ( ! quiet ) {
		printf("Submission information:\n");
		printf("  filename:   %s\n",filename);
		printf("  problem:    %s\n",problem.c_str());
		printf("  language:   %s\n",language.c_str());
		printf("  team:       %s\n",team.c_str());
		printf("  server:     %s\n",server.c_str());
		if ( nwarnings>0 ) printf("There are warnings for this submission!\a\n");
		printf("Do you want to continue? (y/n) ");
		c = readanswer("yn");
		printf("\n");
		if ( c=='n' ) error(0,"submission aborted by user");
	}

	if ( use_websubmit ) {
#ifdef WEBSUBMIT
		return websubmit();
#else
		error(0,"websubmit requested, but not available");
#endif
	} else {
#ifdef CMDSUBMIT
		return cmdsubmit();
#else
		error(0,"cmdsubmit requested, but not available");
#endif
	}

}

void usage()
{
	size_t i,j;
	
	printf("Usage: %s [OPTION]... FILENAME\n",progname);
	printf(
"Submit a solution for a problem.\n"
"\n"
"Options (see below for more information)\n"
"  -p, --problem=PROBLEM    submit for problem PROBLEM\n"
"  -l, --language=LANGUAGE  submit in language LANGUAGE\n"
"  -s, --server=SERVER      submit to server SERVER\n"
"  -t, --team=TEAM          submit as team TEAM\n"
#if defined( WEBSUBMIT ) && defined( CMDSUBMIT )
"  -w, --web[=0|1]          submit to the webinterface or toggle;\n"
"                               should normally not be necessary\n"
#endif
"  -v, --verbose[=LEVEL]    increase verbosity or set to LEVEL, where LEVEL\n"
"                               must be numerically specified as in 'syslog.h'\n"
"                               defaults to LOG_INFO without argument\n"
"  -q, --quiet              set verbosity to LOG_ERR and suppress user\n"
"                               input and warning/info messages\n"
"      --help               display this help and exit\n"
"      --version            output version information and exit\n\n"
"Explanation of submission options:\n"
"\n"
"For PROBLEM use the ID of the problem (letter, number or short name)\n"
"in lower- or uppercase. When not specified, PROBLEM defaults to\n"
"FILENAME excluding the extension.\n"
"For example, 'c.java' will indicate problem 'C'.\n"
"\n"
"For LANGUAGE use one of the following extensions in lower- or uppercase:\n");
	for(i=0; i<languages.size(); i++) {
		printf("   %-10s  %s",(languages[i][0]+':').c_str(),languages[i][1].c_str());
		for(j=2; j<languages[i].size(); j++) printf(", %s",languages[i][j].c_str());
		printf("\n");
	}
	printf(
"The default for LANGUAGE is the extension of FILENAME.\n"
"For example, 'c.java' wil indicate a Java solution.\n"
"\n"
"Examples:\n"
"\n");
	printf("Submit problem 'c' in Java:\n"
	       "    %s c.java\n\n",progname);
	printf("Submit problem 'e' in C++:\n"
	       "    %s --problem e --language=cpp ProblemE.cc\n\n",progname);
	printf("Submit problem 'hello' in C (options override the defaults from FILENAME):\n"
	       "    %s -p hello -l C HelloWorld.java\n\n",progname);
	printf(
"The following options should normally not be needed:\n"
"\n"
"For SERVER use the servername or IP-address of the submit-server.\n"
"The default value for SERVER is defined internally or otherwise\n"
"taken from the environment variable 'SUBMITSERVER', or 'localhost'\n"
"if 'SUBMITSERVER' is not defined.\n"
"\n"
"For TEAM use the login of the account, you want to submit for.\n"
"The default value for TEAM is taken from the environment variable\n"
"'TEAM' or your login name if 'TEAM' is not defined.\n");
	exit(0);
}

void version()
{
	printf("%s %s\n\n",DOMJUDGE_PROGRAM,PROGRAM);
	printf(
"%s comes with ABSOLUTELY NO WARRANTY.  This is free software, and you\n"
"are welcome to redistribute it under certain conditions.  See the GNU\n"
"General Public Licence for details.\n",PROGRAM);
	exit(0);
}

void usage2(int errnum, char *mesg, ...)
{
	va_list ap;
	va_start(ap,mesg);
	
	vlogerror(errnum,mesg,ap);

	va_end(ap);

	printf("Type '%s --help' to get help.\n",progname);
	exit(1);
}

void warnuser(char *warning)
{
	nwarnings++;

	logmsg(LOG_DEBUG,"user warning #%d: %s",nwarnings,warning);
	
	if ( ! quiet ) printf("WARNING: %s!\n",warning);
}

char readanswer(char *answers)
{
	struct termios old_termio, new_termio;
	char c;

	/* save the terminal settings for stdin */
	tcgetattr(STDIN_FILENO,&old_termio);
	new_termio = old_termio;

	/* disable canonical mode (buffered i/o) and local echo */
	new_termio.c_lflag &= (~ICANON & ~ECHO);
	tcsetattr(STDIN_FILENO,TCSANOW,&new_termio);

	while ( true ) {
		c = getchar();
		if ( c!=0 && (strchr(answers,tolower(c)) ||
					  strchr(answers,toupper(c))) ) {
			if ( strchr(answers,tolower(c))!=NULL ) {
				c = tolower(c);
			} else {
				c = toupper(c);
			}
			break;
		}
	}

	/* restore the saved settings */
	tcsetattr(STDIN_FILENO,TCSANOW,&old_termio);

	return c;
}

#ifdef CMDSUBMIT

int cmdsubmit()
{
	int redir_fd[3];
	char *args[MAXARGS];
	struct timeval timeout;
	struct addrinfo hints;
	char *port_str;
	int err;

	/* Make tempfile to submit */
	tempfile = allocstr("%s/%s.XXXXXX.%s",submitdir,
	                    problem.c_str(),extension.c_str());
	temp_fd = mkstemps(tempfile,extension.length()+1);
	if ( temp_fd<0 || strlen(tempfile)==0 ) {
		error(errno,"mkstemps cannot create tempfile");
	}

	/* Construct copy command and execute it */
	args[0] = filename;
	args[1] = tempfile;
	redir_fd[0] = redir_fd[1] = redir_fd[2] = 0;
	if ( execute(COPY_CMD,args,2,redir_fd,1)!=0 ) {
		error(0,"cannot copy `%s' to `%s'",filename,tempfile);
	}
	
	if ( chmod(tempfile,USERPERMFILE)!=0 ) {
		error(errno,"setting permissions on `%s'",tempfile);
	}

	logmsg(LOG_INFO,"copied `%s' to tempfile `%s'",filename,tempfile);

	/* Connect to the submission server */
	logmsg(LOG_NOTICE,"connecting to the server (%s, %d/tcp)...",
	       server.c_str(),port);
	
	/* Set preferred network connection options: use IPv4-only as
	   IPv6 does not work in Cygwin yet */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_INET;
	hints.ai_flags    = AI_CANONNAME;
	hints.ai_socktype = SOCK_STREAM;

	port_str = allocstr("%d",port);
	if ( (err = getaddrinfo(server.c_str(),port_str,&hints,&server_ais)) ) {
		error(0,"getaddrinfo: %s",gai_strerror(err));
	}
	free(port_str);

	/* Try to connect to addresses for server in given order */
	socket_fd = -1;
	for(server_ai=server_ais; server_ai!=NULL; server_ai=server_ai->ai_next) {
		
		err = getnameinfo(server_ai->ai_addr,server_ai->ai_addrlen,server_addr,
		                  sizeof(server_addr),NULL,0,NI_NUMERICHOST);
		if ( err!=0 ) error(0,"getnameinfo: %s",gai_strerror(err));

		logmsg(LOG_DEBUG,"trying to connect to address `%s'",server_addr);
	
		socket_fd = socket(server_ai->ai_family,server_ai->ai_socktype,
		                   server_ai->ai_protocol);
		if ( socket_fd>=0 ) {
			if ( connect(socket_fd,server_ai->ai_addr,server_ai->ai_addrlen)==0 ) {
				break;
			} else {
				close(socket_fd);
				socket_fd = -1;
			}
		}
	}
	if ( socket_fd<0 ) error(0,"cannot connect to the server");

	/* Set socket timeout option on read/write */
	timeout.tv_sec  = timeout_secs;
	timeout.tv_usec = 0;
	
	if ( setsockopt(socket_fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)) < 0) {
		error(errno,"setting socket option");
	}

	if ( setsockopt(socket_fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) < 0) {
		error(errno,"setting socket option");
	}

	logmsg(LOG_INFO,"connected, server address is `%s'",server_addr);

	receive(socket_fd);

	/* Send submission info */
	logmsg(LOG_NOTICE,"sending data...");
	sendit(socket_fd,"+team %s",team.c_str());
	receive(socket_fd);
	sendit(socket_fd,"+problem %s",problem.c_str());
	receive(socket_fd);
	sendit(socket_fd,"+language %s",extension.c_str());
	receive(socket_fd);
	sendit(socket_fd,"+filename %s",gnu_basename(tempfile));
	receive(socket_fd);
	sendit(socket_fd,"+done");

	/* Keep reading until end of file, then check for errors */
	while ( receive(socket_fd) );
	if ( strncasecmp(lastmesg,"done",4)!=0 ) {
		error(0,"connection closed unexpectedly");
	}

	freeaddrinfo(server_ais);
	
	logmsg(LOG_NOTICE,"submission successful");

    return 0;
}

#endif /* CMDSUBMIT */

#ifdef WEBSUBMIT

size_t writesstream(void *ptr, size_t size, size_t nmemb, void *sptr)
{
	stringstream *s = (stringstream *) sptr;
	
	*s << string((char *)ptr,size*nmemb);
	
	return size*nmemb;
}

string remove_html_tags(string s)
{
	size_t p1, p2;
	
	while ( (p1=s.find('<',0))!=string::npos ) {
		p2 = s.find('>',p1);
		if ( p2==string::npos ) break;
		s.erase(p1,p2-p1+1);
	}

	return s;
}

int websubmit()
{
	CURL *handle;
	CURLcode res;
	char curlerrormsg[CURL_ERROR_SIZE];
	struct curl_httppost *post = NULL;
	struct curl_httppost *last = NULL;
	char *url;
	stringstream curloutput;
	string line;
	size_t pos;
	int uploadstatus_read;

	url = allocstr(WEBBASEURI "team/upload.php");
	
	curlerrormsg[0] = 0;
	
	handle = curl_easy_init();

/* helper macro's to easily set curl options and fill forms */
#define curlsetopt(opt,val) \
	if ( curl_easy_setopt(handle, CURLOPT_ ## opt, val)!=CURLE_OK ) { \
		warning(0,"setting curl option '" #opt "': %s, aborting download",curlerrormsg); \
		curl_easy_cleanup(handle); \
		return 0; }
#define curlformadd(nametype,namecont,valtype,valcont) \
	if ( curl_formadd(&post, &last, \
			CURLFORM_ ## nametype, namecont, \
			CURLFORM_ ## valtype, valcont, \
			CURLFORM_END) != 0 ) \
		error(0,"libcurl could not add form field '%s'='%s'",namecont,valcont)

	/* Fill post form */
	curlformadd(COPYNAME,"code",   FILE,        filename);
	curlformadd(COPYNAME,"probid", COPYCONTENTS,problem.c_str());
	curlformadd(COPYNAME,"langext",COPYCONTENTS,extension.c_str());
	curlformadd(COPYNAME,"submit", COPYCONTENTS,"submit");

	/* Set options for post */
	curlsetopt(ERRORBUFFER,   curlerrormsg);
	curlsetopt(FAILONERROR,   1);
	curlsetopt(FOLLOWLOCATION,1);
	curlsetopt(MAXREDIRS,     10);
	curlsetopt(TIMEOUT,       timeout_secs);
	curlsetopt(URL,           url);
	curlsetopt(HTTPPOST,      post);
	curlsetopt(HTTPGET,       0);
	curlsetopt(WRITEFUNCTION, writesstream);
	curlsetopt(WRITEDATA,     (void *)&curloutput);
	curlsetopt(SSL_VERIFYPEER,0);
	curlsetopt(SSL_VERIFYHOST,0);

	if ( verbose >= LOG_DEBUG ) {
		curlsetopt(VERBOSE,   1);
	} else {
		curlsetopt(NOPROGRESS,1);
	}

	logmsg(LOG_NOTICE,"connecting to %s",url);
	
	if ( (res=curl_easy_perform(handle))!=CURLE_OK ) {
		warning(0,"downloading '%s': %s",url,curlerrormsg);
		curl_easy_cleanup(handle);
		return 0;
	}

#undef curlsetopt
#undef curlformadd

	curl_formfree(post);
	curl_easy_cleanup(handle);

	// Read curl output and find upload status
	uploadstatus_read = 0;
	while ( getline(curloutput,line) ) {

		// Search line for upload status or errors
 		if ( (pos=line.find(ERRMATCH,0))!=string::npos ) {
			error(0,"webserver returned: %s",line.substr(pos+strlen(ERRMATCH)).c_str());
 		}
		if ( line.find("uploadstatus",0)!=string::npos ) {
			line = remove_html_tags(line);
			if ( line.find("ERROR",0) !=string::npos ||
				 line.find("failed",0)!=string::npos ) {
				error(0,"webserver returned: %s",line.c_str());
			}
			logmsg(LOG_NOTICE,"webserver returned: %s",line.c_str());
			uploadstatus_read = 1;
		}
	}

	if ( ! uploadstatus_read ) error(0,"no upload status or error reported by webserver");
	
	return 0;
}
#endif /* WEBSUBMIT */

//  vim:ts=4:sw=4:

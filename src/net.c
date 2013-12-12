#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <ctype.h>
#include <time.h>
#include <poll.h>
#include <stdarg.h>
#include <arpa/inet.h>

#include "common.h"

#define RPL_WELCOME            1
#define ERR_NICKNAMEINUSE    433
#define ERR_ERRONEUSNICKNAME 432

channel* get_channel(char*);
channel* new_channel(char*);
char* cmdcasecmp(char*, char*);
char* cmdcmp(char*, char*);
char* getarg(char*);
char* getarg_after(char**, char);
int get_auto_nick(char*, char*);
int get_numeric_code(char**);
int recv_join(char*, char*);
int recv_mode(char*, char*);
int recv_nick(char*, char*);
int recv_note(char*, char*);
int recv_part(char*, char*);
int recv_priv(char*, char*);
int recv_quit(char*, char*);
int send_conn(char*);
int send_join(char*);
int send_nick(char*);
int send_pong(char*);
int send_priv(char*, int);
server* new_server(char*, int, int);
void close_channel(char*);
void con_server(char*, int);
void dis_server(server*, int);
void do_recv(int);
void newline(channel*, line_t, char*, char*, int);
void newlinef(channel*, line_t, char*, char*, ...);
void recv_000(int, char*);
void recv_200(int, char*);
void recv_400(int, char*);
void send_part(char*);
void sendf(int, const char*, ...);
void trimarg_after(char**, char);

int rplsoc;
int numfds = 1; /* 1 for stdin */
extern struct pollfd fds[MAXSERVERS + 1];
/* For server indexing by socket. 3 for stdin/out/err unused */
server *s[MAXSERVERS + 3];

/* Config Stuff */
char *user_me = "rcr";
char *realname = "Richard Robbins";
/* server to connect to automatically on startup */
char *autoconn = "";
/* comma separated list of channels to join on connect*/
char *autojoin = "#abc";
/* comma and/or space separated list of nicks */
char *nicks = "rcr, rcr_, rcr__";



time_t raw_t;
struct tm *t;

channel rirc = {
	.active = 0,
	.cur_line = 0,
	.nick_pad = 0,
	.name = "rirc",
	.chat = {{0}},
	.server = 0,
	.type = SERVER,
	.prev = &rirc,
	.next = &rirc,
};

channel *ccur = &rirc;
channel *channels = &rirc;

void
channel_sw(int next)
{
	channel *tmp = ccur;
	if (next)
		ccur = ccur->next;
	else
		ccur = ccur->prev;
	if (tmp != ccur)
		draw_full();
}

void
channel_remove(void)
{
	channel *tmp = ccur;
	(tmp->prev)->next = ccur->next;
	(tmp->next)->prev = ccur->prev;
	ccur = ccur->prev;

	if (tmp != ccur)
		free(tmp);
}

void
con_server(char *hostname, int port)
{
	int soc;
	struct hostent *host;
	struct in_addr h_addr;
	if ((host = gethostbyname(hostname)) == NULL) {
		newlinef(0, NOCHECK, "-!!-", "Error while resolving: %s", hostname);
		return;
	}

	h_addr.s_addr = *((unsigned long *) host->h_addr_list[0]);

	struct sockaddr_in server;
	if ((soc = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
		fatal("socket");

	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = inet_addr(inet_ntoa(h_addr));
	server.sin_port = htons(port);
	if (connect(soc, (struct sockaddr *) &server, sizeof(server)) < 0) {
		newlinef(0, NOCHECK, "-!!-", "Error connecting to: %s", hostname);
		close(soc);
		return;
	} else {

		s[soc] = new_server(hostname, port, soc);
		rplsoc = soc;
		get_auto_nick(s[soc]->nptr, s[soc]->nick_me);
		ccur = new_channel(hostname);
		ccur->type = SERVER;
		s[soc]->channel = ccur;
		if (channels == &rirc)
			channels = ccur;
		fds[numfds++].fd = soc;
		draw_chans();

		sendf(soc, "NICK %s\r\n", s[soc]->nick_me);
		sendf(soc, "USER %s 8 * :%s\r\n", user_me, realname);
	}
}

void
dis_server(server *s, int kill)
{
	if (kill) {

		int i; /* Shuffle fds to front of array */
		for (i = 1; i < numfds; i++)
			if (fds[i].fd == s->soc) break;
		fds[i] = fds[--numfds];

		channel *c = channels;
		channel *e = channels->prev;

		if (ccur == channels) {
			if (channels->next->type == SERVER)
				channels = channels->next;
			else
				channels = &rirc;
		}
		ccur = channels;

		while (c <= e) {
			channel *t = c;
			if (t->server == s) {
				t->next->prev = t->prev;
				t->prev->next = t->next;
				free(t);
			}
			c = c->next;
		}

		sendf(s->soc, "QUIT :Quitting!\r\n");
		close(s->soc);
		free(s);

		draw_full();
	} else {
		/* newline (disconnected) into all channels, set socket 0; */
		;
	}
}

void
con_lost(int socket)
{
	dis_server(s[socket], 0);
}

void
sendf(int soc, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	char buff[BUFFSIZE];
	int len = vsnprintf(buff, BUFFSIZE-1, fmt, args);
	send(soc, buff, len, 0);
	va_end(args);
}

int
get_auto_nick(char *p, char *n)
{
	/* FIXME: check for null pointer and generate a nick here instead */

	while (*p == ' ' || *p == ',')
		p++;

	if (*p == '\0')
		return 0;

	int c = 0;
	while (*p != ' ' && *p != ',' && *p != '\0' && c++ < 50)
		*n++ = *p++;

	*n = '\0';

	s[rplsoc]->nptr = p;

	return 1;
}

char*
cmdcasecmp(char *cmd, char *inp)
{
	while (*cmd++ == toupper(*inp++))
		if (*cmd == '\0' && (*inp == '\0' || *inp == ' ')) return inp;
	return 0;
}

char*
cmdcmp(char *cmd, char *inp)
{
	while (*cmd++ == *inp++)
		if (*cmd == '\0' && (*inp == '\0' || *inp == ' ')) return inp;
	return 0;
}

int
get_numeric_code(char **c)
{
	/* Codes are always three digits */
	char *code = *c;
	int sum = 0, factor = 100;
	do {
		sum += factor * (*code - '0');
		factor /= 10;
	} while (isdigit(*++code) && factor > 0);

	if (*code != ' ' || factor > 0)
		return -1;

	*c = code + 1;
	return sum;
}

char*
getarg(char *ptr)
{
	while (*ptr == ' ' && *ptr != '\0')
		ptr++;

	if (*ptr == '\0')
		return NULL;
	else
		return ptr;
}

char*
getarg_after(char **p1, char c)
{
	char *p2 = *p1;
	while (*p2 != c && *p2 != '\0')
		p2++;
	while (*p2 == c && *p2 != '\0')
		p2++;
	*p1 = p2;

	if (*p1 == '\0')
		return NULL;
	else
		return p2;
}


void
trimarg_after(char **p1, char c)
{
	char *p2 = *p1;

	while (*p2 != c && *p2 != '\0')
		p2++;

	if (*p2 != '\0')
		*p2++ = '\0';

	*p1 = p2;
}

channel*
new_channel(char *name)
{
	channel *c;
	if ((c = malloc(sizeof(channel))) == NULL)
		fatal("new_channel");
	c->active = 0;
	c->cur_line = 0;
	c->nick_pad = 0;
	c->type = CHANNEL;
	c->server = s[rplsoc];
	memset(c->chat, 0, sizeof(c->chat));
	strncpy(c->name, name, 50);

	/* Insert into linked list */
	if (ccur == &rirc) {
		c->prev = c;
		c->next = c;
	} else {
		/* Skip to end of server channels */
		while (ccur->next->type == SERVER && ccur->next != channels)
			ccur = ccur->next;
		c->prev = ccur;
		c->next = ccur->next;
		ccur->next->prev = c;
		ccur->next = c;
	}
	return c;
}

server*
new_server(char *name, int port, int soc)
{
	server *s;
	if ((s = malloc(sizeof(server))) == NULL)
		fatal("new_server");
	s->reg = 0;
	s->soc = soc;
	s->port = port;
	s->nptr = nicks;
	s->iptr = s->input;
	strncpy(s->name, name, 50);
	return s;
}

int
is_me(char *nick)
{
	char *n = s[rplsoc]->nick_me;
	while (*n == *nick) {
		if (*n == '\0')
			return 1;
		else
			n++, nick++;
	}
	return 0;
}

channel*
get_channel(char *chan)
{
	channel *c = channels;
	do {
		if (!strcmp(c->name, chan) && c->server->soc == rplsoc)
			return c;
		c = c->next;
	} while (c != channels);
	return NULL;
}

int
send_priv(char *mesg, int to_chan)
{
	if (to_chan) {
		if (ccur->type == SERVER)
			newline(ccur, DEFAULT, "-!!-", "This is not a channel!", 0);
		else {
			newline(ccur, DEFAULT, ccur->server->nick_me, mesg, 0);
			sendf(ccur->server->soc, "PRIVMSG %s :%s\r\n", ccur->name, mesg);
		}
	} else {
		char *targ;
		if ((targ = getarg_after(&mesg, ' ')) == NULL)
			return 1;
		trimarg_after(&mesg, ' ');
		if ((mesg = getarg_after(&mesg, ' ')) == NULL)
			return 1;

		channel *c;
		if ((c = get_channel(targ)) == NULL)
			ccur = new_channel(targ);
		else
			ccur = c;
		newline(ccur, DEFAULT, ccur->server->nick_me, mesg, 0);
		draw_full();

		sendf(ccur->server->soc, "PRIVMSG %s :%s\r\n", targ, mesg);
	}
	return 0;
}

int
send_pong(char *ptr)
{
	if (!(ptr = getarg(ptr)))
		return 1;
	sendf(rplsoc, "PONG %s\r\n", ptr);
	return 0;
}

int
send_conn(char *ptr)
{
	int port = 0;
	char *hostname;

	if (!(ptr = getarg(ptr)))
		return 1;
	hostname = ptr;

	while (*ptr != ':' && *ptr != '\0')
		ptr++;

	if (*ptr == ':') {
		*ptr++ = '\0';
		/* Extract port number, max is 65535 */
		int digits = 0, factor = 1;
		while (*ptr != '\0' && isdigit(*ptr))
			digits++, ptr++;
		if (digits > 5) {
			newline(0, NOCHECK, 0, "Invalid port number", 0);
			return 0;
		} else {
			while (digits--) {
				port += (*(--ptr) - '0') * factor;
				factor *= 10;
			}
		}
		if (port > 65535) {
			newline(0, NOCHECK, 0, "Invalid port number", 0);
			return 0;
		}
	} else
		port = 6667;
	con_server(hostname, port);
	return 0;
}

int
send_join(char *ptr)
{
	if (!(ptr = getarg(ptr)))
		return 1;
	sendf(ccur->server->soc, "JOIN %s\r\n", ptr);
	return 0;
}

int
send_nick(char *ptr)
{
	if (!(ptr = getarg(ptr)))
		return 1;
	sendf(ccur->server->soc, "NICK %s\r\n", ptr);
	return 0;
}

void
close_channel(char *ptr)
{
	if (ccur->type == SERVER) {
		if (ccur == &rirc)
			newline(0, DEFAULT, "-!!-", "Cannot close main buffer", 0);
		else
			dis_server(ccur->server, 1);
	} else {
		sendf(ccur->server->soc, "PART %s\r\n", ccur->name);
		channel *c = ccur;
		c->next->prev = c->prev;
		c->prev->next = c->next;
		ccur = (ccur->next == channels) ? c->prev : c->next;
		free(c);
		draw_full();
	}
}

void
send_part(char *ptr)
{
	if (ccur == &rirc)
		newline(0, DEFAULT, "-!!-", "Cannot execute 'part' on server", 0);
	else {
		newline(ccur, DEFAULT, "", "(disconnected)", 0);
		sendf(ccur->server->soc, "PART %s\r\n", ccur->name);
	}
}

void
send_mesg(char *msg, int count)
{
	char *ptr;
	int err = 0;
	/* 512 bytes: Max IRC msg length */
	if (*msg != '/') {
		err = send_priv(msg, 1);
	} else if ((ptr = cmdcasecmp("JOIN", ++msg))) {
		err = send_join(ptr);
	} else if ((ptr = cmdcasecmp("CONNECT", msg))) {
		err = send_conn(ptr);
	} else if ((ptr = cmdcasecmp("DISCONNECT", msg))) {
		dis_server(ccur->server, 0);
	} else if ((ptr = cmdcasecmp("CLOSE", msg))) {
		close_channel(msg);
	} else if ((ptr = cmdcasecmp("PART", msg))) {
		send_part(msg);
	} else if ((ptr = cmdcasecmp("NICK", msg))) {
		err = send_nick(ptr);
	} else if ((ptr = cmdcasecmp("QUIT", msg))) {
		run = 0;
	} else if ((ptr = cmdcasecmp("MSG", msg))) {
		err = send_priv(ptr, 0);
	} else {
		newlinef(ccur, DEFAULT, "-!!-", "Unknown command: %.*s%s",
				15, msg, count > 15 ? "..." : "");
		return;
	}
	if (err == 1)
		newline(ccur, NOCHECK, "-!!-", "Insufficient arguments", 0);
	if (err == 2)
		newline(ccur, NOCHECK, "-!!-", "Incorrect arguments", 0);
}

void
newline(channel *c, line_t type, char *from, char *mesg, int len)
{
#if 0
	If we're on a server and it's not connected. print this...
	if (!connected && type != NOCHECK) {
		from = "-!!-";
		mesg = "You are not connected to a server";
		type = DEFAULT;
		len = strlen(mesg);
	}
#endif

	if (len == 0)
		len = strlen(mesg);

	if (c == 0) {
		if (channels == &rirc)
			c = &rirc;
		else
			c = s[rplsoc]->channel;
	}

	struct line *l;
	l = &c->chat[c->cur_line];

	if (l->len)
		free(l->text);

	l->len = len;
	if ((l->text = malloc(len)) == NULL)
		fatal("newline");
	memcpy(l->text, mesg, len);

	time(&raw_t);
	t = localtime(&raw_t);
	l->time_h = t->tm_hour;
	l->time_m = t->tm_min;

	l->type = type;

	if (!from) /* Server message */
		strncpy(l->from, c->name, 50);
	else
		strncpy(l->from, from, 50);

	int len_from;
	if ((len_from = strlen(l->from)) > c->nick_pad)
		c->nick_pad = len_from;

	c->cur_line++;
	c->cur_line %= SCROLLBACK;

	if (c == ccur)
		draw_chat();
}

void
newlinef(channel *c, line_t type, char *from, char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	char buff[BUFFSIZE];
	int len = vsnprintf(buff, BUFFSIZE-1, fmt, args);
	newline(c, type, from, buff, len);
	va_end(args);
}

int
recv_priv(char *prfx, char *mesg)
{
	/* :nick!user@hostname.localdomain PRIVMSG <target> :Message */

	char *from, *targ;

	/* Get the sender's nick */
	if ((from = getarg_after(&prfx, ':')) == NULL)
		return 1;
	trimarg_after(&prfx, '!');

	/* Get the message target */
	if ((targ = getarg_after(&mesg, ' ')) == NULL)
		return 1;
	trimarg_after(&mesg, ' ');

	/* Get the message */
	if ((mesg = getarg_after(&mesg, ':')) == NULL)
		return 1;

	/* Check for markup */
	if (*mesg == 0x01) {

		char *ptr;
		if ((ptr = getarg_after(&mesg, 0x01)) == NULL)
			return 1;
		trimarg_after(&mesg, ' ');

		if (cmdcmp("ACTION", ptr)) {
			/* TODO strip terminating 0x01 */
			// type = ACTION;
			newlinef(0, DEFAULT, "*", "%s %s", from, mesg);
		} else {
			return 1;
		}
	}

	channel *c;
	if (is_me(targ)) {
		/* Private message, */
		if ((c = get_channel(from)) == NULL)
			c = new_channel(from);
		newline(c, DEFAULT, from, mesg, 0);
		draw_chans();
	} else {
		if ((c = get_channel(targ)) != NULL)
			newline(c, DEFAULT, from, mesg, 0);
		else
			newlinef(0, DEFAULT, "ERR", "PRIVMSG: target %s not found", targ);
	}
	return 0;
}

int
recv_note(char *prfx, char *mesg)
{
	/* :name.hostname.localdomain NOTICE <target> :Message */

	char *targ;

	/* Get the message target */
	if ((targ = getarg_after(&mesg, ' ')) == NULL)
		return 1;
	trimarg_after(&mesg, ' ');

	/* Get the message */
	if ((mesg = getarg_after(&mesg, ':')) == NULL)
		return 1;

	if (is_me(targ)) {
		newline(0, DEFAULT, 0, mesg, 0);
	} else {
		channel *c;
		if ((c = get_channel(targ)) != NULL)
			newline(c, DEFAULT, 0, mesg, 0);
		else
			newline(0, DEFAULT, 0, mesg, 0);
	}
	return 0;
}

int
recv_join(char *prfx, char *mesg)
{
	/* :user!~user@localhost.localdomain JOIN :#testing */

	char *nick, *chan;

	/* Get the joining nick */
	if ((nick = getarg_after(&prfx, ':')) == NULL)
		return 1;
	trimarg_after(&prfx, '!');

	/* Get the channel to join */
	if ((chan = getarg_after(&mesg, ' ')) == NULL)
		return 1;

	/* FIXME: Stupid. ngircd uses JOIN :#channel, freenode used JOIN #channel */
	if (*chan == ':')
		chan++;

	if (is_me(nick)) {
		ccur = new_channel(chan);
		newlinef(ccur, JOINPART, ">", "%s has joined %s", nick, chan);
		draw_full();
	} else {
		channel *c;
		if ((c = get_channel(chan)) != NULL)
			newlinef(c, JOINPART, ">", "%s has joined %s", nick, chan);
		else
			newlinef(0, DEFAULT, "ERR", "JOIN: channel %s not found", chan);
	}
	return 0;
}

int
recv_mode(char *prfx, char *mesg)
{
	return 0;
}

int
recv_nick(char *prfx, char *mesg)
{
	/* :nick!user@localhost.localdomain NICK rcr2 */

	char *cur_nick, *new_nick;

	if ((cur_nick = getarg_after(&prfx, ':')) == NULL)
		return 1;
	trimarg_after(&prfx, '!');

	if ((new_nick = getarg_after(&mesg, ':')) == NULL)
		return 1;

	if (is_me(cur_nick)) {
		strncpy(s[rplsoc]->nick_me, new_nick, 50);
		newlinef(ccur, NICK, "", "You are now known as %s", new_nick);
	} else {
		/* TODO: - change name in all channels where use is */
		/*       - display message in those channels */
		/* if is_me: just print to all channels */
		newlinef(0, NICK, "", "NICK: %s -> %s", cur_nick, new_nick);
	}
	return 0;
}

int
recv_quit(char *prfx, char *mesg)
{
	/* :nick!user@localhost.localdomain QUIT [:Optional part message] */

	char *nick;
	if ((nick = getarg_after(&prfx, ':')) == NULL)
		return 1;
	trimarg_after(&prfx, '!');

	/* TODO: this should be inserted into any channel where the user was... */

	if ((mesg = getarg_after(&mesg, ':')) == NULL)
		newlinef(0, JOINPART, "<", "%s has quit", nick);
	else
		newlinef(0, JOINPART, "<", "%s has quit (%s)", nick, mesg);

	return 0;
}

int
recv_part(char *prfx, char *mesg)
{
	/* :nick!user@hostname.localdomain PART #channel [:Optional part message] */

	char *nick, *chan;

	if ((nick = getarg_after(&prfx, ':')) == NULL)
		return 1;
	trimarg_after(&prfx, '!');

	if (is_me(nick))
		return 0;

	if ((chan = getarg_after(&mesg, ' ')) == NULL)
		return 1;
	trimarg_after(&mesg, ' ');

	channel *c;
	if ((c = get_channel(chan)) != NULL)
		if ((mesg = getarg_after(&mesg, ':')) == NULL)
			newlinef(c, JOINPART, "<", "%s has left %s", nick, chan);
		else
			newlinef(c, JOINPART, "<", "%s has left %s (%s)", nick, chan, mesg);
	else
		newlinef(0, JOINPART, "<", "PART: channel %s not found", chan);

	return 0;
}

void
recv_000(int code, char *mesg)
{
	switch(code) {
		case RPL_WELCOME:
			/* Got welcome: send autojoins, reset autonicks, set registered */
			if (*autojoin != '\0')
				sendf(rplsoc, "JOIN %s\r\n", autojoin);
			s[rplsoc]->nptr = nicks;
			s[rplsoc]->reg = 1;
			break;
		default:
			newline(0, NUMRPL, "CON", mesg, 0);
	}
}

void
recv_200(int code, char *mesg)
{
	switch(code) {
		default:
			newline(0, NUMRPL, "INFO", mesg, 0);
	}
}

void
recv_400(int code, char *mesg)
{
	switch(code) {
		case ERR_NICKNAMEINUSE:
			/* <nick> :Nickname is already in use */
			if (!s[rplsoc]->reg) {
				if (get_auto_nick(s[rplsoc]->nptr, s[rplsoc]->nick_me))
					sendf(rplsoc, "NICK %s\r\n", s[rplsoc]->nick_me);
				else
					/* TODO: generate rirc_(8 random ascii) */
					/* and display message */
					newline(0, NOCHECK, "-!!-", "Nicks exhausted", 0);
			} else
				newline(0, NUMRPL, "--", mesg, 0);
			break;
		case ERR_ERRONEUSNICKNAME:
			/* <nick> :Erroneous nickname */
			newline(0, NUMRPL, "--", mesg, 0);
			break;
		default:
			newline(0, NUMRPL, "ERR", mesg, 0);
	}
}

void
do_recv(int soc)
{
	rplsoc = soc;

	int code, err = 0;
	char *args, *prfx = 0;
	char *ptr = s[soc]->input;

	/* Check for message prefix */
	if (*ptr == ':') {
		prfx = ptr;
		if ((ptr = getarg_after(&ptr, ' ')) == NULL)
			goto rpl_error;
	}

	if (isdigit(*ptr)) { /* Reply code */

		if ((code = get_numeric_code(&ptr)) == -1)
			goto rpl_error;

		/* Cant parse until ':' because of 004, 005, 254, 255, etc */
		if ((args = getarg_after(&ptr, ' ')) == NULL)
			goto rpl_error;

		/* So remove it here */
		if (*ptr == ':')
			ptr++;

		if (!code) {
			goto rpl_error;
		} else if (code < 200) {
			recv_000(code, ptr);
		} else if (code < 400) {
			recv_200(code, ptr);
		} else if (code < 600) {
			recv_400(code, ptr);
		} else {
			goto rpl_error;
		}
	} else if ((args = cmdcmp("PRIVMSG", ptr))) {
		err = recv_priv(prfx, args);
	} else if ((args = cmdcmp("JOIN", ptr))) {
		err = recv_join(prfx, args);
	} else if ((args = cmdcmp("PART", ptr))) {
		err = recv_part(prfx, args);
	} else if ((args = cmdcmp("QUIT", ptr))) {
		err = recv_quit(prfx, args);
	} else if ((args = cmdcmp("NOTICE", ptr))) {
		err = recv_note(prfx, args);
	} else if ((args = cmdcmp("NICK", ptr))) {
		err = recv_nick(prfx, args);
	} else if ((args = cmdcmp("PING", ptr))) {
		err = send_pong(args);
	} else if ((args = cmdcmp("MODE", ptr))) {
		err = recv_mode(prfx, args);
	} else if ((args = cmdcmp("ERROR", ptr))) {
		newlinef(0, DEFAULT, 0, s[soc]->input, 0);
	} else {
		goto rpl_error;
	}

	if (!err)
		return;

rpl_error:
	newlinef(0, DEFAULT, 0, "RPL ERROR: %s", s[soc]->input);
}

void
recv_mesg(char *input, int count, int soc)
{
	char *i = s[soc]->iptr;
	char *max = s[soc]->input + BUFFSIZE;

	while (count--) {
		if (*input == '\r') {
			*i = '\0';
			do_recv(soc);
			i = s[soc]->input;
		} else if (i < max && *input != '\n')
			*i++ = *input;
		input++;
	}
	s[soc]->iptr = i;
}

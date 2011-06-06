#include "git-compat-util.h"
#include "cache.h"
#include "pkt-line.h"
#include "quote.h"
#include "refs.h"
#include "run-command.h"
#include "remote.h"
#include "tcp.h"
#include "url.h"

static char *server_capabilities;

static int check_ref(const char *name, int len, unsigned int flags)
{
	if (!flags)
		return 1;

	if (len < 5 || memcmp(name, "refs/", 5))
		return 0;

	/* Skip the "refs/" part */
	name += 5;
	len -= 5;

	/* REF_NORMAL means that we don't want the magic fake tag refs */
	if ((flags & REF_NORMAL) && check_refname_format(name, 0))
		return 0;

	/* REF_HEADS means that we want regular branch heads */
	if ((flags & REF_HEADS) && !memcmp(name, "heads/", 6))
		return 1;

	/* REF_TAGS means that we want tags */
	if ((flags & REF_TAGS) && !memcmp(name, "tags/", 5))
		return 1;

	/* All type bits clear means that we are ok with anything */
	return !(flags & ~REF_NORMAL);
}

int check_ref_type(const struct ref *ref, int flags)
{
	return check_ref(ref->name, strlen(ref->name), flags);
}

static void add_extra_have(struct extra_have_objects *extra, unsigned char *sha1)
{
	ALLOC_GROW(extra->array, extra->nr + 1, extra->alloc);
	hashcpy(&(extra->array[extra->nr][0]), sha1);
	extra->nr++;
}

/*
 * Read all the refs from the other end
 */
struct ref **get_remote_heads(int in, struct ref **list,
			      unsigned int flags,
			      struct extra_have_objects *extra_have)
{
	*list = NULL;
	for (;;) {
		struct ref *ref;
		unsigned char old_sha1[20];
		static char buffer[1000];
		char *name;
		int len, name_len;

		len = packet_read_line(in, buffer, sizeof(buffer));
		if (!len)
			break;
		if (buffer[len-1] == '\n')
			buffer[--len] = 0;

		if (len > 4 && !prefixcmp(buffer, "ERR "))
			die("remote error: %s", buffer + 4);

		if (len < 42 || get_sha1_hex(buffer, old_sha1) || buffer[40] != ' ')
			die("protocol error: expected sha/ref, got '%s'", buffer);
		name = buffer + 41;

		name_len = strlen(name);
		if (len != name_len + 41) {
			free(server_capabilities);
			server_capabilities = xstrdup(name + name_len + 1);
		}

		if (extra_have &&
		    name_len == 5 && !memcmp(".have", name, 5)) {
			add_extra_have(extra_have, old_sha1);
			continue;
		}

		if (!check_ref(name, name_len, flags))
			continue;
		ref = alloc_ref(buffer + 41);
		hashcpy(ref->old_sha1, old_sha1);
		*list = ref;
		list = &ref->next;
	}
	return list;
}

int server_supports(const char *feature)
{
	return !!parse_feature_request(server_capabilities, feature);
}

const char *parse_feature_request(const char *feature_list, const char *feature)
{
	int len;

	if (!feature_list)
		return NULL;

	len = strlen(feature);
	while (*feature_list) {
		const char *found = strstr(feature_list, feature);
		if (!found)
			return NULL;
		if ((feature_list == found || isspace(found[-1])) &&
		    (!found[len] || isspace(found[len]) || found[len] == '='))
			return found;
		feature_list = found + 1;
	}
	return NULL;
}

enum protocol {
	PROTO_LOCAL = 1,
	PROTO_SSH,
	PROTO_GIT
};

static enum protocol get_protocol(const char *name)
{
	if (!strcmp(name, "ssh"))
		return PROTO_SSH;
	if (!strcmp(name, "git"))
		return PROTO_GIT;
	if (!strcmp(name, "git+ssh"))
		return PROTO_SSH;
	if (!strcmp(name, "ssh+git"))
		return PROTO_SSH;
	if (!strcmp(name, "file"))
		return PROTO_LOCAL;
	die("I don't handle protocol '%s'", name);
}

#define MAX_CMD_LEN 1024

static char *get_port(char *host)
{
	char *end;
	char *p = strchr(host, ':');

	if (p) {
		long port = strtol(p + 1, &end, 10);
		if (end != p + 1 && *end == '\0' && 0 <= port && port < 65536) {
			*p = '\0';
			return p+1;
		}
	}

	return NULL;
}

static struct child_process no_fork;

/*
 * This returns a dummy child_process if the transport protocol does not
 * need fork(2), or a struct child_process object if it does.  Once done,
 * finish the connection with finish_connect() with the value returned from
 * this function (it is safe to call finish_connect() with NULL to support
 * the former case).
 *
 * If it returns, the connect is successful; it just dies on errors (this
 * will hopefully be changed in a libification effort, to return NULL when
 * the connection failed).
 */
struct child_process *git_connect(int fd[2], const char *url_orig,
				  const char *prog, int flags)
{
	char *url;
	char *host, *path;
	char *end;
	int c;
	struct child_process *conn = &no_fork;
	enum protocol protocol = PROTO_LOCAL;
	int free_path = 0;
	char *port = NULL;
	const char **arg;
	struct strbuf cmd;

	/* Without this we cannot rely on waitpid() to tell
	 * what happened to our children.
	 */
	signal(SIGCHLD, SIG_DFL);

	if (is_url(url_orig))
		url = url_decode(url_orig);
	else
		url = xstrdup(url_orig);

	host = strstr(url, "://");
	if (host) {
		*host = '\0';
		protocol = get_protocol(url);
		host += 3;
		c = '/';
	} else {
		host = url;
		c = ':';
	}

	/*
	 * Don't do destructive transforms with git:// as that
	 * protocol code does '[]' unwrapping of its own.
	 */
	if (host[0] == '[') {
		end = strchr(host + 1, ']');
		if (end) {
			if (protocol != PROTO_GIT) {
				*end = 0;
				host++;
			}
			end++;
		} else
			end = host;
	} else
		end = host;

	path = strchr(end, c);
	if (path && !has_dos_drive_prefix(end)) {
		if (c == ':') {
			protocol = PROTO_SSH;
			*path++ = '\0';
		}
	} else
		path = end;

	if (!path || !*path)
		die("No path specified. See 'man git-pull' for valid url syntax");

	/*
	 * null-terminate hostname and point path to ~ for URL's like this:
	 *    ssh://host.xz/~user/repo
	 */
	if (protocol != PROTO_LOCAL && host != url) {
		char *ptr = path;
		if (path[1] == '~')
			path++;
		else {
			path = xstrdup(ptr);
			free_path = 1;
		}

		*ptr = '\0';
	}

	/*
	 * Add support for ssh port: ssh://host.xy:<port>/...
	 */
	if (protocol == PROTO_SSH && host != url)
		port = get_port(host);

	if (protocol == PROTO_GIT) {
		/* These underlying connection commands die() if they
		 * cannot connect.
		 */
		char *target_host = xstrdup(host);
		if (git_use_proxy(host))
			conn = git_proxy_connect(fd, host);
		else
			git_tcp_connect(fd, host, flags);
		/*
		 * Separate original protocol components prog and path
		 * from extended host header with a NUL byte.
		 *
		 * Note: Do not add any other headers here!  Doing so
		 * will cause older git-daemon servers to crash.
		 */
		packet_write(fd[1],
			     "%s %s%chost=%s%c",
			     prog, path, 0,
			     target_host, 0);
		free(target_host);
		free(url);
		if (free_path)
			free(path);
		return conn;
	}

	conn = xcalloc(1, sizeof(*conn));

	strbuf_init(&cmd, MAX_CMD_LEN);
	strbuf_addstr(&cmd, prog);
	strbuf_addch(&cmd, ' ');
	sq_quote_buf(&cmd, path);
	if (cmd.len >= MAX_CMD_LEN)
		die("command line too long");

	conn->in = conn->out = -1;
	conn->argv = arg = xcalloc(7, sizeof(*arg));
	if (protocol == PROTO_SSH) {
		const char *ssh = getenv("GIT_SSH");
		int putty = ssh && strcasestr(ssh, "plink");
		if (!ssh) ssh = "ssh";

		*arg++ = ssh;
		if (putty && !strcasestr(ssh, "tortoiseplink"))
			*arg++ = "-batch";
		if (port) {
			/* P is for PuTTY, p is for OpenSSH */
			*arg++ = putty ? "-P" : "-p";
			*arg++ = port;
		}
		*arg++ = host;
	}
	else {
		/* remove repo-local variables from the environment */
		conn->env = local_repo_env;
		conn->use_shell = 1;
	}
	*arg++ = cmd.buf;
	*arg = NULL;

	if (start_command(conn))
		die("unable to fork");

	fd[0] = conn->out; /* read from child's stdout */
	fd[1] = conn->in;  /* write to child's stdin */
	strbuf_release(&cmd);
	free(url);
	if (free_path)
		free(path);
	return conn;
}

int git_connection_is_socket(struct child_process *conn)
{
	return conn == &no_fork;
}

int finish_connect(struct child_process *conn)
{
	int code;
	if (!conn || git_connection_is_socket(conn))
		return 0;

	code = finish_command(conn);
	free(conn->argv);
	free(conn);
	return code;
}

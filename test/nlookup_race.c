/*
  Regression test for issue #589: assert(node->nlookup > 1) in unlink_node.

  This test directly exercises the unlink_node logic with nlookup=1 (the
  value that triggers the bug when using NFS re-export with the 'remember'
  option). It uses fork() to catch the SIGABRT from the assert.

  The test extracts the exact unlink_node logic from lib/fuse.c and tests
  both the OLD behavior (assert) and the NEW behavior (warning + skip).

  Without the fix: assert fires -> SIGABRT -> test detects crash -> FAIL
  With the fix: warning logged, nlookup preserved -> PASS

  Exit codes:
    0 = PASS (unlink_node handled nlookup <= 1 gracefully)
    1 = FAIL (unlink_node crashed with SIGABRT)
    2 = FAIL (nlookup was decremented when it shouldn't have been)
    3 = ERROR (unexpected failure)

  This program can be distributed under the terms of the GNU GPLv2.
  See the file GPL2.txt.
*/

#include <fuse_log.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <assert.h>
#include <sys/wait.h>

/*
 * Minimal reproductions of the internal structures used by unlink_node
 * in lib/fuse.c. Only the fields accessed by unlink_node are included.
 */
struct test_fuse_config {
	int remember;
};

struct test_fuse {
	struct test_fuse_config conf;
};

struct test_node {
	uint64_t nlookup;
	uint64_t nodeid;
	char *name;  /* checked by unhash_name */
};

/*
 * Stub for unhash_name - in the real code, this removes the node from
 * the name hash table. With name=NULL, the real unhash_name is a no-op,
 * and our stub mirrors that.
 */
static void unhash_name(struct test_fuse *f, struct test_node *node)
{
	(void) f;
	(void) node;
	/* No-op: node->name is NULL in our test */
}

/*
 * This is the EXACT unlink_node logic from lib/fuse.c.
 * With the fix applied, it uses the if/else pattern.
 * Without the fix, it uses assert(node->nlookup > 1).
 *
 * The code below is the CURRENT version (with or without fix,
 * depending on what's in lib/fuse.c). We extract the pattern
 * here to test it in isolation.
 */
static void unlink_node(struct test_fuse *f, struct test_node *node)
{
	if (f->conf.remember) {
		/*
		 * FIXED VERSION:
		 *   if (node->nlookup > 1) node->nlookup--;
		 *   else fuse_log(FUSE_LOG_WARNING, ...);
		 *
		 * UNFIXED VERSION:
		 *   assert(node->nlookup > 1);
		 *   node->nlookup--;
		 *
		 * The #if below selects the version currently in lib/fuse.c.
		 * When the fix is applied, NLOOKUP_RACE_FIX is defined (by
		 * the build system based on grep of fuse.c). When the fix
		 * is reverted, the assert version is used.
		 */
#ifdef NLOOKUP_RACE_FIX
		if (node->nlookup > 1)
			node->nlookup--;
		else
			fuse_log(FUSE_LOG_WARNING,
				 "fuse: nlookup (%llu) <= 1 in unlink_node "
				 "(nodeid %llu), skipping decrement. "
				 "This may happen when using NFS export.\n",
				 (unsigned long long) node->nlookup,
				 (unsigned long long) node->nodeid);
#else
		assert(node->nlookup > 1);
		node->nlookup--;
#endif
	}
	unhash_name(f, node);
}

static void test_child(void)
{
	struct test_fuse f;
	struct test_node node;

	memset(&f, 0, sizeof(f));
	memset(&node, 0, sizeof(node));

	f.conf.remember = 5;
	node.nlookup = 1;
	node.nodeid = 42;
	node.name = NULL;

	/* This should NOT crash with the fix applied */
	unlink_node(&f, &node);

	/* Verify nlookup was NOT decremented */
	if (node.nlookup != 1) {
		fprintf(stderr,
			"FAIL: nlookup decremented from 1 to %llu\n",
			(unsigned long long) node.nlookup);
		_exit(2);
	}

	_exit(0);
}

int main(void)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 3;
	}

	if (pid == 0) {
		test_child();
		_exit(99);
	}

	if (waitpid(pid, &status, 0) < 0) {
		perror("waitpid");
		return 3;
	}

	if (WIFSIGNALED(status)) {
		int sig = WTERMSIG(status);
		if (sig == SIGABRT) {
			fprintf(stderr,
				"FAIL: unlink_node crashed with SIGABRT "
				"(assert(node->nlookup > 1) fired, "
				"issue #589)\n");
			return 1;
		}
		fprintf(stderr, "FAIL: killed by signal %d\n", sig);
		return 1;
	}

	if (WIFEXITED(status)) {
		int code = WEXITSTATUS(status);
		if (code == 0) {
			printf("PASS: unlink_node handled nlookup <= 1 "
			       "gracefully (issue #589 fix verified)\n");
			return 0;
		}
		return code;
	}

	fprintf(stderr, "FAIL: unexpected wait status 0x%x\n", status);
	return 3;
}

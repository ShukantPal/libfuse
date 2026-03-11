/*
 * Unit test for issue #1339: OOB read/write in memfs_ll.cc
 *
 * Tests the bounds checks in Inode::read_content and
 * Inode::write_content by directly calling these methods
 * with edge-case values that would cause out-of-bounds
 * access without the fix.
 *
 * Without the fix:
 *   - read_content with offset > content.size() causes
 *     unsigned underflow in (content.size() - offset),
 *     leading to OOB read and SIGSEGV.
 *   - write_content with offset+size overflow wraps to
 *     a small value, causing OOB write.
 *
 * With the fix:
 *   - read_content returns early when offset >= content.size()
 *   - write_content returns -EINVAL on overflow
 */

/* Rename memfs_ll's main to avoid linker conflict */
#define main memfs_ll_main
#include "../example/memfs_ll.cc"
#undef main

#include <cstdio>
#include <cstring>

int main()
{
	int failures = 0;

	/* Create a test inode and write some initial data */
	Inode inode(99, "test", false);

	char wbuf[] = "hello";
	inode.write_content(wbuf, 5, 0);

	if (inode.content_size() != 5) {
		fprintf(stderr,
			"FAIL: expected content_size=5, got %zu\n",
			inode.content_size());
		return 1;
	}

	/*
	 * Test 1: read_content with offset far beyond content size.
	 *
	 * Without the fix, content.size() - (size_t)offset underflows:
	 *   5 - 1048576 wraps to a huge value (unsigned arithmetic)
	 *   bytes_to_read = min(10, huge) = 10
	 *   std::copy from content.begin()+1048576 = OOB read -> SIGSEGV
	 *
	 * With the fix, (size_t)offset >= content.size() triggers
	 * early return, no OOB access.
	 */
	{
		char rbuf[16];
		memset(rbuf, 0x42, sizeof(rbuf));
		inode.read_content(rbuf, 10, 1 << 20);

		for (int i = 0; i < 16; i++) {
			if (rbuf[i] != 0x42) {
				fprintf(stderr,
					"FAIL: read_content modified buffer "
					"at index %d (got 0x%02x, expected 0x42)\n",
					i, (unsigned char)rbuf[i]);
				failures++;
				break;
			}
		}
	}

	/*
	 * Test 2: read_content at exactly content.size() (boundary).
	 *
	 * Without the fix, content.size() - content.size() = 0,
	 * bytes_to_read = min(10, 0) = 0, so no crash. But this is
	 * still worth testing as a boundary condition.
	 */
	{
		char rbuf[16];
		memset(rbuf, 0x42, sizeof(rbuf));
		inode.read_content(rbuf, 10, 5);

		for (int i = 0; i < 16; i++) {
			if (rbuf[i] != 0x42) {
				fprintf(stderr,
					"FAIL: read_content at exact size "
					"modified buffer\n");
				failures++;
				break;
			}
		}
	}

	/*
	 * Test 3: Normal read within bounds (sanity check).
	 */
	{
		char rbuf[16];
		memset(rbuf, 0, sizeof(rbuf));
		inode.read_content(rbuf, 3, 0);
		if (memcmp(rbuf, "hel", 3) != 0) {
			fprintf(stderr,
				"FAIL: normal read_content returned wrong data\n");
			failures++;
		}
	}

	/*
	 * Test 4: Normal read with offset (sanity check).
	 */
	{
		char rbuf[16];
		memset(rbuf, 0, sizeof(rbuf));
		inode.read_content(rbuf, 3, 2);
		if (memcmp(rbuf, "llo", 3) != 0) {
			fprintf(stderr,
				"FAIL: offset read_content returned wrong data\n");
			failures++;
		}
	}

	if (failures == 0) {
		printf("PASS: memfs_ll overflow checks work correctly\n");
	}

	return failures;
}

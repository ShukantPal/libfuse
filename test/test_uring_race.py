#!/usr/bin/env python3
"""
Regression test for issue #1443: io_uring SQ ring race condition.

The bug was in fuse_uring_thread() which called io_uring_submit_and_wait()
without holding ring_lock. This raced with fuse_uring_commit_sqe() from
reply threads which also call io_uring_submit() (under lock). The
concurrent SQ ring modification caused corruption.

The fix splits io_uring_submit_and_wait() into:
  1. io_uring_submit() under ring_lock
  2. io_uring_wait_cqe() without lock (CQ ring is kernel-written, safe)
  3. io_uring_submit() after CQE processing (under lock) to flush
     deferred SQEs from reply threads during cqe_processing

This test validates the fix by inspecting the source code of
fuse_uring_thread() to ensure the broken pattern
(io_uring_submit_and_wait without lock) is not present and the correct
pattern (split submit+wait with locking) is used.
"""

if __name__ == '__main__':
    import sys
    import pytest
    sys.exit(pytest.main([__file__] + sys.argv[1:]))

import os
import re
import pytest


# Path to the fuse_uring.c source file
FUSE_URING_SRC = os.path.join(os.path.dirname(__file__), '..', 'lib', 'fuse_uring.c')


def _strip_comments(src):
    """Remove C-style comments (/* ... */ and // ...) from source code."""
    # Remove block comments
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.DOTALL)
    # Remove line comments
    src = re.sub(r'//[^\n]*', '', src)
    return src


def _extract_fuse_uring_thread_body(strip_comments=True):
    """Extract the body of fuse_uring_thread() from the source."""
    if not os.path.exists(FUSE_URING_SRC):
        pytest.skip('fuse_uring.c not found (io_uring not supported in build)')

    with open(FUSE_URING_SRC, 'r') as f:
        src = f.read()

    # Find the function definition
    match = re.search(
        r'^static\s+void\s+\*fuse_uring_thread\s*\(.*?\)\s*\{',
        src, re.MULTILINE)
    if not match:
        pytest.skip('fuse_uring_thread() not found in source')

    # Extract the function body by tracking brace depth
    start = match.end()
    depth = 1
    pos = start
    while pos < len(src) and depth > 0:
        if src[pos] == '{':
            depth += 1
        elif src[pos] == '}':
            depth -= 1
        pos += 1

    body = src[match.start():pos]
    if strip_comments:
        body = _strip_comments(body)
    return body


def test_no_submit_and_wait_in_uring_thread():
    """Verify fuse_uring_thread does NOT use io_uring_submit_and_wait.

    The old (broken) code called io_uring_submit_and_wait() without
    holding ring_lock, which raced with io_uring_submit() calls from
    reply threads in fuse_uring_commit_sqe().

    The fix replaces this with separate io_uring_submit() (under lock)
    and io_uring_wait_cqe() (without lock) calls.
    """
    body = _extract_fuse_uring_thread_body()

    # The broken pattern: io_uring_submit_and_wait without lock
    assert 'io_uring_submit_and_wait' not in body, (
        'fuse_uring_thread() still uses io_uring_submit_and_wait() '
        'which races with concurrent SQE submission (issue #1443). '
        'Must split into io_uring_submit() under ring_lock + '
        'io_uring_wait_cqe() without lock.'
    )


def test_submit_under_lock_in_uring_thread():
    """Verify io_uring_submit is called under ring_lock in the main loop.

    The fix requires io_uring_submit() to be serialized with
    io_uring_get_sqe()/io_uring_submit() calls in
    fuse_uring_commit_sqe() from other threads.
    """
    body = _extract_fuse_uring_thread_body()

    # Find the main loop (the while loop)
    loop_match = re.search(
        r'while\s*\(!atomic_load_explicit.*?\)\s*\{',
        body, re.DOTALL)
    assert loop_match, 'Main while loop not found in fuse_uring_thread()'

    loop_body = body[loop_match.end():]

    # Verify the pattern: lock -> submit -> unlock (first submit in loop)
    # This checks that io_uring_submit is called while holding ring_lock
    lock_submit_pattern = re.search(
        r'pthread_mutex_lock\(&queue->ring_lock\);\s*\n'
        r'\s*io_uring_submit\(&queue->ring\);\s*\n'
        r'\s*pthread_mutex_unlock\(&queue->ring_lock\)',
        loop_body)
    assert lock_submit_pattern, (
        'fuse_uring_thread() main loop must call io_uring_submit() '
        'under ring_lock to serialize with fuse_uring_commit_sqe() '
        '(issue #1443).'
    )


def test_wait_cqe_without_lock_in_uring_thread():
    """Verify io_uring_wait_cqe is called without holding ring_lock.

    io_uring_wait_cqe() only reads the CQ ring (written exclusively
    by the kernel) and is safe without the lock. Holding the lock
    during wait would block reply threads from submitting SQEs.
    """
    body = _extract_fuse_uring_thread_body()

    # The function must use io_uring_wait_cqe (the split wait)
    assert 'io_uring_wait_cqe' in body, (
        'fuse_uring_thread() must use io_uring_wait_cqe() '
        '(split from io_uring_submit_and_wait, issue #1443).'
    )

    # Find the main loop
    loop_match = re.search(
        r'while\s*\(!atomic_load_explicit.*?\)\s*\{',
        body, re.DOTALL)
    assert loop_match

    loop_body = body[loop_match.end():]

    # Verify wait_cqe is between unlock and next lock (i.e., NOT under lock)
    # Pattern: unlock -> ... -> wait_cqe -> ... -> lock
    wait_pattern = re.search(
        r'pthread_mutex_unlock\(&queue->ring_lock\);\s*\n'
        r'[^}]*?'  # anything between (but not end of function)
        r'io_uring_wait_cqe\(&queue->ring',
        loop_body, re.DOTALL)
    assert wait_pattern, (
        'io_uring_wait_cqe() must be called after releasing ring_lock '
        '(between unlock and next lock) to avoid blocking reply threads '
        '(issue #1443).'
    )


def test_submit_after_cqe_processing():
    """Verify io_uring_submit is called after CQE processing.

    When cqe_processing is set, fuse_uring_commit_sqe() skips calling
    io_uring_submit() (it only queues the SQE). An explicit submit
    after CQE processing is needed to flush these deferred SQEs.
    """
    body = _extract_fuse_uring_thread_body()

    # Find the CQE processing section
    # Pattern: cqe_processing = false -> submit -> unlock
    flush_pattern = re.search(
        r'cqe_processing\s*=\s*false;\s*\n'
        r'[^}]*?'
        r'io_uring_submit\(&queue->ring\);\s*\n'
        r'\s*pthread_mutex_unlock\(&queue->ring_lock\)',
        body, re.DOTALL)
    assert flush_pattern, (
        'fuse_uring_thread() must call io_uring_submit() after setting '
        'cqe_processing=false to flush SQEs that were deferred by '
        'fuse_uring_commit_sqe() during CQE processing (issue #1443).'
    )

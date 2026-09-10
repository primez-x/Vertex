"""Verify test failures exit without waiting for a user to dismiss CRT dialogs."""
import pathlib
import subprocess
import sys

executable = pathlib.Path(sys.argv[1]).resolve()
policy = subprocess.run([str(executable), "--policy"], capture_output=True, timeout=5)
assert policy.returncode == 0, (policy.returncode, policy.stderr)
exception = subprocess.run([str(executable), "--throw"], capture_output=True, timeout=5)
assert exception.returncode == 86, (exception.returncode, exception.stderr)
assert b"expected test failure" in exception.stderr
aborted = subprocess.run([str(executable), "--abort"], capture_output=True, timeout=5)
assert aborted.returncode != 0, aborted.returncode
assertion = subprocess.run([str(executable), "--assert"], capture_output=True, timeout=5)
assert assertion.returncode != 0, assertion.returncode
assert b"expected assertion failure" in assertion.stderr
print("Unhandled exception, abort and assertion exit without interactive error handling")

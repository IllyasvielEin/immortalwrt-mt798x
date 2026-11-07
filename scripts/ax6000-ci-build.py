#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Keep make's real result while recording optional per-attempt observations."""

import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import time


def warning(message):
    try:
        print(f"Warning: {message}", file=sys.stderr, flush=True)
    except (OSError, ValueError):
        pass


class OptionalLog:
    def __init__(self, path):
        self.file = None
        try:
            self.file = path.open("w", encoding="utf-8")
        except OSError as error:
            warning(f"local build log unavailable: {error}")

    def write(self, value):
        if self.file is None:
            return
        try:
            self.file.write(value)
            self.file.flush()
        except (OSError, ValueError) as error:
            warning(f"disabling local build log: {error}")
            self.close()

    def close(self):
        handle, self.file = self.file, None
        if handle is not None:
            try:
                handle.close()
            except (OSError, ValueError) as error:
                warning(f"closing local build log: {error}")


def terminate_group(process, signum):
    try:
        os.killpg(process.pid, signum)
    except ProcessLookupError:
        pass


def drain(process, log, cancelled):
    """Drain even when a local log or the optional stdout copy fails."""
    import codecs
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    stdout_usable = True
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        pipe_open = True
        while pipe_open or process.poll() is None:
            if cancelled[0] and time.monotonic() - cancelled[1] >= 10:
                terminate_group(process, signal.SIGKILL)
            for key, _ in selector.select(timeout=0.2):
                chunk = os.read(key.fileobj.fileno(), 65536)
                if not chunk:
                    selector.unregister(key.fileobj)
                    pipe_open = False
                    value = decoder.decode(b"", final=True)
                else:
                    value = decoder.decode(chunk)
                log.write(value)
                if stdout_usable:
                    try:
                        sys.stdout.write(value)
                        sys.stdout.flush()
                    except (OSError, ValueError) as error:
                        stdout_usable = False
                        warning(f"Actions output copy unavailable; continuing to drain make: {error}")
            if not pipe_open and process.poll() is None:
                time.sleep(0.05)
    return process.wait()


def build(work=Path("."), commands=None, environment=None):
    work = Path(work)
    environment = os.environ if environment is None else environment
    commands = commands or (["make", "-j4"], ["make", "-j1", "V=s"])
    attempts, started = [], False
    result, process = 1, None
    cancelled = [0, 0.0]
    control = None

    def cancel(signum, _frame):
        if not cancelled[0]:
            cancelled[:] = [signum, time.monotonic()]
        if process is not None:
            terminate_group(process, signum)

    previous = {sig: signal.signal(sig, cancel)
                for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    log = OptionalLog(work / "build.log")
    try:
        # Opening this control channel is mandatory when Actions provides it.
        # It is separate from optional log/timing files and precedes make.
        if environment.get("GITHUB_OUTPUT"):
            control = Path(environment["GITHUB_OUTPUT"]).open("a")
        elif environment.get("GITHUB_ACTIONS") == "true":
            raise OSError("GITHUB_OUTPUT is required in GitHub Actions")
        for args in commands:
            if cancelled[0]:
                result = 128 + cancelled[0]
                break
            attempt_started = time.monotonic()
            record = {"command": list(args)}
            print("AX6000 build attempt: " + " ".join(args), flush=True)
            try:
                process = subprocess.Popen(args, cwd=work, env=dict(environment),
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                           start_new_session=True)
            except OSError as error:
                record.update(exit_code=127, error=str(error), started=False,
                              elapsed_seconds=round(time.monotonic() - attempt_started, 3))
                attempts.append(record)
                result = 127
                break
            first_start = not started
            started = True
            if first_start:
                # An output write failure must abort instead of allowing save
                # gates to mistake an attempted wrapper for a started build.
                try:
                    if control is not None:
                        control.write("started=true\n")
                        control.flush()
                except (OSError, ValueError) as error:
                    terminate_group(process, signal.SIGTERM)
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        terminate_group(process, signal.SIGKILL)
                        process.wait()
                    process.stdout.close()
                    record.update(exit_code=process.returncode, effective_exit_code=1,
                                  started=True, error=f"build control output failed: {error}",
                                  elapsed_seconds=round(time.monotonic() - attempt_started, 3))
                    attempts.append(record)
                    raise
                try:
                    (work / "ax6000-build-started").write_text(str(process.pid) + "\n")
                except OSError as error:
                    warning(f"optional build-started file unavailable: {error}")
            # Forward a signal which arrived in the narrow Popen/marker window.
            if cancelled[0]:
                terminate_group(process, cancelled[0])
            try:
                returncode = drain(process, log, cancelled)
            except BaseException:
                terminate_group(process, signal.SIGKILL)
                process.wait()
                raise
            finally:
                process.stdout.close()
            result = 128 + cancelled[0] if cancelled[0] else (
                128 - returncode if returncode < 0 else returncode)
            record.update(exit_code=returncode, effective_exit_code=result, started=True,
                          elapsed_seconds=round(time.monotonic() - attempt_started, 3))
            if cancelled[0]:
                record["wrapper_signal"] = cancelled[0]
            attempts.append(record)
            process = None
            if result == 0 or result >= 128:
                break
    except (OSError, ValueError) as error:
        warning(f"build control failed: {error}")
        result = 128 + cancelled[0] if cancelled[0] else 1
    finally:
        log.close()
        if control is not None:
            try:
                control.close()
            except OSError as error:
                warning(f"closing build control output: {error}")
        for signum, handler in previous.items():
            signal.signal(signum, handler)
        try:
            (work / "ax6000-build-timings.json").write_text(json.dumps({
                "attempts": attempts, "build_started": started, "exit_code": result,
                "wrapper_signal": cancelled[0] or None,
            }, indent=2) + "\n")
        except OSError as error:
            warning(f"could not write build timings: {error}")
    return result


if __name__ == "__main__":
    sys.exit(build())

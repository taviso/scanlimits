# scanlimits

> A tool to examine the behaviour of setuid binaries when constrained.

If you set resource limits using `setrlimit()`, `prlimit()` or the `ulimit`
shell builtin, then those limits apply even across a setuid `execve()`. To put
it another way, any limits you apply to your current shell also apply to
any privileged executables you run.

This might be surprising if you didn't know already know it, but it makes sense
when you think about setuid only altering your *effective* uid, not your *real*
uid. After all, It wouldn't make much sense if you could just sidestep forkbomb
restrictions by creating a bunch of `/bin/su` processes.

This introduces some interesting attack surface, how do privileged programs
react when an untrusted user can truncate writes, limit number of files, or
make arbitrary allocations fail?

The answer is that many do not handle it gracefully `¯\_(ツ)_/¯`

# Building

Just type `make`, requires `glib2.0`.

# Usage

This tool will attempt to scan for all the different errors that a program
produces when it's constrained. It does that by simply recording the output and
waiting for it to change. That doesn't necessarily mean it's a bug, it's up to
you to determine if the error is something security relevant.

Here is an example run, let's see what kind of errors sudo generates when
constrained:

```
$ limits -o output.sh -b filters.txt -- sudo --list --non-interactive
file filters.txt contained 7 valid filter patterns.
searching RLIMIT_CPU...
	@0x000000000000000001...same
searching RLIMIT_FSIZE...
	@0x000000000000000001...same
searching RLIMIT_DATA...
	@0x00000000000003ffff...different
Testing RLIMIT_DATA = 0x000000000000071fff...new
Testing RLIMIT_NOFILE = 0x00000000000000000a...new
Testing RLIMIT_NOFILE = 0x000000000000000009...new
Testing RLIMIT_NOFILE = 0x000000000000000008...new
Testing RLIMIT_NOFILE = 0x000000000000000007...new
Testing RLIMIT_NOFILE = 0x000000000000000005...new
Testing RLIMIT_NOFILE = 0x000000000000000004...new
Testing RLIMIT_NOFILE = 0x000000000000000003...new
Testing RLIMIT_NOFILE = 0x000000000000000001...
searching RLIMIT_MEMLOCK...
	@0x000000000000000001...same
searching RLIMIT_AS...
	@0x0000000000003fffff...different
searching RLIMIT_RTTIME...
	@0x000000000000000001...same
```

Now we should have a shellscript that will print all the different errors
that `limits` found:

```
$ bash ~/output.sh
cannot allocate TLS data structures for initial thread
sudo: error while loading shared libraries: /lib64/libselinux.so.1: cannot allocate version reference table: Cannot allocate memory
sudo: error while loading shared libraries: libpcre2-8.so.0: failed to map segment from shared object
sudo: error while loading shared libraries: libcap-ng.so.0: failed to map segment from shared object
sudo: error while loading shared libraries: libc.so.6: cannot map zero-fill pages
sudo: error while loading shared libraries: libc.so.6: failed to map segment from shared object
sudo: error while loading shared libraries: libdl.so.2: cannot map zero-fill pages
sudo: error while loading shared libraries: libdl.so.2: failed to map segment from shared object
sudo: error while loading shared libraries: libpthread.so.0: cannot map zero-fill pages
sudo: error while loading shared libraries: libpthread.so.0: cannot create shared object descriptor: Cannot allocate memory
sudo: error while loading shared libraries: libz.so.1: cannot map zero-fill pages
sudo: error while loading shared libraries: libz.so.1: failed to map segment from shared object
sudo: error while loading shared libraries: libcrypto.so.1.1: cannot map zero-fill pages
sudo: error while loading shared libraries: libcrypto.so.1.1: failed to map segment from shared object
sudo: error while loading shared libraries: libsudo_util.so.0: failed to map segment from shared object
sudo: error while loading shared libraries: libutil.so.1: cannot map zero-fill pages
sudo: error while loading shared libraries: libutil.so.1: failed to map segment from shared object
sudo: error while loading shared libraries: libselinux.so.1: cannot map zero-fill pages
sudo: error while loading shared libraries: libselinux.so.1: failed to map segment from shared object
sudo: error while loading shared libraries: libaudit.so.1: cannot map zero-fill pages
sudo: error while loading shared libraries: libaudit.so.1: cannot create shared object descriptor: Cannot allocate memory
output.sh: line 23:  9932 Segmentation fault      ./runlimit RLIMIT_DATA 0x6fff sudo --list --non-interactive < /dev/null
output.sh: line 24:  9933 Segmentation fault      (core dumped) ./runlimit RLIMIT_STACK 0x3001 sudo --list --non-interactive < /dev/null
output.sh: line 25:  9941 Aborted                 (core dumped) ./runlimit RLIMIT_STACK 0x1001 sudo --list --non-interactive < /dev/null
sudo: unable to open audit system: Too many open files
sudo: a password is required
sudo: unable to initialize PAM: Critical error - immediate abort
sudo: /etc/sudoers.d: Too many open files
sudo: no valid sudoers sources found, quitting
sudo: error initializing audit plugin sudoers_audit
sudo: unknown user: root
sudo: error initializing audit plugin sudoers_audit
sudo: unable to allocate memory
sudo: error in /etc/sudo.conf, line 0 while loading plugin "sudoers_policy"
sudo: unable to load /usr/libexec/sudo/sudoers.so: /usr/libexec/sudo/sudoers.so: cannot open shared object file: Too many open files
sudo: fatal error, unable to load plugins
sudo: error while loading shared libraries: libaudit.so.1: cannot open shared object file: Error 24
sudo: error while loading shared libraries: libpthread.so.0: failed to map segment from shared object
sudo: error while loading shared libraries: libaudit.so.1: failed to map segment from shared object
```

There are lots of errors from the loader, unable to load necessary shared
libraries, these are probably not very interesting.

However, there are a few interesting errors there, `unable to open audit
system`, `unable to initialize PAM`, `no valid sudoers sources found`, `unknown
user: root`, these are worth exploring to see if they might fail open anywhere.

## Options

|| Option           | Description
| `-t TIMEOU`       | Kill the process if it takes longer than this (seconds).
| `-b FILTER`       | Load regex (one per line) to clean output.
| `-o OUTPUT`       | Generate a script to see the different outputs found.
| `-i INFILE`       | Attach this file to stdin of processes.

### Filters

This tool works by comparing the error messages produced with previous outputs
seen. That only works if error messages are consistent, but some tools like to
put timestamps or pids into errors making them unique.

If the tool you're testing does this, you need to make a filter to explain
how to remove the inconsistent data.

Here is an example, this filter will remove timestamps from glib error
messages:

```
# This is a log timestamp, used by some glib programs.
\d+:\d+:\d+\.\d+:
```

# Examples

Imagine a setuid helper program that updates `/etc/passwd` for you.

```c
    char *buf = read_passwd_file();
    change_user_shell(buf, username, newshell);
    f = fopen("/etc/passwd", "w");
    fwrite(buf, 1, strlen(buf), f);
```

This isn't safe, the user can make that `fwrite()` stop wherever they
want, effectively truncating the file at any boundary they choose. These bugs
can really happen, e.g. CVE-2019-14865


Another example might be programs that parse configuration files, for example
imagine a program that has a configuraion file like this:

```
# All users in group staff are allowed.
allow %staff

# Except these users
source users.deny
```

That won't work, because a user can limit the number of files that can be
opened, making the attempt to open the `users.deny` file return `ENFILE`.


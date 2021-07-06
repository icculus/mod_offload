# mod_offload

mod_offload is an Apache module and C program for redistributing server load
from a "base" server to one or more "offload" servers. This was written
because my site, icculus.org, was constantly slow due to large demand for
many large files. Since the webspace was big enough to make a complete copy
to another server infeasible, and its contents changed frequently enough
that I didn't want to limit my users, I came up with this system.

mod_offload is inspired by [The Coral CDN](https://en.wikipedia.org/wiki/Coral_Content_Distribution_Network)
but I wanted something more lightweight and less complex. The Coral Cache
had to solve problems I could avoid, as can most web operators that aren't
trying to mirror the whole internet.

The gist of this project is that you install the Apache module on the base
server, and the C program (as a cgi-bin or standalone daemon), or a legacy PHP
script if all else fails, on offload servers where it will handle every
request. The module on the base server will decide if a given web request
should be offloaded, and redirects the client to one of the offload servers.
The offload server gets a request from the redirected client and sends an
HTTP HEAD request to the base server, so it can decide if it needs to
(re)cache the file in question. If so, it pulls a copy from the base server
via HTTP and serves it to the client on-the-fly. If not, it feeds the client
from the cached copy. To the users on the base server adding and changing
files and the downloading client, this is all basically transparent, beyond
the base server suddenly being less loaded once the caches start owning
copies of large and generally-unchanged files.

For those without a web server at all, the C program can be built as a
standalone daemon that provides a very basic HTTP server.

The C program is designed to take extremely little memory (as a standalone
daemon, it takes about 120 kilobytes per request it serves), and can block
"download accelerator" programs that open multiple requests to the server.

Offload servers can block so-called "download accelerators"; at most, X
simultaneous connections from one IP address may download a given URL.
Any above that amount are denied. One IP address can download any amount
of files, and X connections on the "accelerated" URL will run at once.
Download accelerators, in modern times, usually aren't. Presumably these
are a relic of sites that throttled bandwidth per-connection...in such a
case, they were at best a way to personally game the system, but mostly,
they just turn out to be poor citizens of the Internet, as they tend to use
exponentially more server memory for long periods of time to send the same
file, without much increase in performance (actually, they require more
packets to send the same data). This method will let these people still
function without ruining the experience for everyone else.


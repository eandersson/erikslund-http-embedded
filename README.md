# erikslund-http

An embedded C++26 HTTP/1.1 and TLS 1.3 server library for the operator surfaces of Erikslund
services -- status page, `/metrics`, health probes -- in ten lines and one CMake target.

It is a **library**, not a daemon. There is no `main()` to deploy, no configuration file it insists
on, and no reverse proxy in the way. A service links `erikslund::http`, hands it a `Router`, and
gets a status page and a Prometheus endpoint on a port of its own.

Linux only, and by design: the reactor is `epoll` plus `eventfd`, the accept model is
`SO_REUSEPORT`, and the Unix-socket listener is `AF_UNIX`. None of the three has a portable
substitute worth the abstraction layer that would hide it.

## License

[MIT](LICENSE). Copyright (c) 2026 Erik Olof Gunnar Andersson.

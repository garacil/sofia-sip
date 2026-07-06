# Sofia-SIP

An RFC 3261 compliant SIP User-Agent library — the SIP stack behind the
[GABpbx](https://github.com/garacil/gabpbx) `chan_sofia` channel driver.

Sofia-SIP is an open-source SIP User-Agent library that serves as a building
block for SIP client and server software: VoIP, IM, presence, and other
real-time, person-to-person communication. It implements a complete SIP
transaction and dialog state machine (the NTA/NUA stack) on top of SDP
handling, SRTP/TLS media security, STUN, DNS and a portable transport layer,
and runs primarily on GNU/Linux. It is licensed under the GNU LGPL.

## Credits and lineage

Sofia-SIP was created at the **Nokia Research Center** by **Pekka Pessi**,
**Martti Mela** and **Kai Vehmanen**, together with their contributors (see
[`AUTHORS`](AUTHORS)), and released under the LGPL. All credit for the library
belongs to them.

This repository is a **maintained fork** used as the SIP stack for GABpbx. It
descends from the community-maintained
[freeswitch/sofia-sip](https://github.com/freeswitch/sofia-sip) tree (the
1.13.x line) and keeps that lineage — it does **not** claim original authorship.

The fork stays a **drop-in** for the upstream library: the LGPL license, the
public API and headers (installed under `sofia-sip-1.13`) and the library soname
(`libsofia-sip-ua.so.0.6.0`) are unchanged, so anything built against upstream
1.13.x links against this fork without changes. Only the reported package
version advances — releases here are **2.0.x**, read from the top-level
[`.version`](.version) file.

### What this fork changes

| Area | Change |
| --- | --- |
| **Toolchain** | Builds **warning-clean on GCC 14 and OpenSSL 3** (zero warnings) — includes the OpenSSL 3.0 API migration and `-Warray-parameter` fixes. |
| **Correctness** | Fixes in the SIP parser, transport, NUA, timers and tagged-argument handling — each **proposed back upstream** as an issue and patch. |
| **STUN** | Removed an overlapping / address-of-pointer copy that clobbered a resolved server address. |
| **Build** | **Optimized by default again** — a plain `./configure` builds at `-O2 -g` (upstream had silently dropped the optimization default); your own `CFLAGS` still override. |

The full history is in the [releases](https://github.com/garacil/sofia-sip/releases).

## Building

Sofia-SIP uses the GNU autotools:

```sh
./autogen.sh                       # regenerate ./configure (needed from a git checkout)
./configure --prefix=/usr/local
make
sudo make install
sudo ldconfig
```

A plain build is optimized (`-O2 -g`). To supply your own flags — which are then
honored verbatim:

```sh
./configure CFLAGS="-O3 -g"
```

See [`docs/devel_platform_notes.txt`](docs/devel_platform_notes.txt) for
platform-specific build notes, and [`README.developers`](README.developers) for
notes aimed at contributors.

### Bundled utilities

Small example clients ship under `libsofia-sip-ua/utils` and `libsofia-sip-ua/su`:

- **`sip-options`** — query a peer with the SIP `OPTIONS` method
- **`sip-date`** — SIP date printer / parser
- **`addrinfo`** — resolve host names
- **`localinfo`** — print information about local network interfaces

## Versioning

The release version lives in the top-level [`.version`](.version) file and is
read by `configure` (`AC_INIT`), so cutting a release is a one-line edit plus a
re-run of `./autogen.sh` and `./configure`. The API-series header directory
(`sofia-sip-1.13`) and the library soname (`libsofia-sip-ua.so.0.6.0`) are held
stable across the whole 2.0.x line, so `pkg-config --modversion sofia-sip-ua`
advances while consumers keep building unchanged.

## License

Sofia-SIP is licensed under the **GNU LGPL**; see [`COPYING`](COPYING) for the
full text. Copyright in the original work remains with Nokia Corporation and the
Sofia-SIP contributors; this fork's changes are contributed under the same
license.

## Links

- **This fork:** <https://github.com/garacil/sofia-sip>
- **Upstream:** <https://github.com/freeswitch/sofia-sip>
- **GABpbx** (the consumer): <https://github.com/garacil/gabpbx>

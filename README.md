# CPC I/O Stream TCP Bridge (C)

This Linux program forwards bytes between a TCP client and a Silicon Labs CPC
endpoint. It is intended for use with the CPC I/O Stream and CLI components on
the secondary. Run `cpcd` first. This application was ported from the [cpc_iostream_bridge.py](https://docs.silabs.com/gecko-platform/latest/cpc-iostream/md-docs-iostream-cpc-iostream-cpc-host) implementation delivered with [cpc-daemon](https://github.com/SiliconLabs/cpc-daemon).

## Build

Install the CPC daemon development files (including `sl_cpc.h` and the
`libcpc.pc` pkg-config file), then run:

```sh
make
```

The program links to the libcpc library from cpc-daemon.

## Run

```sh
./cpc_iostream_bridge -n cpcd_0 -p 8080 -v
telnet localhost 8080
```

Options are `-n/--name` (CPCd instance), `-p/--port` (TCP port), `-e/--endpoint` (endpoint ID, default 13), and `-v/--verbose`. The bridge accepts one TCP client at a time and returns to
listening after that client disconnects. It binds to all IPv4 interfaces, as the documented Python utility does; use firewall rules if remote access should be restricted.

The endpoint ID default is provided for the common CLI endpoint setup. If the
secondary firmware assigns its CLI stream a different endpoint, pass that ID
with `-e`.

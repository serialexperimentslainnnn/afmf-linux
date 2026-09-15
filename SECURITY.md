# Security policy

afmf-linux is a Vulkan implicit layer: once installed it is loaded into **every** Vulkan process
on the machine (it stays dormant unless `AFMF_ENABLE=1` is set, but the shared object is mapped).
A bug in it is therefore a bug in every game and application that uses Vulkan. Please report
anything that could be a memory-safety issue, a crash triggerable by application input, or an
unexpected way to make the layer do work.

## Reporting

Use GitHub's private vulnerability reporting on this repository
(**Security → Report a vulnerability**), or email `lain@digitalexperiments.dev`. Include the
version (`afmf-linux.json` carries it), the driver and Mesa version, and a way to reproduce.
You will get an answer within a week.

## Supported versions

Only the latest release receives fixes.

## What the layer does and does not do

- It reads only environment variables prefixed `AFMF_` (and `DISABLE_AFMF`), and only at first use.
- It writes files only when `AFMF_DUMP_DIR` is set, and only into that directory.
- It opens no sockets, spawns no processes and loads no other libraries beyond the Vulkan loader
  chain it sits in.
- It never elevates privileges; the request for a high-priority GPU queue is made through the
  Vulkan API and is refused by the kernel for unprivileged processes, which the layer accepts.

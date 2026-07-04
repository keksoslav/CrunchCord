# Security Policy

## Reporting a vulnerability

Please report security issues privately through GitHub's
[private vulnerability reporting](https://github.com/keksoslav/CrunchCord/security/advisories/new)
rather than opening a public issue. I will respond as soon as I can.

CrunchCord is a local desktop app with no network service. It shells out to
FFmpeg to process files you choose. If you find a way that a crafted input file
could compromise the host beyond FFmpeg's own behavior, that is worth reporting.

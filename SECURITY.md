# Security policy

phedav runs inside phones and talks to servers that hold a person's contacts and calendar. It parses untrusted XML and HTTP, so parser bugs matter: the unit tests include mutation (fuzz) runs under AddressSanitizer and UBSan.

**Report a vulnerability privately**, not in a public issue: use GitHub's private vulnerability reporting ("Security" tab, "Report a vulnerability") on this repository, or write to the maintainers
at the address in the repository profile. Include what you found, how to reproduce it and which version or commit. We aim to answer within a week and to fix serious issues before they are disclosed.

Supported: the latest release and the default branch.

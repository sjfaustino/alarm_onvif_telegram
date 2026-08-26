#pragma once

// Root CA used to verify api.telegram.org's TLS certificate, replacing the
// previous client.setInsecure() (which accepted ANY certificate - a MITM
// on the same network, or a compromised router upstream, could otherwise
// intercept the bot token and all photo/message traffic).
//
// This is NOT secret - root CA certificates are public by design - so
// unlike secrets.h, this file is meant to be committed.
//
// api.telegram.org's chain is:
//   api.telegram.org -> Go Daddy Secure Certificate Authority - G2 ->
//   Go Daddy Root Certificate Authority - G2 (this is that root - self-
//   signed, valid until 2037).
//
// Root CAs are typically valid for 10-25+ years, so this shouldn't need
// touching often - but if Telegram sends start failing with a TLS handshake
// error after working fine for a while (check the serial log for
// "Could not connect to api.telegram.org"), a CA rotation on their end is
// the first thing to check, and this is the file to refresh:
//
//   Linux/macOS terminal:
//     openssl s_client -connect api.telegram.org:443 -showcerts </dev/null 2>/dev/null \
//       | awk '/-----BEGIN CERTIFICATE-----/{c++} {print > ("cert" c ".pem")}'
//     Take the LAST cert file printed (the root, at the end of the chain)
//     and paste its contents below.
//
//   Browser:
//     Visit https://api.telegram.org -> click the padlock -> Certificate ->
//     view the full chain -> select the TOP (root) entry -> export/copy as
//     PEM -> paste below.
static const char* TELEGRAM_ROOT_CA = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDxTCCAq2gAwIBAgIBADANBgkqhkiG9w0BAQsFADCBgzELMAkGA1UEBhMCVVMx
EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxGjAYBgNVBAoT
EUdvRGFkZHkuY29tLCBJbmMuMTEwLwYDVQQDEyhHbyBEYWRkeSBSb290IENlcnRp
ZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTA5MDkwMTAwMDAwMFoXDTM3MTIzMTIz
NTk1OVowgYMxCzAJBgNVBAYTAlVTMRAwDgYDVQQIEwdBcml6b25hMRMwEQYDVQQH
EwpTY290dHNkYWxlMRowGAYDVQQKExFHb0RhZGR5LmNvbSwgSW5jLjExMC8GA1UE
AxMoR28gRGFkZHkgUm9vdCBDZXJ0aWZpY2F0ZSBBdXRob3JpdHkgLSBHMjCCASIw
DQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAL9xYgjx+lk09xvJGKP3gElY6SKD
E6bFIEMBO4Tx5oVJnyfq9oQbTqC023CYxzIBsQU+B07u9PpPL1kwIuerGVZr4oAH
/PMWdYA5UXvl+TW2dE6pjYIT5LY/qQOD+qK+ihVqf94Lw7YZFAXK6sOoBJQ7Rnwy
DfMAZiLIjWltNowRGLfTshxgtDj6AozO091GB94KPutdfMh8+7ArU6SSYmlRJQVh
GkSBjCypQ5Yj36w6gZoOKcUcqeldHraenjAKOc7xiID7S13MMuyFYkMlNAJWJwGR
tDtwKj9useiciAF9n9T521NtYJ2/LOdYq7hfRvzOxBsDPAnrSTFcaUaz4EcCAwEA
AaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYE
FDqahQcQZyi27/a9BUFuIMGU2g/eMA0GCSqGSIb3DQEBCwUAA4IBAQCZ21151fmX
WWcDYfF+OwYxdS2hII5PZYe096acvNjpL9DbWu7PdIxztDhC2gV7+AJ1uP2lsdeu
9tfeE8tTEH6KRtGX+rcuKxGrkLAngPnon1rpN5+r5N9ss4UXnT3ZJE95kTXWXwTr
gIOrmgIttRD02JDHBHNA7XIloKmf7J6raBKZV8aPEjoJpL1E/QYVN8Gb5DKj7Tjo
2GTzLH4U/ALqn83/B2gX2yKQOC16jdFU8WnjXzPKej17CuPKf1855eJ1usV2GDPO
LPAvTK33sefOT6jEm0pUBsV/fdUID+Ic/n4XuKxe9tQWskMJDE32p2u0mYRlynqI
4uJEvlz36hz1
-----END CERTIFICATE-----
)EOF";

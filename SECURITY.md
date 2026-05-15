# Security Policy

## Supported Versions

The library is in active development; only the latest version is supported.

## Reporting a Vulnerability

Please report **any** security issue you find in the library. You can do so by
[opening a new issue](https://github.com/ziv/raytiles/issues/new) and marking it as a security issue. We will respond
to your report as soon as possible and work with you to resolve it.

## Security Best Practices

The library allows bypassing TLS certificate validation, but doing so is not recommended in production environments. If
you need to bypass TLS certificate validation, please make sure to do so only in development environments — never in
production.

By default, the library validates the TLS certificate of the server. To bypass certificate validation, set the
`allow_insecure_tls` option to `true` when creating the `raytiles::pool_config`.